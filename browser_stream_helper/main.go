package main

import (
	"context"
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/sha256"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/base64"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"log"
	"math/big"
	"net"
	"net/http"
	"os"
	"strings"
	"sync"
	"time"

	"github.com/quic-go/quic-go/http3"
	webtransport "github.com/quic-go/webtransport-go"
)

const maxIPCFrameSize = 16 * 1024 * 1024
const maxControlFrameSize = 1024 * 1024

type readyInfo struct {
	Status     string `json:"status"`
	Addr       string `json:"addr"`
	CertSHA256 string `json:"cert_sha256"`
}

type ipcMessage struct {
	IPCToken     string          `json:"ipc_token"`
	Type         string          `json:"type"`
	SessionToken string          `json:"session_token,omitempty"`
	Payload      string          `json:"payload_b64,omitempty"`
	Payloads     []string        `json:"payloads_b64,omitempty"`
	Events       json.RawMessage `json:"events,omitempty"`
}

type controlStreamMessage struct {
	Type   string          `json:"type"`
	Events json.RawMessage `json:"events,omitempty"`
}

type sessionRegistry struct {
	mu       sync.Mutex
	tokens   map[string]struct{}
	sessions map[*webtransport.Session]string
}

func newSessionRegistry() *sessionRegistry {
	return &sessionRegistry{
		tokens:   make(map[string]struct{}),
		sessions: make(map[*webtransport.Session]string),
	}
}

func (r *sessionRegistry) register(token string) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.tokens[token] = struct{}{}
}

func (r *sessionRegistry) stop(token string) bool {
	r.mu.Lock()
	defer r.mu.Unlock()
	_, existed := r.tokens[token]
	delete(r.tokens, token)
	for sess, sessionToken := range r.sessions {
		if sessionToken == token {
			_ = sess.CloseWithError(0, "session stopped")
			delete(r.sessions, sess)
			existed = true
		}
	}
	return existed
}

func (r *sessionRegistry) has(token string) bool {
	r.mu.Lock()
	defer r.mu.Unlock()
	_, ok := r.tokens[token]
	return ok
}

func (r *sessionRegistry) consume(token string) bool {
	r.mu.Lock()
	defer r.mu.Unlock()
	if _, ok := r.tokens[token]; !ok {
		return false
	}
	delete(r.tokens, token)
	return true
}

func (r *sessionRegistry) attach(token string, sess *webtransport.Session) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.sessions[sess] = token
}

func (r *sessionRegistry) detach(sess *webtransport.Session) {
	r.mu.Lock()
	defer r.mu.Unlock()
	delete(r.sessions, sess)
}

func (r *sessionRegistry) sendDatagram(token string, payload []byte) int {
	r.mu.Lock()
	targets := make([]*webtransport.Session, 0, len(r.sessions))
	for sess, sessionToken := range r.sessions {
		if token == "" || token == sessionToken {
			targets = append(targets, sess)
		}
	}
	r.mu.Unlock()

	sent := 0
	for _, sess := range targets {
		if err := sess.SendDatagram(payload); err == nil {
			sent++
		}
	}
	return sent
}

func tokenLabel(token string) string {
	if len(token) <= 8 {
		return token
	}
	return token[:8]
}

func addCertificateHost(dnsNames map[string]struct{}, ipAddresses map[string]net.IP, host string) {
	host = strings.TrimSpace(host)
	if host == "" {
		return
	}

	if splitHost, _, err := net.SplitHostPort(host); err == nil {
		host = splitHost
	}
	host = strings.Trim(host, "[]")
	if host == "" {
		return
	}

	if ip := net.ParseIP(host); ip != nil {
		ipAddresses[ip.String()] = ip
		return
	}

	dnsNames[strings.ToLower(host)] = struct{}{}
}

func localCertificateHosts() []string {
	hosts := []string{"localhost", "127.0.0.1", "::1"}
	if hostname, err := os.Hostname(); err == nil {
		hostname = strings.TrimSpace(hostname)
		if hostname != "" {
			hosts = append(hosts, hostname)
			shortName := strings.SplitN(hostname, ".", 2)[0]
			if shortName != "" {
				hosts = append(hosts, shortName, shortName+".lan", shortName+".local")
			}
		}
	}

	ifaces, err := net.Interfaces()
	if err != nil {
		return hosts
	}
	for _, iface := range ifaces {
		if iface.Flags&net.FlagUp == 0 {
			continue
		}
		addrs, err := iface.Addrs()
		if err != nil {
			continue
		}
		for _, addr := range addrs {
			var ip net.IP
			switch value := addr.(type) {
			case *net.IPNet:
				ip = value.IP
			case *net.IPAddr:
				ip = value.IP
			}
			if ip == nil {
				continue
			}
			hosts = append(hosts, ip.String())
		}
	}
	return hosts
}

func generateCertificate(now time.Time, extraHosts []string) (tls.Certificate, []byte, time.Time, error) {
	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		return tls.Certificate{}, nil, time.Time{}, err
	}

	notAfter := now.Add(13 * 24 * time.Hour)
	serialLimit := new(big.Int).Lsh(big.NewInt(1), 128)
	serial, err := rand.Int(rand.Reader, serialLimit)
	if err != nil {
		return tls.Certificate{}, nil, time.Time{}, err
	}

	dnsNames := make(map[string]struct{})
	ipAddresses := make(map[string]net.IP)
	for _, host := range localCertificateHosts() {
		addCertificateHost(dnsNames, ipAddresses, host)
	}
	for _, host := range extraHosts {
		addCertificateHost(dnsNames, ipAddresses, host)
	}

	certDNSNames := make([]string, 0, len(dnsNames))
	for host := range dnsNames {
		certDNSNames = append(certDNSNames, host)
	}
	certIPAddresses := make([]net.IP, 0, len(ipAddresses))
	for _, ip := range ipAddresses {
		certIPAddresses = append(certIPAddresses, ip)
	}

	template := &x509.Certificate{
		SerialNumber: serial,
		Subject: pkix.Name{
			CommonName: "Polaris Browser Stream",
		},
		NotBefore:             now.Add(-1 * time.Minute),
		NotAfter:              notAfter,
		KeyUsage:              x509.KeyUsageDigitalSignature,
		ExtKeyUsage:           []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
		BasicConstraintsValid: true,
		DNSNames:              certDNSNames,
		IPAddresses:           certIPAddresses,
	}

	der, err := x509.CreateCertificate(rand.Reader, template, template, &key.PublicKey, key)
	if err != nil {
		return tls.Certificate{}, nil, time.Time{}, err
	}

	cert := tls.Certificate{
		Certificate: [][]byte{der},
		PrivateKey:  key,
		Leaf:        template,
	}
	return cert, der, notAfter, nil
}

func readIPCMessage(conn net.Conn) (ipcMessage, error) {
	var frameLen uint32
	if err := binary.Read(conn, binary.BigEndian, &frameLen); err != nil {
		return ipcMessage{}, err
	}
	if frameLen == 0 || frameLen > maxIPCFrameSize {
		return ipcMessage{}, fmt.Errorf("invalid IPC frame length %d", frameLen)
	}

	payload := make([]byte, frameLen)
	if _, err := io.ReadFull(conn, payload); err != nil {
		return ipcMessage{}, err
	}

	var msg ipcMessage
	if err := json.Unmarshal(payload, &msg); err != nil {
		return ipcMessage{}, err
	}
	return msg, nil
}

func writeIPCResponse(conn net.Conn, status string, errText string) {
	payload, _ := json.Marshal(map[string]string{
		"status": status,
		"error":  errText,
	})
	_ = binary.Write(conn, binary.BigEndian, uint32(len(payload)))
	_, _ = conn.Write(payload)
}

func writeIPCMessage(conn net.Conn, msg any) error {
	payload, err := json.Marshal(msg)
	if err != nil {
		return err
	}
	if len(payload) > maxIPCFrameSize {
		return fmt.Errorf("IPC payload is too large: %d", len(payload))
	}
	if err := binary.Write(conn, binary.BigEndian, uint32(len(payload))); err != nil {
		return err
	}
	_, err = conn.Write(payload)
	return err
}

func readIPCResponse(conn net.Conn) (map[string]string, error) {
	var frameLen uint32
	if err := binary.Read(conn, binary.BigEndian, &frameLen); err != nil {
		return nil, err
	}
	if frameLen == 0 || frameLen > maxIPCFrameSize {
		return nil, fmt.Errorf("invalid IPC response frame length %d", frameLen)
	}

	payload := make([]byte, frameLen)
	if _, err := io.ReadFull(conn, payload); err != nil {
		return nil, err
	}

	var response map[string]string
	if err := json.Unmarshal(payload, &response); err != nil {
		return nil, err
	}
	return response, nil
}

func readControlFrame(r io.Reader) ([]byte, error) {
	var frameLen uint32
	if err := binary.Read(r, binary.BigEndian, &frameLen); err != nil {
		return nil, err
	}
	if frameLen == 0 || frameLen > maxControlFrameSize {
		return nil, fmt.Errorf("invalid control frame length %d", frameLen)
	}

	payload := make([]byte, frameLen)
	if _, err := io.ReadFull(r, payload); err != nil {
		return nil, err
	}
	return payload, nil
}

func forwardInputEvents(inputIPCSocket string, inputIPCToken string, sessionToken string, events json.RawMessage) error {
	if inputIPCSocket == "" {
		return errors.New("input IPC socket is not configured")
	}
	if len(events) == 0 {
		return errors.New("input events payload is empty")
	}

	conn, err := net.Dial("unix", inputIPCSocket)
	if err != nil {
		return err
	}
	defer conn.Close()

	if err := writeIPCMessage(conn, ipcMessage{
		IPCToken:     inputIPCToken,
		Type:         "input_events",
		SessionToken: sessionToken,
		Events:       events,
	}); err != nil {
		return err
	}

	response, err := readIPCResponse(conn)
	if err != nil {
		return err
	}
	if response["status"] != "ok" {
		if response["error"] == "" {
			return errors.New("Polaris input IPC rejected the message")
		}
		return errors.New(response["error"])
	}
	return nil
}

func notifyTransportClosed(inputIPCSocket string, inputIPCToken string, sessionToken string) error {
	if inputIPCSocket == "" {
		return errors.New("input IPC socket is not configured")
	}

	conn, err := net.Dial("unix", inputIPCSocket)
	if err != nil {
		return err
	}
	defer conn.Close()

	if err := writeIPCMessage(conn, ipcMessage{
		IPCToken:     inputIPCToken,
		Type:         "transport_closed",
		SessionToken: sessionToken,
	}); err != nil {
		return err
	}

	response, err := readIPCResponse(conn)
	if err != nil {
		return err
	}
	if response["status"] != "ok" {
		if response["error"] == "" {
			return errors.New("Polaris rejected the transport close notification")
		}
		return errors.New(response["error"])
	}
	return nil
}

func handleIPCConn(conn net.Conn, ipcToken string, registry *sessionRegistry) {
	defer conn.Close()

	for {
		msg, err := readIPCMessage(conn)
		if err != nil {
			if !errors.Is(err, io.EOF) {
				writeIPCResponse(conn, "error", err.Error())
			}
			return
		}
		if msg.IPCToken != ipcToken {
			writeIPCResponse(conn, "error", "invalid IPC token")
			return
		}

		switch msg.Type {
		case "register_session":
			if msg.SessionToken == "" {
				writeIPCResponse(conn, "error", "missing session token")
				continue
			}
			registry.register(msg.SessionToken)
			log.Printf("registered session token=%s", tokenLabel(msg.SessionToken))
			writeIPCResponse(conn, "ok", "")
		case "stop_session":
			stopped := registry.stop(msg.SessionToken)
			log.Printf("stopped session token=%s existed=%t", tokenLabel(msg.SessionToken), stopped)
			writeIPCResponse(conn, "ok", "")
		case "media_datagram":
			payload, err := base64.StdEncoding.DecodeString(msg.Payload)
			if err != nil {
				writeIPCResponse(conn, "error", "invalid media payload")
				continue
			}
			registry.sendDatagram(msg.SessionToken, payload)
			writeIPCResponse(conn, "ok", "")
		case "media_datagrams":
			valid := true
			for _, encodedPayload := range msg.Payloads {
				payload, err := base64.StdEncoding.DecodeString(encodedPayload)
				if err != nil {
					valid = false
					break
				}
				registry.sendDatagram(msg.SessionToken, payload)
			}
			if !valid {
				writeIPCResponse(conn, "error", "invalid media payload")
				continue
			}
			writeIPCResponse(conn, "ok", "")
		default:
			writeIPCResponse(conn, "error", "unknown message type")
		}
	}
}

func serveIPC(ctx context.Context, path string, ipcToken string, registry *sessionRegistry) error {
	if path == "" {
		return nil
	}

	_ = os.Remove(path)
	ln, err := net.Listen("unix", path)
	if err != nil {
		return err
	}
	if err := os.Chmod(path, 0o600); err != nil {
		_ = ln.Close()
		return err
	}

	go func() {
		<-ctx.Done()
		_ = ln.Close()
		_ = os.Remove(path)
	}()

	go func() {
		for {
			conn, err := ln.Accept()
			if err != nil {
				return
			}
			go handleIPCConn(conn, ipcToken, registry)
		}
	}()
	return nil
}

func handleControlStream(token string, stream io.ReadWriteCloser, inputIPCSocket string, inputIPCToken string) {
	defer stream.Close()

	for {
		payload, err := readControlFrame(stream)
		if err != nil {
			if !errors.Is(err, io.EOF) {
				log.Printf("control stream read failed: %v", err)
			}
			return
		}

		var msg controlStreamMessage
		if err := json.Unmarshal(payload, &msg); err != nil {
			log.Printf("control stream payload rejected: %v", err)
			continue
		}

		switch msg.Type {
		case "input_events":
			if err := forwardInputEvents(inputIPCSocket, inputIPCToken, token, msg.Events); err != nil {
				log.Printf("input IPC forward failed: %v", err)
			}
		default:
			log.Printf("control stream ignored unknown message type %q", msg.Type)
		}
	}
}

func handleWebTransportSession(ctx context.Context, token string, sess *webtransport.Session, registry *sessionRegistry, inputIPCSocket string, inputIPCToken string) {
	registry.attach(token, sess)
	log.Printf("webtransport session attached token=%s", tokenLabel(token))
	defer func() {
		registry.detach(sess)
		log.Printf("webtransport session detached token=%s", tokenLabel(token))
		if err := notifyTransportClosed(inputIPCSocket, inputIPCToken, token); err != nil {
			log.Printf("transport close notification failed token=%s: %v", tokenLabel(token), err)
		}
	}()

	go func() {
		for {
			stream, err := sess.AcceptStream(ctx)
			if err != nil {
				return
			}
			go func() {
				handleControlStream(token, stream, inputIPCSocket, inputIPCToken)
			}()
		}
	}()

	<-sess.Context().Done()
}

func main() {
	addr := flag.String("addr", ":47992", "UDP HTTPS/WebTransport bind address")
	hosts := flag.String("hosts", "", "Comma-separated hostnames or IP addresses clients may use for WebTransport")
	ipcSocket := flag.String("ipc-socket", "", "Unix socket path for Polaris IPC")
	ipcToken := flag.String("ipc-token", "", "Random per-process IPC token")
	inputIPCSocket := flag.String("input-ipc-socket", "", "Unix socket path for Browser Stream input IPC")
	inputIPCToken := flag.String("input-ipc-token", "", "Random per-process Browser Stream input IPC token")
	certHashFile := flag.String("cert-hash-file", "", "Optional path to write the server certificate SHA-256 hex digest")
	flag.Parse()

	var extraHosts []string
	if *hosts != "" {
		extraHosts = strings.Split(*hosts, ",")
	}

	cert, der, _, err := generateCertificate(time.Now(), extraHosts)
	if err != nil {
		log.Fatalf("certificate generation failed: %v", err)
	}
	certHash := sha256.Sum256(der)
	certHashHex := hex.EncodeToString(certHash[:])

	if *certHashFile != "" {
		if err := os.WriteFile(*certHashFile, []byte(certHashHex), 0o600); err != nil {
			log.Fatalf("writing cert hash file failed: %v", err)
		}
	}

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	registry := newSessionRegistry()
	if err := serveIPC(ctx, *ipcSocket, *ipcToken, registry); err != nil {
		log.Fatalf("IPC setup failed: %v", err)
	}

	mux := http.NewServeMux()
	server := webtransport.Server{
		H3: &http3.Server{
			Addr:    *addr,
			Handler: mux,
			TLSConfig: http3.ConfigureTLSConfig(&tls.Config{
				Certificates: []tls.Certificate{cert},
			}),
		},
		CheckOrigin: func(r *http.Request) bool {
			return true
		},
	}
	webtransport.ConfigureHTTP3Server(server.H3)
	defer server.Close()

	mux.HandleFunc("/status", func(w http.ResponseWriter, _ *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		_ = json.NewEncoder(w).Encode(readyInfo{
			Status:     "ok",
			Addr:       *addr,
			CertSHA256: certHashHex,
		})
	})

	mux.HandleFunc("/browser-stream", func(w http.ResponseWriter, r *http.Request) {
		token := r.URL.Query().Get("token")
		if !registry.has(token) {
			log.Printf("webtransport rejected remote=%s token=%s reason=invalid-or-used", r.RemoteAddr, tokenLabel(token))
			http.Error(w, "invalid or already used Browser Stream token", http.StatusForbidden)
			return
		}

		log.Printf("webtransport upgrade requested remote=%s token=%s", r.RemoteAddr, tokenLabel(token))
		sess, err := server.Upgrade(w, r)
		if err != nil {
			log.Printf("webtransport upgrade failed: %v", err)
			return
		}
		if !registry.consume(token) {
			log.Printf("webtransport rejected after upgrade remote=%s token=%s reason=invalid-or-used", r.RemoteAddr, tokenLabel(token))
			_ = sess.CloseWithError(0, "invalid or already used Browser Stream token")
			return
		}
		log.Printf("webtransport upgrade accepted remote=%s token=%s", r.RemoteAddr, tokenLabel(token))
		go handleWebTransportSession(ctx, token, sess, registry, *inputIPCSocket, *inputIPCToken)
	})

	log.Printf("webtransport helper ready addr=%s hosts=%s cert_sha256=%s", *addr, *hosts, certHashHex)
	_ = json.NewEncoder(os.Stdout).Encode(readyInfo{
		Status:     "ready",
		Addr:       *addr,
		CertSHA256: certHashHex,
	})

	if err := server.ListenAndServe(); err != nil && !errors.Is(err, http.ErrServerClosed) {
		log.Fatalf("WebTransport server failed: %v", err)
	}
}
