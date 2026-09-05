//go:build linux

package main

import (
	"context"
	"crypto/rand"
	"errors"
	"fmt"
	"net"
	"os"
	"path/filepath"
	"sync"
	"syscall"
	"time"
)

const (
	handshakeTimeout = 2 * time.Second
	idleTimeout      = 30 * time.Second
	unixPathLimit    = 107
	maxConnections   = 4
)

type workerServer struct {
	config           workerConfig
	paths            workerPaths
	uid              uint32
	capability       [capabilitySize]byte
	control          *net.UnixListener
	media            *net.UnixListener
	controlID        fileIdentity
	mediaID          fileIdentity
	readyID          fileIdentity
	cancel           context.CancelFunc
	closeOnce        sync.Once
	connectionsMutex sync.Mutex
	connections      map[*net.UnixConn]struct{}
	closing          bool
}

func listenPrivateUnix(path string, expectedUID uint32) (*net.UnixListener, fileIdentity, error) {
	if len(path) == 0 || len(path) > unixPathLimit {
		return nil, fileIdentity{}, errors.New("worker IPC socket path is too long")
	}
	if err := removeOwnedSocket(path, expectedUID); err != nil {
		return nil, fileIdentity{}, err
	}
	listener, err := net.ListenUnix("unix", &net.UnixAddr{Name: path, Net: "unix"})
	if err != nil {
		return nil, fileIdentity{}, errors.New("worker IPC socket cannot listen")
	}
	if err := os.Chmod(path, 0o600); err != nil {
		_ = listener.Close()
		_ = os.Remove(path)
		return nil, fileIdentity{}, errors.New("worker IPC socket mode cannot be set")
	}
	identity, err := identityOf(path)
	if err != nil {
		_ = listener.Close()
		_ = os.Remove(path)
		return nil, fileIdentity{}, errors.New("worker IPC socket identity cannot be read")
	}
	return listener, identity, nil
}

func peerUID(connection *net.UnixConn) (uint32, error) {
	raw, err := connection.SyscallConn()
	if err != nil {
		return 0, errors.New("worker IPC peer credentials are unavailable")
	}
	var credentials *syscall.Ucred
	var socketError error
	if err := raw.Control(func(descriptor uintptr) {
		credentials, socketError = syscall.GetsockoptUcred(
			int(descriptor),
			syscall.SOL_SOCKET,
			syscall.SO_PEERCRED,
		)
	}); err != nil || socketError != nil || credentials == nil {
		return 0, errors.New("worker IPC peer credentials cannot be read")
	}
	return credentials.Uid, nil
}

func (server *workerServer) authenticate(connection *net.UnixConn, selectedChannel channel) error {
	if err := connection.SetDeadline(time.Now().Add(handshakeTimeout)); err != nil {
		return errors.New("worker IPC handshake deadline cannot be set")
	}
	uid, err := peerUID(connection)
	if err != nil || uid != server.uid {
		return errors.New("worker IPC peer identity was rejected")
	}
	var challenge [challengeSize]byte
	if _, err := rand.Read(challenge[:]); err != nil {
		return errors.New("worker IPC challenge cannot be generated")
	}
	if err := writeFrame(connection, frame{
		Channel:    selectedChannel,
		Message:    messageChallenge,
		Slot:       server.config.Identity.Slot,
		Generation: server.config.Identity.Generation,
		Sequence:   1,
		Payload:    challenge[:],
	}); err != nil {
		return err
	}

	presented, err := readFrame(
		connection,
		selectedChannel,
		server.config.Identity.Slot,
		server.config.Identity.Generation,
	)
	if err != nil || presented.Message != messageAuthenticate || presented.Sequence != 1 {
		return errors.New("worker IPC authentication frame was rejected")
	}
	expected, err := authenticationProof(
		server.capability,
		proofController,
		selectedChannel,
		server.config.Identity,
		challenge,
	)
	if err != nil || !verifyProof(expected, presented.Payload) {
		return errors.New("worker IPC authentication proof was rejected")
	}
	response, err := authenticationProof(
		server.capability,
		proofWorker,
		selectedChannel,
		server.config.Identity,
		challenge,
	)
	if err != nil {
		return errors.New("worker IPC response proof cannot be generated")
	}
	if err := writeFrame(connection, frame{
		Channel:    selectedChannel,
		Message:    messageAuthenticated,
		Slot:       server.config.Identity.Slot,
		Generation: server.config.Identity.Generation,
		Sequence:   2,
		Payload:    response[:],
	}); err != nil {
		return err
	}
	return connection.SetDeadline(time.Now().Add(idleTimeout))
}

func (server *workerServer) serveConnection(
	connection *net.UnixConn,
	selectedChannel channel,
) {
	server.connectionsMutex.Lock()
	if server.closing {
		server.connectionsMutex.Unlock()
		_ = connection.Close()
		return
	}
	server.connections[connection] = struct{}{}
	server.connectionsMutex.Unlock()
	defer func() {
		server.connectionsMutex.Lock()
		delete(server.connections, connection)
		server.connectionsMutex.Unlock()
		_ = connection.Close()
	}()
	if err := server.authenticate(connection, selectedChannel); err != nil {
		return
	}
	incoming := newSequenceGuard()
	if !incoming.accept(1) {
		return
	}
	// Authentication consumed controller sequence 1.
	outgoingSequence := uint64(3)
	for {
		value, err := readFrame(
			connection,
			selectedChannel,
			server.config.Identity.Slot,
			server.config.Identity.Generation,
		)
		if err != nil || !incoming.accept(value.Sequence) {
			return
		}
		if err := connection.SetDeadline(time.Now().Add(idleTimeout)); err != nil {
			return
		}
		switch value.Message {
		case messageHeartbeat:
			if err := writeFrame(connection, frame{
				Channel:    selectedChannel,
				Message:    messageHeartbeatAck,
				Slot:       server.config.Identity.Slot,
				Generation: server.config.Identity.Generation,
				Sequence:   outgoingSequence,
			}); err != nil {
				return
			}
			outgoingSequence++
		case messageShutdown:
			if selectedChannel != channelControl {
				return
			}
			_ = writeFrame(connection, frame{
				Channel:    channelControl,
				Message:    messageShutdownAck,
				Slot:       server.config.Identity.Slot,
				Generation: server.config.Identity.Generation,
				Sequence:   outgoingSequence,
			})
			server.cancel()
			return
		default:
			// Launch, input, feedback, and media routing are intentionally not
			// enabled by this supervisor-only slice.
			return
		}
	}
}

func (server *workerServer) acceptLoop(
	context context.Context,
	listener *net.UnixListener,
	selectedChannel channel,
	errors chan<- error,
) {
	permits := make(chan struct{}, maxConnections)
	for {
		connection, err := listener.AcceptUnix()
		if err != nil {
			select {
			case <-context.Done():
				return
			default:
				errors <- fmt.Errorf("worker %d IPC listener failed", selectedChannel)
				return
			}
		}
		select {
		case permits <- struct{}{}:
			go func(accepted *net.UnixConn) {
				defer func() { <-permits }()
				server.serveConnection(accepted, selectedChannel)
			}(connection)
		default:
			_ = connection.Close()
		}
	}
}

func (server *workerServer) close() {
	server.closeOnce.Do(func() {
		server.cancel()
		server.connectionsMutex.Lock()
		server.closing = true
		server.connectionsMutex.Unlock()
		if server.control != nil {
			_ = server.control.Close()
		}
		if server.media != nil {
			_ = server.media.Close()
		}
		server.connectionsMutex.Lock()
		for connection := range server.connections {
			_ = connection.Close()
		}
		server.connectionsMutex.Unlock()
		removeExact(filepath.Join(server.paths.IPC, controlSocketName), server.controlID)
		removeExact(filepath.Join(server.paths.IPC, mediaSocketName), server.mediaID)
		removeExact(filepath.Join(server.paths.State, readyFileName), server.readyID)
	})
}

func probeWorkerSocket(
	path string,
	config workerConfig,
	capability [capabilitySize]byte,
	selectedChannel channel,
	expectedUID uint32,
) error {
	connection, err := net.DialUnix(
		"unix",
		nil,
		&net.UnixAddr{Name: path, Net: "unix"},
	)
	if err != nil {
		return errors.New("worker IPC health connection failed")
	}
	defer connection.Close()
	if err := connection.SetDeadline(time.Now().Add(handshakeTimeout)); err != nil {
		return errors.New("worker IPC health deadline cannot be set")
	}
	uid, err := peerUID(connection)
	if err != nil || uid != expectedUID {
		return errors.New("worker IPC health peer was rejected")
	}
	challengeFrame, err := readFrame(
		connection,
		selectedChannel,
		config.Identity.Slot,
		config.Identity.Generation,
	)
	if err != nil || challengeFrame.Message != messageChallenge || challengeFrame.Sequence != 1 {
		return errors.New("worker IPC health challenge was rejected")
	}
	var challenge [challengeSize]byte
	copy(challenge[:], challengeFrame.Payload)
	controllerProof, err := authenticationProof(
		capability,
		proofController,
		selectedChannel,
		config.Identity,
		challenge,
	)
	if err != nil {
		return errors.New("worker IPC health proof cannot be generated")
	}
	if err := writeFrame(connection, frame{
		Channel:    selectedChannel,
		Message:    messageAuthenticate,
		Slot:       config.Identity.Slot,
		Generation: config.Identity.Generation,
		Sequence:   1,
		Payload:    controllerProof[:],
	}); err != nil {
		return err
	}
	authenticated, err := readFrame(
		connection,
		selectedChannel,
		config.Identity.Slot,
		config.Identity.Generation,
	)
	if err != nil || authenticated.Message != messageAuthenticated || authenticated.Sequence != 2 {
		return errors.New("worker IPC health authentication was rejected")
	}
	workerProof, err := authenticationProof(
		capability,
		proofWorker,
		selectedChannel,
		config.Identity,
		challenge,
	)
	if err != nil || !verifyProof(workerProof, authenticated.Payload) {
		return errors.New("worker IPC health response proof was rejected")
	}
	if err := writeFrame(connection, frame{
		Channel:    selectedChannel,
		Message:    messageHeartbeat,
		Slot:       config.Identity.Slot,
		Generation: config.Identity.Generation,
		Sequence:   2,
	}); err != nil {
		return err
	}
	heartbeat, err := readFrame(
		connection,
		selectedChannel,
		config.Identity.Slot,
		config.Identity.Generation,
	)
	if err != nil || heartbeat.Message != messageHeartbeatAck || heartbeat.Sequence != 3 {
		return errors.New("worker IPC health heartbeat was rejected")
	}
	return nil
}

func newWorkerServer(
	parent context.Context,
	config workerConfig,
	paths workerPaths,
	expectedUID uint32,
) (*workerServer, context.Context, error) {
	if err := privateDirectory(paths.IPC, expectedUID); err != nil {
		return nil, nil, err
	}
	if err := privateDirectory(paths.Auth, expectedUID); err != nil {
		return nil, nil, err
	}
	if err := privateDirectory(paths.State, expectedUID); err != nil {
		return nil, nil, err
	}
	capability, err := readCapability(
		filepath.Join(paths.Auth, capabilityFileName),
		expectedUID,
	)
	if err != nil {
		return nil, nil, err
	}
	workerContext, cancel := context.WithCancel(parent)
	server := &workerServer{
		config:      config,
		paths:       paths,
		uid:         expectedUID,
		capability:  capability,
		cancel:      cancel,
		connections: make(map[*net.UnixConn]struct{}),
	}
	server.control, server.controlID, err = listenPrivateUnix(
		filepath.Join(paths.IPC, controlSocketName),
		expectedUID,
	)
	if err != nil {
		server.close()
		return nil, nil, err
	}
	server.media, server.mediaID, err = listenPrivateUnix(
		filepath.Join(paths.IPC, mediaSocketName),
		expectedUID,
	)
	if err != nil {
		server.close()
		return nil, nil, err
	}
	server.readyID, err = writeReadyState(
		filepath.Join(paths.State, readyFileName),
		config,
		expectedUID,
	)
	if err != nil {
		server.close()
		return nil, nil, err
	}
	return server, workerContext, nil
}

func runWorker(
	parent context.Context,
	config workerConfig,
	paths workerPaths,
	expectedUID uint32,
) error {
	return runWorkerWithRuntime(
		parent,
		config,
		paths,
		expectedUID,
		nil,
		runtimeOptions{},
	)
}

// runWorkerWithRuntime keeps runtime activation injection-only. The production
// entrypoint calls runWorker above with no adapters, so this slice cannot touch
// a session bus, audio server, compositor, input device, GPU, or launcher.
func runWorkerWithRuntime(
	parent context.Context,
	config workerConfig,
	paths workerPaths,
	expectedUID uint32,
	adapters *runtimeAdapters,
	options runtimeOptions,
) (returnError error) {
	var managedRuntime *workerRuntime
	if adapters != nil {
		var err error
		managedRuntime, err = startWorkerRuntime(parent, config, *adapters, options)
		if err != nil {
			return err
		}
		defer func() {
			returnError = errors.Join(returnError, managedRuntime.Stop())
		}()
	}

	server, workerContext, err := newWorkerServer(parent, config, paths, expectedUID)
	if err != nil {
		return err
	}
	defer server.close()
	serverErrors := make(chan error, 2)
	go server.acceptLoop(workerContext, server.control, channelControl, serverErrors)
	go server.acceptLoop(workerContext, server.media, channelMedia, serverErrors)
	var runtimeFailures <-chan error
	if managedRuntime != nil {
		runtimeFailures = managedRuntime.Failures()
	}
	select {
	case <-workerContext.Done():
		return nil
	case err := <-serverErrors:
		return err
	case err := <-runtimeFailures:
		return err
	}
}
