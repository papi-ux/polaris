//go:build linux

package main

import (
	"context"
	"crypto/hmac"
	"encoding/hex"
	"errors"
	"net"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"
	"time"
)

type testWorker struct {
	config workerConfig
	paths  workerPaths
	cancel context.CancelFunc
	done   chan error
}

func prepareTestWorkerPaths(t *testing.T, name string) workerPaths {
	t.Helper()
	root := filepath.Join(t.TempDir(), name)
	ipc := filepath.Join(root, "ipc")
	auth := filepath.Join(root, "auth")
	state := filepath.Join(root, "state")
	if err := os.MkdirAll(ipc, 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(state, 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(auth, 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.Chmod(ipc, 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.Chmod(state, 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.Chmod(auth, 0o700); err != nil {
		t.Fatal(err)
	}
	capability := goldenCapability()
	encoded := hex.EncodeToString(capability[:]) + "\n"
	if err := os.WriteFile(filepath.Join(auth, capabilityFileName), []byte(encoded), 0o600); err != nil {
		t.Fatal(err)
	}
	return workerPaths{IPC: ipc, Auth: auth, State: state}
}

func createTestWorker(t *testing.T, name string, generation uint64, slot uint32) testWorker {
	t.Helper()
	paths := prepareTestWorkerPaths(t, name)
	config := workerConfig{
		Identity: endpointIdentity{
			ControllerEpoch: "controller-a1b2",
			LogicalGPU:      "gpu-primary",
			Slot:            slot,
			Generation:      generation,
			WorkerName:      name,
		},
		RuntimeNamespace: "runtime-" + name,
		WaylandSocket:    "wayland-" + name,
		AudioSink:        "audio-" + name,
		InputSeat:        "input-" + name,
		RenderNode:       "/dev/dri/renderD128",
		Compositor:       "gamescope",
		EncoderSessions:  1,
		WorkloadKey:      "synthetic",
	}
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan error, 1)
	go func() {
		done <- runWorker(ctx, config, paths, uint32(os.Geteuid()))
		close(done)
	}()
	worker := testWorker{
		config: config,
		paths:  paths,
		cancel: cancel,
		done:   done,
	}
	deadline := time.Now().Add(2 * time.Second)
	for {
		if err := checkHealth(worker.config, worker.paths, uint32(os.Geteuid())); err == nil {
			break
		}
		if time.Now().After(deadline) {
			cancel()
			t.Fatal("worker did not become healthy")
		}
		time.Sleep(5 * time.Millisecond)
	}
	t.Cleanup(func() {
		cancel()
		select {
		case <-done:
		case <-time.After(2 * time.Second):
			t.Error("worker did not stop during cleanup")
		}
	})
	return worker
}

type testConnection struct {
	connection *net.UnixConn
	challenge  [challengeSize]byte
	worker     testWorker
	channel    channel
	incoming   uint64
	outgoing   uint64
}

func connectAndAuthenticate(t *testing.T, worker testWorker, selectedChannel channel) *testConnection {
	t.Helper()
	name := controlSocketName
	if selectedChannel == channelMedia {
		name = mediaSocketName
	}
	connection, err := net.DialUnix(
		"unix",
		nil,
		&net.UnixAddr{Name: filepath.Join(worker.paths.IPC, name), Net: "unix"},
	)
	if err != nil {
		t.Fatal(err)
	}
	challengeFrame, err := readFrame(
		connection,
		selectedChannel,
		worker.config.Identity.Slot,
		worker.config.Identity.Generation,
	)
	if err != nil || challengeFrame.Message != messageChallenge || challengeFrame.Sequence != 1 {
		connection.Close()
		t.Fatalf("invalid challenge: %+v, %v", challengeFrame, err)
	}
	var challenge [challengeSize]byte
	copy(challenge[:], challengeFrame.Payload)
	controllerProof, err := authenticationProof(
		goldenCapability(),
		proofController,
		selectedChannel,
		worker.config.Identity,
		challenge,
	)
	if err != nil {
		connection.Close()
		t.Fatal(err)
	}
	if err := writeFrame(connection, frame{
		Channel: selectedChannel, Message: messageAuthenticate,
		Slot: worker.config.Identity.Slot, Generation: worker.config.Identity.Generation,
		Sequence: 1, Payload: controllerProof[:],
	}); err != nil {
		connection.Close()
		t.Fatal(err)
	}
	authenticated, err := readFrame(
		connection,
		selectedChannel,
		worker.config.Identity.Slot,
		worker.config.Identity.Generation,
	)
	if err != nil || authenticated.Message != messageAuthenticated || authenticated.Sequence != 2 {
		connection.Close()
		t.Fatalf("invalid authenticated response: %+v, %v", authenticated, err)
	}
	workerProof, err := authenticationProof(
		goldenCapability(),
		proofWorker,
		selectedChannel,
		worker.config.Identity,
		challenge,
	)
	if err != nil || !hmac.Equal(workerProof[:], authenticated.Payload) {
		connection.Close()
		t.Fatal("worker proof was invalid")
	}
	return &testConnection{
		connection: connection,
		challenge:  challenge,
		worker:     worker,
		channel:    selectedChannel,
		incoming:   3,
		outgoing:   2,
	}
}

func (client *testConnection) heartbeat(t *testing.T) {
	t.Helper()
	if err := writeFrame(client.connection, frame{
		Channel: client.channel, Message: messageHeartbeat,
		Slot:       client.worker.config.Identity.Slot,
		Generation: client.worker.config.Identity.Generation,
		Sequence:   client.outgoing,
	}); err != nil {
		t.Fatal(err)
	}
	client.outgoing++
	response, err := readFrame(
		client.connection,
		client.channel,
		client.worker.config.Identity.Slot,
		client.worker.config.Identity.Generation,
	)
	if err != nil || response.Message != messageHeartbeatAck || response.Sequence != client.incoming {
		t.Fatalf("invalid heartbeat response: %+v, %v", response, err)
	}
	client.incoming++
}

func TestTwoWorkersAuthenticateBothChannelsAndStopIndependently(t *testing.T) {
	first := createTestWorker(t, "worker-a", 41, 0)
	second := createTestWorker(t, "worker-b", 42, 1)
	firstControl := connectAndAuthenticate(t, first, channelControl)
	defer firstControl.connection.Close()
	secondControl := connectAndAuthenticate(t, second, channelControl)
	defer secondControl.connection.Close()
	firstMedia := connectAndAuthenticate(t, first, channelMedia)
	defer firstMedia.connection.Close()
	secondMedia := connectAndAuthenticate(t, second, channelMedia)
	defer secondMedia.connection.Close()

	firstControl.heartbeat(t)
	firstMedia.heartbeat(t)
	secondControl.heartbeat(t)
	secondMedia.heartbeat(t)
	if err := writeFrame(firstControl.connection, frame{
		Channel: channelControl, Message: messageShutdown,
		Slot:       first.config.Identity.Slot,
		Generation: first.config.Identity.Generation,
		Sequence:   firstControl.outgoing,
	}); err != nil {
		t.Fatal(err)
	}
	response, err := readFrame(
		firstControl.connection,
		channelControl,
		first.config.Identity.Slot,
		first.config.Identity.Generation,
	)
	if err != nil || response.Message != messageShutdownAck || response.Sequence != firstControl.incoming {
		t.Fatalf("invalid shutdown response: %+v, %v", response, err)
	}
	select {
	case err := <-first.done:
		if err != nil {
			t.Fatal(err)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("first worker did not stop")
	}
	if err := checkHealth(first.config, first.paths, uint32(os.Geteuid())); err == nil {
		t.Fatal("stopped worker remained healthy")
	}
	if err := checkHealth(second.config, second.paths, uint32(os.Geteuid())); err != nil {
		t.Fatalf("stopping the first worker affected the second: %v", err)
	}
	secondControl.heartbeat(t)
}

func TestBadProofAndReplayFailClosedWithoutStoppingWorker(t *testing.T) {
	worker := createTestWorker(t, "worker-proof", 43, 0)
	connection, err := net.DialUnix(
		"unix",
		nil,
		&net.UnixAddr{Name: filepath.Join(worker.paths.IPC, controlSocketName), Net: "unix"},
	)
	if err != nil {
		t.Fatal(err)
	}
	challengeFrame, err := readFrame(connection, channelControl, 0, 43)
	if err != nil {
		t.Fatal(err)
	}
	badProof := make([]byte, proofSize)
	if err := writeFrame(connection, frame{
		Channel: channelControl, Message: messageAuthenticate,
		Slot: 0, Generation: 43, Sequence: 1, Payload: badProof,
	}); err != nil {
		t.Fatal(err)
	}
	_ = connection.SetReadDeadline(time.Now().Add(500 * time.Millisecond))
	if _, err := readFrame(connection, channelControl, 0, 43); err == nil {
		t.Fatal("bad proof received an authenticated response")
	}
	connection.Close()
	if len(challengeFrame.Payload) != challengeSize {
		t.Fatal("challenge was malformed")
	}

	valid := connectAndAuthenticate(t, worker, channelControl)
	defer valid.connection.Close()
	valid.heartbeat(t)
	if err := writeFrame(valid.connection, frame{
		Channel: channelControl, Message: messageHeartbeat,
		Slot: 0, Generation: 43, Sequence: valid.outgoing - 1,
	}); err != nil {
		t.Fatal(err)
	}
	_ = valid.connection.SetReadDeadline(time.Now().Add(500 * time.Millisecond))
	if _, err := readFrame(valid.connection, channelControl, 0, 43); err == nil {
		t.Fatal("replayed sequence received a response")
	}
	if err := checkHealth(worker.config, worker.paths, uint32(os.Geteuid())); err != nil {
		t.Fatalf("rejected connection stopped worker: %v", err)
	}
}

func TestPrivateCapabilityRejectsBroadModeAndSymlink(t *testing.T) {
	worker := createTestWorker(t, "worker-private", 44, 0)
	capabilityPath := filepath.Join(worker.paths.Auth, capabilityFileName)
	if err := os.Chmod(capabilityPath, 0o644); err != nil {
		t.Fatal(err)
	}
	if _, err := readCapability(capabilityPath, uint32(os.Geteuid())); err == nil {
		t.Fatal("group-readable capability was accepted")
	}
	if err := os.Chmod(capabilityPath, 0o600); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(capabilityPath, []byte(strings.Repeat("f", capabilitySize*2)), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := checkHealth(worker.config, worker.paths, uint32(os.Geteuid())); err == nil {
		t.Fatal("health accepted a capability that did not authenticate the live worker")
	}
	capability := goldenCapability()
	if err := os.WriteFile(
		capabilityPath,
		[]byte(hex.EncodeToString(capability[:])+"\n"),
		0o600,
	); err != nil {
		t.Fatal(err)
	}
	link := filepath.Join(worker.paths.Auth, "linked-token")
	if err := os.Symlink(capabilityPath, link); err != nil {
		t.Fatal(err)
	}
	if _, err := readCapability(link, uint32(os.Geteuid())); err == nil {
		t.Fatal("symlinked capability was accepted")
	}
}

func TestSecondWorkerCannotReplaceAnActiveSocket(t *testing.T) {
	worker := createTestWorker(t, "worker-exclusive", 46, 0)
	server, _, err := newWorkerServer(
		context.Background(),
		worker.config,
		worker.paths,
		uint32(os.Geteuid()),
	)
	if err == nil {
		server.close()
		t.Fatal("second worker replaced an active socket")
	}
	if err := checkHealth(worker.config, worker.paths, uint32(os.Geteuid())); err != nil {
		t.Fatalf("failed duplicate start damaged the active worker: %v", err)
	}
}

func TestCancellationReturnsCleanly(t *testing.T) {
	worker := createTestWorker(t, "worker-cancel", 45, 0)
	worker.cancel()
	select {
	case err := <-worker.done:
		if err != nil && !errors.Is(err, context.Canceled) {
			t.Fatal(err)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("cancelled worker did not return")
	}
}

func TestInjectedRuntimeMustBeReadyBeforeWorkerPublishesHealth(t *testing.T) {
	paths := prepareTestWorkerPaths(t, "r")
	config := runtimeTestConfig("worker-runtime-ready", 61, 0)
	runtimeSet := newFakeRuntimeSet()
	releaseLauncher := make(chan struct{})
	runtimeSet.byStage[runtimeStageLauncherProcessTree].start = func(
		context.Context,
		runtimeAllocation,
	) (runtimeLease, error) {
		<-releaseLauncher
		return runtimeSet.leases[runtimeStageLauncherProcessTree], nil
	}
	ctx, cancel := context.WithCancel(context.Background())
	t.Cleanup(cancel)
	done := make(chan error, 1)
	go func() {
		done <- runWorkerWithRuntime(
			ctx,
			config,
			paths,
			uint32(os.Geteuid()),
			&runtimeSet.adapters,
			runtimeTestOptions(),
		)
	}()
	waitForRuntimeEvent(t, runtimeSet.recorder, "start:launcher-process-tree")
	if _, err := os.Lstat(filepath.Join(paths.State, readyFileName)); !os.IsNotExist(err) {
		t.Fatalf("worker published readiness before runtime completion: %v", err)
	}
	close(releaseLauncher)
	deadline := time.Now().Add(2 * time.Second)
	for {
		if err := checkHealth(config, paths, uint32(os.Geteuid())); err == nil {
			break
		}
		select {
		case err := <-done:
			t.Fatalf("worker exited before publishing health: %v", err)
		default:
		}
		if time.Now().After(deadline) {
			cancel()
			t.Fatal("worker did not publish health after runtime became ready")
		}
		time.Sleep(5 * time.Millisecond)
	}
	cancel()
	select {
	case err := <-done:
		if err != nil {
			t.Fatal(err)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("worker did not stop after cancellation")
	}
	events, _ := runtimeSet.recorder.snapshot()
	if !reflect.DeepEqual(events, completeRuntimeEvents()) {
		t.Fatalf("ready-gated lifecycle mismatch: %#v", events)
	}
}

func TestInjectedRuntimeFailureClosesWorkerAndTearsDown(t *testing.T) {
	paths := prepareTestWorkerPaths(t, "f")
	config := runtimeTestConfig("worker-runtime-failure", 62, 0)
	runtimeSet := newFakeRuntimeSet()
	done := make(chan error, 1)
	go func() {
		done <- runWorkerWithRuntime(
			context.Background(),
			config,
			paths,
			uint32(os.Geteuid()),
			&runtimeSet.adapters,
			runtimeTestOptions(),
		)
	}()
	deadline := time.Now().Add(2 * time.Second)
	for {
		if err := checkHealth(config, paths, uint32(os.Geteuid())); err == nil {
			break
		}
		select {
		case err := <-done:
			t.Fatalf("worker exited before becoming healthy: %v", err)
		default:
		}
		if time.Now().After(deadline) {
			t.Fatal("worker did not become healthy")
		}
		time.Sleep(5 * time.Millisecond)
	}
	runtimeSet.leases[runtimeStageCapture].finish(errors.New("private adapter detail"))
	select {
	case err := <-done:
		if err == nil || !strings.Contains(err.Error(), "capture exited unexpectedly") {
			t.Fatalf("unexpected worker runtime failure: %v", err)
		}
		if strings.Contains(err.Error(), "private adapter") {
			t.Fatalf("adapter detail escaped the worker boundary: %v", err)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("worker did not stop after runtime failure")
	}
	if err := checkHealth(config, paths, uint32(os.Geteuid())); err == nil {
		t.Fatal("worker remained healthy after runtime failure")
	}
	events, _ := runtimeSet.recorder.snapshot()
	if !reflect.DeepEqual(events, completeRuntimeEvents()) {
		t.Fatalf("failure teardown order mismatch: %#v", events)
	}
}
