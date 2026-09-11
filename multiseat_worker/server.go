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
	handshakeTimeout      = 2 * time.Second
	idleTimeout           = 30 * time.Second
	dataPlaneRouteTimeout = 5 * time.Second
	unixPathLimit         = 107
	maxConnections        = 4
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
	context          context.Context
	cancel           context.CancelFunc
	dataPlane        workerDataPlane
	failures         chan error
	closeOnce        sync.Once
	connectionsMutex sync.Mutex
	connections      map[*net.UnixConn]struct{}
	attached         map[channel]*net.UnixConn
	closing          bool
}

type connectionFrameWriter struct {
	mutex      sync.Mutex
	connection *net.UnixConn
	channel    channel
	identity   endpointIdentity
	sequence   uint64
}

func (writer *connectionFrameWriter) send(selectedMessage message, payload []byte) error {
	writer.mutex.Lock()
	defer writer.mutex.Unlock()
	if writer.sequence == 0 {
		return errors.New("worker IPC outgoing sequence is exhausted")
	}
	if err := writer.connection.SetWriteDeadline(time.Now().Add(idleTimeout)); err != nil {
		return errors.New("worker IPC write deadline cannot be set")
	}
	if err := writeFrame(writer.connection, frame{
		Channel:    writer.channel,
		Message:    selectedMessage,
		Slot:       writer.identity.Slot,
		Generation: writer.identity.Generation,
		Sequence:   writer.sequence,
		Payload:    payload,
	}); err != nil {
		return err
	}
	if writer.sequence == ^uint64(0) {
		writer.sequence = 0
	} else {
		writer.sequence++
	}
	return nil
}

func (server *workerServer) claimDataPlane(
	connection *net.UnixConn,
	selectedChannel channel,
) bool {
	server.connectionsMutex.Lock()
	defer server.connectionsMutex.Unlock()
	if server.closing || server.attached[selectedChannel] != nil {
		return false
	}
	server.attached[selectedChannel] = connection
	return true
}

func (server *workerServer) releaseDataPlane(
	connection *net.UnixConn,
	selectedChannel channel,
) {
	server.connectionsMutex.Lock()
	defer server.connectionsMutex.Unlock()
	if server.attached[selectedChannel] == connection {
		delete(server.attached, selectedChannel)
	}
}

func (server *workerServer) failDataPlane(route string) {
	select {
	case server.failures <- fmt.Errorf("worker data plane %s route failed", route):
	default:
	}
	server.cancel()
}

func (server *workerServer) pumpDataPlane(
	connectionContext context.Context,
	connection *net.UnixConn,
	writer *connectionFrameWriter,
) {
	for {
		var output routedOutput
		var err error
		if writer.channel == channelControl {
			output, err = server.dataPlane.NextFeedback(connectionContext)
		} else {
			output, err = server.dataPlane.NextMedia(connectionContext)
		}
		if err != nil {
			if connectionContext.Err() == nil && server.context.Err() == nil {
				server.failDataPlane(writer.channel.String())
			}
			_ = connection.Close()
			return
		}
		if !validRoutedOutput(output, server.config.Identity, writer.channel) {
			server.failDataPlane(writer.channel.String())
			_ = connection.Close()
			return
		}
		payload := append([]byte(nil), output.Payload...)
		if err := writer.send(output.Message, payload); err != nil {
			_ = connection.Close()
			return
		}
		if output.Message == messageEndOfStream {
			return
		}
	}
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
	connectionContext, cancelConnection := context.WithCancel(server.context)
	attached := false
	defer func() {
		cancelConnection()
		if attached {
			server.releaseDataPlane(connection, selectedChannel)
			server.cancel()
		}
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
	writer := &connectionFrameWriter{
		connection: connection,
		channel:    selectedChannel,
		identity:   server.config.Identity,
		sequence:   3,
	}
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
		if err := connection.SetReadDeadline(time.Now().Add(idleTimeout)); err != nil {
			return
		}
		switch value.Message {
		case messageHeartbeat:
			if err := writer.send(messageHeartbeatAck, nil); err != nil {
				return
			}
		case messageAttach:
			if attached || nilRuntimeInterface(server.dataPlane) ||
				!server.claimDataPlane(connection, selectedChannel) {
				return
			}
			attached = true
			if err := writer.send(messageAttached, nil); err != nil {
				return
			}
			go server.pumpDataPlane(
				connectionContext,
				connection,
				writer,
			)
		case messageInput:
			if selectedChannel != channelControl || !attached ||
				nilRuntimeInterface(server.dataPlane) {
				return
			}
			routeContext, cancelRoute := context.WithTimeout(
				connectionContext,
				dataPlaneRouteTimeout,
			)
			err := server.dataPlane.RouteInput(routeContext, routedInput{
				Identity: server.config.Identity,
				Payload:  append([]byte(nil), value.Payload...),
			})
			cancelRoute()
			if err != nil {
				server.failDataPlane("input")
				return
			}
			if err := writer.send(messageInputAck, nil); err != nil {
				return
			}
		case messageMediaConfigAck, messageRequestIDR, messageInvalidateReferenceFrames:
			// The contract messages reach the encoder through the same gate as
			// input: the exact control channel of an attached data plane.
			if selectedChannel != channelControl || !attached ||
				nilRuntimeInterface(server.dataPlane) {
				return
			}
			control := routedMediaControl{
				Identity: server.config.Identity,
				Message:  value.Message,
			}
			if value.Message == messageInvalidateReferenceFrames {
				span, err := parseFrameRange(value.Payload)
				if err != nil {
					return
				}
				control.Range = span
			}
			routeContext, cancelRoute := context.WithTimeout(
				connectionContext,
				dataPlaneRouteTimeout,
			)
			err := server.dataPlane.RouteMediaControl(routeContext, control)
			cancelRoute()
			if err != nil {
				server.failDataPlane("media control")
				return
			}
			if err := writer.send(messageMediaControlAck, nil); err != nil {
				return
			}
		case messageShutdown:
			if selectedChannel != channelControl {
				return
			}
			_ = writer.send(messageShutdownAck, nil)
			server.cancel()
			return
		default:
			// Directionally invalid or unsupported messages fail the exact
			// connection closed. They never reach another seat's adapter.
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
	parent context.Context,
	path string,
	config workerConfig,
	capability [capabilitySize]byte,
	selectedChannel channel,
	expectedUID uint32,
) error {
	ctx, cancel := context.WithTimeout(parent, handshakeTimeout)
	defer cancel()
	raw, err := (&net.Dialer{}).DialContext(ctx, "unix", path)
	if err != nil {
		return errors.New("worker IPC health connection failed")
	}
	defer raw.Close()
	connection, ok := raw.(*net.UnixConn)
	if !ok {
		return errors.New("worker IPC health connection type invalid")
	}
	deadline, _ := ctx.Deadline()
	if err := connection.SetDeadline(deadline); err != nil {
		return errors.New("worker IPC health deadline cannot be set")
	}
	// Arm after the normal deadline: cancellation cannot be overwritten by it.
	stopCancellation := context.AfterFunc(ctx, func() { _ = connection.SetDeadline(time.Now()) })
	defer stopCancellation()
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
	return ctx.Err()
}

func newWorkerServer(
	parent context.Context,
	config workerConfig,
	paths workerPaths,
	expectedUID uint32,
) (*workerServer, context.Context, error) {
	return newWorkerServerWithDataPlane(
		parent,
		config,
		paths,
		expectedUID,
		nil,
	)
}

func newWorkerServerWithDataPlane(
	parent context.Context,
	config workerConfig,
	paths workerPaths,
	expectedUID uint32,
	dataPlane workerDataPlane,
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
		context:     workerContext,
		cancel:      cancel,
		dataPlane:   dataPlane,
		failures:    make(chan error, 1),
		connections: make(map[*net.UnixConn]struct{}),
		attached:    make(map[channel]*net.UnixConn),
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
	return runWorkerWithRuntimeAndDataPlane(
		parent,
		config,
		paths,
		expectedUID,
		adapters,
		nil,
		options,
	)
}

// runWorkerWithRuntimeAndDataPlane is an injection-only executable contract.
// The production entrypoint supplies neither side, so no gameplay resource or
// route is activated by this checkpoint.
func runWorkerWithRuntimeAndDataPlane(
	parent context.Context,
	config workerConfig,
	paths workerPaths,
	expectedUID uint32,
	adapters *runtimeAdapters,
	dataPlane workerDataPlane,
	options runtimeOptions,
) (returnError error) {
	if adapters == nil && !nilRuntimeInterface(dataPlane) {
		return errors.New("worker data plane requires a managed runtime")
	}
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

	server, workerContext, err := newWorkerServerWithDataPlane(
		parent,
		config,
		paths,
		expectedUID,
		dataPlane,
	)
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
		select {
		case err := <-server.failures:
			return err
		default:
			return nil
		}
	case err := <-serverErrors:
		return err
	case err := <-server.failures:
		return err
	case err := <-runtimeFailures:
		return err
	}
}
