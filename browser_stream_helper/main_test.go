package main

import (
	"crypto/sha256"
	"crypto/x509"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"net"
	"os"
	"slices"
	"testing"
	"time"
)

func TestGenerateCertificateUsesP256AndShortLifetime(t *testing.T) {
	now := time.Date(2026, 5, 7, 12, 0, 0, 0, time.UTC)
	cert, der, notAfter, err := generateCertificate(now, nil)
	if err != nil {
		t.Fatalf("generateCertificate failed: %v", err)
	}
	if len(der) == 0 {
		t.Fatal("certificate DER is empty")
	}
	if cert.Leaf == nil {
		t.Fatal("certificate Leaf is nil")
	}
	if lifetime := notAfter.Sub(now); lifetime >= 14*24*time.Hour {
		t.Fatalf("certificate lifetime is too long: %s", lifetime)
	}
}

func TestCertificateHashIsSHA256Hex(t *testing.T) {
	_, der, _, err := generateCertificate(time.Now(), nil)
	if err != nil {
		t.Fatalf("generateCertificate failed: %v", err)
	}

	hash := sha256.Sum256(der)
	encoded := hex.EncodeToString(hash[:])

	if len(encoded) != 64 {
		t.Fatalf("expected 64 hex chars, got %d", len(encoded))
	}
}

func TestSessionTokenIsSingleUse(t *testing.T) {
	registry := newSessionRegistry()
	registry.register("token-1")

	if !registry.consume("token-1") {
		t.Fatal("registered token was rejected")
	}
	if registry.consume("token-1") {
		t.Fatal("token was accepted twice")
	}
}

func testSocketPath(t *testing.T, name string) string {
	t.Helper()
	path := fmt.Sprintf("/tmp/%s-%d.sock", name, time.Now().UnixNano())
	t.Cleanup(func() {
		_ = os.Remove(path)
	})
	return path
}

func TestForwardInputEventsWritesInputIPCFrame(t *testing.T) {
	socketPath := testSocketPath(t, "bs-input")
	ln, err := net.Listen("unix", socketPath)
	if err != nil {
		t.Fatalf("listen failed: %v", err)
	}
	defer ln.Close()

	done := make(chan ipcMessage, 1)
	go func() {
		conn, err := ln.Accept()
		if err != nil {
			t.Errorf("accept failed: %v", err)
			return
		}
		defer conn.Close()

		msg, err := readIPCMessage(conn)
		if err != nil {
			t.Errorf("readIPCMessage failed: %v", err)
			return
		}
		done <- msg
		writeIPCResponse(conn, "ok", "")
	}()

	events := json.RawMessage(`[{"type":"pointer","action":"move"}]`)
	if err := forwardInputEvents(socketPath, "ipc-token", "session-token", events); err != nil {
		t.Fatalf("forwardInputEvents failed: %v", err)
	}

	select {
	case msg := <-done:
		if msg.IPCToken != "ipc-token" {
			t.Fatalf("unexpected IPC token: %q", msg.IPCToken)
		}
		if msg.Type != "input_events" {
			t.Fatalf("unexpected message type: %q", msg.Type)
		}
		if msg.SessionToken != "session-token" {
			t.Fatalf("unexpected session token: %q", msg.SessionToken)
		}
		if string(msg.Events) != string(events) {
			t.Fatalf("unexpected events payload: %s", msg.Events)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("timed out waiting for IPC message")
	}
}

func TestForwardInputEventsReportsInputIPCRejection(t *testing.T) {
	socketPath := testSocketPath(t, "bs-reject")
	ln, err := net.Listen("unix", socketPath)
	if err != nil {
		t.Fatalf("listen failed: %v", err)
	}
	defer ln.Close()

	go func() {
		conn, err := ln.Accept()
		if err != nil {
			t.Errorf("accept failed: %v", err)
			return
		}
		defer conn.Close()
		if _, err := readIPCMessage(conn); err != nil {
			t.Errorf("readIPCMessage failed: %v", err)
			return
		}
		writeIPCResponse(conn, "error", "input token rejected")
	}()

	events := json.RawMessage(`[{"type":"pointer","action":"move"}]`)
	if err := forwardInputEvents(socketPath, "ipc-token", "bad-session-token", events); err == nil {
		t.Fatal("expected forwardInputEvents to report the IPC rejection")
	}
}

func TestReadControlFrameRejectsOversizePayload(t *testing.T) {
	reader, writer := net.Pipe()
	defer reader.Close()
	defer writer.Close()

	go func() {
		_ = binary.Write(writer, binary.BigEndian, uint32(maxControlFrameSize+1))
	}()

	if _, err := readControlFrame(reader); err == nil {
		t.Fatal("expected oversized control frame to be rejected")
	}
}

func TestGenerateCertificateIncludesExtraHosts(t *testing.T) {
	_, der, _, err := generateCertificate(time.Date(2026, 5, 7, 12, 0, 0, 0, time.UTC), []string{
		"pc-papi.lan",
		"10.0.0.232:47990",
		"[::1]:47990",
	})
	if err != nil {
		t.Fatalf("generateCertificate failed: %v", err)
	}

	cert, err := x509.ParseCertificate(der)
	if err != nil {
		t.Fatalf("ParseCertificate failed: %v", err)
	}

	if !slices.Contains(cert.DNSNames, "pc-papi.lan") {
		t.Fatalf("expected pc-papi.lan DNS SAN, got %v", cert.DNSNames)
	}
	if !slices.ContainsFunc(cert.IPAddresses, func(ip net.IP) bool {
		return ip.Equal(net.ParseIP("10.0.0.232"))
	}) {
		t.Fatalf("expected 10.0.0.232 IP SAN, got %v", cert.IPAddresses)
	}
	if !slices.ContainsFunc(cert.IPAddresses, func(ip net.IP) bool {
		return ip.Equal(net.ParseIP("::1"))
	}) {
		t.Fatalf("expected ::1 IP SAN, got %v", cert.IPAddresses)
	}
}
