//go:build linux

package main

import (
	"context"
	"encoding/hex"
	"errors"
	"os"
	"reflect"
	"strings"
	"testing"
	"time"
)

type fakeWorkerDataPlane struct {
	inputs            chan routedInput
	mediaControls     chan routedMediaControl
	feedback          chan routedOutput
	media             chan routedOutput
	routeInput        func(context.Context, routedInput) error
	routeMediaControl func(context.Context, routedMediaControl) error
}

func newFakeWorkerDataPlane() *fakeWorkerDataPlane {
	return &fakeWorkerDataPlane{
		inputs:        make(chan routedInput, 8),
		mediaControls: make(chan routedMediaControl, 8),
		feedback:      make(chan routedOutput, 8),
		media:         make(chan routedOutput, 8),
	}
}

func (plane *fakeWorkerDataPlane) RouteMediaControl(
	ctx context.Context,
	control routedMediaControl,
) error {
	if plane.routeMediaControl != nil {
		return plane.routeMediaControl(ctx, control)
	}
	select {
	case plane.mediaControls <- control:
		return nil
	case <-ctx.Done():
		return ctx.Err()
	}
}

func (plane *fakeWorkerDataPlane) RouteInput(
	ctx context.Context,
	input routedInput,
) error {
	if plane.routeInput != nil {
		return plane.routeInput(ctx, input)
	}
	select {
	case plane.inputs <- input:
		return nil
	case <-ctx.Done():
		return ctx.Err()
	}
}

func (plane *fakeWorkerDataPlane) NextFeedback(ctx context.Context) (routedOutput, error) {
	select {
	case output := <-plane.feedback:
		return output, nil
	case <-ctx.Done():
		return routedOutput{}, ctx.Err()
	}
}

func (plane *fakeWorkerDataPlane) NextMedia(ctx context.Context) (routedOutput, error) {
	select {
	case output := <-plane.media:
		return output, nil
	case <-ctx.Done():
		return routedOutput{}, ctx.Err()
	}
}

func createRoutedTestWorker(
	t *testing.T,
	name string,
	generation uint64,
	slot uint32,
	plane workerDataPlane,
) (testWorker, *fakeRuntimeSet) {
	t.Helper()
	root, err := os.MkdirTemp("", "psw-")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = os.RemoveAll(root) })
	paths := workerPaths{
		IPC:   root + "/ipc",
		Auth:  root + "/auth",
		State: root + "/state",
	}
	for _, path := range []string{paths.IPC, paths.Auth, paths.State} {
		if err := os.Mkdir(path, 0o700); err != nil {
			t.Fatal(err)
		}
	}
	capability := goldenCapability()
	if err := os.WriteFile(
		paths.Auth+"/"+capabilityFileName,
		[]byte(hex.EncodeToString(capability[:])+"\n"),
		0o600,
	); err != nil {
		t.Fatal(err)
	}
	config := runtimeTestConfig(name, generation, slot)
	runtimeSet := newFakeRuntimeSet()
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan error, 1)
	go func() {
		done <- runWorkerWithRuntimeAndDataPlane(
			ctx,
			config,
			paths,
			uint32(os.Geteuid()),
			&runtimeSet.adapters,
			plane,
			runtimeTestOptions(),
		)
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
		if err := checkHealth(config, paths, uint32(os.Geteuid())); err == nil {
			break
		}
		select {
		case err := <-done:
			t.Fatalf("routed worker exited before health: %v", err)
		default:
		}
		if time.Now().After(deadline) {
			cancel()
			t.Fatal("routed worker did not become healthy")
		}
		time.Sleep(5 * time.Millisecond)
	}
	t.Cleanup(func() {
		cancel()
		select {
		case <-done:
		case <-time.After(2 * time.Second):
			t.Error("routed worker did not stop during cleanup")
		}
	})
	return worker, runtimeSet
}

func (client *testConnection) attach(t *testing.T) {
	t.Helper()
	if err := writeFrame(client.connection, frame{
		Channel: client.channel, Message: messageAttach,
		Slot:       client.worker.config.Identity.Slot,
		Generation: client.worker.config.Identity.Generation,
		Sequence:   client.outgoing,
	}); err != nil {
		t.Fatal(err)
	}
	client.outgoing++
	response := client.read(t)
	if response.Message != messageAttached || len(response.Payload) != 0 {
		t.Fatalf("invalid data-plane attachment: %+v", response)
	}
}

func (client *testConnection) read(t *testing.T) frame {
	t.Helper()
	response, err := readFrame(
		client.connection,
		client.channel,
		client.worker.config.Identity.Slot,
		client.worker.config.Identity.Generation,
	)
	if err != nil || response.Sequence != client.incoming {
		t.Fatalf("invalid routed frame: %+v, %v", response, err)
	}
	client.incoming++
	return response
}

func (client *testConnection) input(t *testing.T, payload []byte) {
	t.Helper()
	if err := writeFrame(client.connection, frame{
		Channel: channelControl, Message: messageInput,
		Slot:       client.worker.config.Identity.Slot,
		Generation: client.worker.config.Identity.Generation,
		Sequence:   client.outgoing, Payload: payload,
	}); err != nil {
		t.Fatal(err)
	}
	client.outgoing++
	if response := client.read(t); response.Message != messageInputAck {
		t.Fatalf("input did not receive an exact acknowledgement: %+v", response)
	}
}

// mediaControl sends one controller-to-worker contract message and requires
// the worker's single acknowledgement for it.
func (client *testConnection) mediaControl(t *testing.T, kind message, payload []byte) {
	t.Helper()
	if err := writeFrame(client.connection, frame{
		Channel: channelControl, Message: kind,
		Slot:       client.worker.config.Identity.Slot,
		Generation: client.worker.config.Identity.Generation,
		Sequence:   client.outgoing, Payload: payload,
	}); err != nil {
		t.Fatal(err)
	}
	client.outgoing++
	if response := client.read(t); response.Message != messageMediaControlAck {
		t.Fatalf("media control did not receive an exact acknowledgement: %+v", response)
	}
}

func TestMediaContractIsAnnouncedOnMediaAndInstructedOnControl(t *testing.T) {
	plane := newFakeWorkerDataPlane()
	worker, _ := createRoutedTestWorker(t, "worker-contract", 91, 0, plane)
	control := connectAndAuthenticate(t, worker, channelControl)
	media := connectAndAuthenticate(t, worker, channelMedia)
	defer control.connection.Close()
	defer media.connection.Close()
	control.attach(t)
	media.attach(t)

	// The worker announces its contract on the channel it describes.
	config, err := encodeMediaConfig(goldenMediaConfig())
	if err != nil {
		t.Fatal(err)
	}
	plane.media <- routedOutput{
		Identity: worker.config.Identity,
		Message:  messageMediaConfig,
		Payload:  config,
	}
	announced := media.read(t)
	if announced.Message != messageMediaConfig {
		t.Fatalf("contract was not announced on the media channel: %+v", announced)
	}
	if decoded, err := parseMediaConfig(announced.Payload); err != nil || decoded != goldenMediaConfig() {
		t.Fatalf("announced contract did not survive the wire: %+v, %v", decoded, err)
	}

	span, err := encodeFrameRange(frameRange{First: 11, Last: 14})
	if err != nil {
		t.Fatal(err)
	}
	control.mediaControl(t, messageMediaConfigAck, nil)
	control.mediaControl(t, messageRequestIDR, nil)
	control.mediaControl(t, messageInvalidateReferenceFrames, span)

	for _, want := range []routedMediaControl{
		{Identity: worker.config.Identity, Message: messageMediaConfigAck},
		{Identity: worker.config.Identity, Message: messageRequestIDR},
		{Identity: worker.config.Identity, Message: messageInvalidateReferenceFrames, Range: frameRange{First: 11, Last: 14}},
	} {
		select {
		case got := <-plane.mediaControls:
			if got != want {
				t.Fatalf("media control reached the encoder changed: %+v, want %+v", got, want)
			}
		case <-time.After(2 * time.Second):
			t.Fatalf("media control never reached the encoder: %+v", want)
		}
	}
}

func TestMediaContractRejectsAMalformedSpanAndTheWrongChannel(t *testing.T) {
	plane := newFakeWorkerDataPlane()
	worker, _ := createRoutedTestWorker(t, "worker-contract-bad", 92, 0, plane)
	control := connectAndAuthenticate(t, worker, channelControl)
	media := connectAndAuthenticate(t, worker, channelMedia)
	defer media.connection.Close()
	control.attach(t)
	media.attach(t)

	// An inverted span is refused before it can reach the encoder, and the
	// exact connection fails closed rather than answering.
	inverted := make([]byte, frameRangeSize)
	inverted[7] = 9
	inverted[15] = 5
	if err := writeFrame(control.connection, frame{
		Channel: channelControl, Message: messageInvalidateReferenceFrames,
		Slot:       worker.config.Identity.Slot,
		Generation: worker.config.Identity.Generation,
		Sequence:   control.outgoing, Payload: inverted,
	}); err != nil {
		t.Fatal(err)
	}
	control.outgoing++
	select {
	case err := <-worker.done:
		if err != nil {
			t.Fatal(err)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("a malformed reference span did not fail its connection closed")
	}
	if len(plane.mediaControls) != 0 {
		t.Fatalf("a malformed reference span reached the encoder: %+v", <-plane.mediaControls)
	}
}

func TestTwoSeatDataPlanesRouteInputFeedbackAndEncodedMediaIndependently(t *testing.T) {
	firstPlane := newFakeWorkerDataPlane()
	secondPlane := newFakeWorkerDataPlane()
	first, firstRuntime := createRoutedTestWorker(t, "worker-route-a", 81, 0, firstPlane)
	second, secondRuntime := createRoutedTestWorker(t, "worker-route-b", 82, 1, secondPlane)

	firstControl := connectAndAuthenticate(t, first, channelControl)
	firstMedia := connectAndAuthenticate(t, first, channelMedia)
	secondControl := connectAndAuthenticate(t, second, channelControl)
	secondMedia := connectAndAuthenticate(t, second, channelMedia)
	defer firstMedia.connection.Close()
	defer secondControl.connection.Close()
	defer secondMedia.connection.Close()
	firstControl.attach(t)
	firstMedia.attach(t)
	secondControl.attach(t)
	secondMedia.attach(t)

	firstControl.input(t, []byte("input-a"))
	secondControl.input(t, []byte("input-b"))
	firstInput := <-firstPlane.inputs
	secondInput := <-secondPlane.inputs
	if firstInput.Identity != first.config.Identity || string(firstInput.Payload) != "input-a" ||
		secondInput.Identity != second.config.Identity || string(secondInput.Payload) != "input-b" {
		t.Fatalf("input crossed seat routes: %+v, %+v", firstInput, secondInput)
	}

	firstPlane.feedback <- routedOutput{
		Identity: first.config.Identity,
		Message:  messageFeedback,
		Payload:  []byte("feedback-a"),
	}
	secondPlane.feedback <- routedOutput{
		Identity: second.config.Identity,
		Message:  messageFeedback,
		Payload:  []byte("feedback-b"),
	}
	if got := firstControl.read(t); got.Message != messageFeedback || string(got.Payload) != "feedback-a" {
		t.Fatalf("first feedback was cross-routed: %+v", got)
	}
	if got := secondControl.read(t); got.Message != messageFeedback || string(got.Payload) != "feedback-b" {
		t.Fatalf("second feedback was cross-routed: %+v", got)
	}

	firstPlane.media <- routedOutput{
		Identity: first.config.Identity,
		Message:  messageVideo,
		Payload:  []byte("encoded-video-a"),
	}
	secondPlane.media <- routedOutput{
		Identity: second.config.Identity,
		Message:  messageAudio,
		Payload:  []byte("encoded-audio-b"),
	}
	if got := firstMedia.read(t); got.Message != messageVideo || string(got.Payload) != "encoded-video-a" {
		t.Fatalf("first media was cross-routed: %+v", got)
	}
	if got := secondMedia.read(t); got.Message != messageAudio || string(got.Payload) != "encoded-audio-b" {
		t.Fatalf("second media was cross-routed: %+v", got)
	}

	if err := firstControl.connection.Close(); err != nil {
		t.Fatal(err)
	}
	select {
	case err := <-first.done:
		if err != nil {
			t.Fatal(err)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("detached first data plane did not stop its worker")
	}
	secondControl.heartbeat(t)
	if events, _ := firstRuntime.recorder.snapshot(); !reflect.DeepEqual(events, completeRuntimeEvents()) {
		t.Fatalf("first route teardown order mismatch: %#v", events)
	}
	if events, _ := secondRuntime.recorder.snapshot(); containsStopEvent(events) {
		t.Fatalf("first route teardown touched second seat: %#v", events)
	}
}

func containsStopEvent(events []string) bool {
	for _, event := range events {
		if strings.HasPrefix(event, "stop:") {
			return true
		}
	}
	return false
}

func TestCrossSeatOutputFailsClosedAndRollsBackOnlyItsRuntime(t *testing.T) {
	firstPlane := newFakeWorkerDataPlane()
	secondPlane := newFakeWorkerDataPlane()
	first, firstRuntime := createRoutedTestWorker(t, "worker-cross-a", 83, 0, firstPlane)
	second, secondRuntime := createRoutedTestWorker(t, "worker-cross-b", 84, 1, secondPlane)
	firstControl := connectAndAuthenticate(t, first, channelControl)
	firstMedia := connectAndAuthenticate(t, first, channelMedia)
	secondControl := connectAndAuthenticate(t, second, channelControl)
	defer firstControl.connection.Close()
	defer firstMedia.connection.Close()
	defer secondControl.connection.Close()
	firstControl.attach(t)
	firstMedia.attach(t)

	firstPlane.media <- routedOutput{
		Identity: second.config.Identity,
		Message:  messageVideo,
		Payload:  []byte("wrong-seat"),
	}
	select {
	case err := <-first.done:
		if err == nil || !strings.Contains(err.Error(), "media route failed") {
			t.Fatalf("cross-seat output did not fail generically: %v", err)
		}
		if strings.Contains(err.Error(), "worker-cross-b") {
			t.Fatalf("cross-seat identity escaped the route boundary: %v", err)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("cross-seat output did not stop the exact worker")
	}
	secondControl.heartbeat(t)
	if events, _ := firstRuntime.recorder.snapshot(); !reflect.DeepEqual(events, completeRuntimeEvents()) {
		t.Fatalf("cross-route rollback order mismatch: %#v", events)
	}
	if events, _ := secondRuntime.recorder.snapshot(); containsStopEvent(events) {
		t.Fatalf("cross-route rollback touched the other runtime: %#v", events)
	}
}

func TestInputRouteFailureRollsBackTheCompleteSeat(t *testing.T) {
	plane := newFakeWorkerDataPlane()
	plane.routeInput = func(context.Context, routedInput) error {
		return errors.New("private virtual input detail")
	}
	worker, runtimeSet := createRoutedTestWorker(t, "worker-input-failure", 85, 0, plane)
	control := connectAndAuthenticate(t, worker, channelControl)
	media := connectAndAuthenticate(t, worker, channelMedia)
	defer control.connection.Close()
	defer media.connection.Close()
	control.attach(t)
	media.attach(t)
	if err := writeFrame(control.connection, frame{
		Channel: channelControl, Message: messageInput,
		Slot:       worker.config.Identity.Slot,
		Generation: worker.config.Identity.Generation,
		Sequence:   control.outgoing, Payload: []byte("input"),
	}); err != nil {
		t.Fatal(err)
	}
	select {
	case err := <-worker.done:
		if err == nil || !strings.Contains(err.Error(), "input route failed") ||
			strings.Contains(err.Error(), "private virtual input") {
			t.Fatalf("input failure was not bounded and redacted: %v", err)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("input route failure did not stop the worker")
	}
	if events, _ := runtimeSet.recorder.snapshot(); !reflect.DeepEqual(events, completeRuntimeEvents()) {
		t.Fatalf("input failure rollback order mismatch: %#v", events)
	}
}

func TestCrossSeatInputHeaderNeverReachesEitherSeatRoute(t *testing.T) {
	firstPlane := newFakeWorkerDataPlane()
	secondPlane := newFakeWorkerDataPlane()
	first, firstRuntime := createRoutedTestWorker(t, "worker-input-a", 86, 0, firstPlane)
	second, secondRuntime := createRoutedTestWorker(t, "worker-input-b", 87, 1, secondPlane)
	control := connectAndAuthenticate(t, first, channelControl)
	defer control.connection.Close()
	control.attach(t)

	if err := writeFrame(control.connection, frame{
		Channel: channelControl, Message: messageInput,
		Slot:       second.config.Identity.Slot,
		Generation: second.config.Identity.Generation,
		Sequence:   control.outgoing, Payload: []byte("cross-seat-input"),
	}); err != nil {
		t.Fatal(err)
	}
	_ = control.connection.SetReadDeadline(time.Now().Add(500 * time.Millisecond))
	if _, err := readFrame(
		control.connection,
		channelControl,
		first.config.Identity.Slot,
		first.config.Identity.Generation,
	); err == nil {
		t.Fatal("cross-seat input received an acknowledgement")
	}
	select {
	case input := <-firstPlane.inputs:
		t.Fatalf("cross-seat input reached first route: %+v", input)
	default:
	}
	select {
	case input := <-secondPlane.inputs:
		t.Fatalf("cross-seat input reached second route: %+v", input)
	default:
	}
	select {
	case err := <-first.done:
		if err != nil {
			t.Fatalf("untrusted route detail escaped worker boundary: %v", err)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("cross-seat input did not close its attached worker")
	}
	if err := checkHealth(second.config, second.paths, uint32(os.Geteuid())); err != nil {
		t.Fatalf("cross-seat input affected the target-looking seat: %v", err)
	}
	if events, _ := firstRuntime.recorder.snapshot(); !reflect.DeepEqual(events, completeRuntimeEvents()) {
		t.Fatalf("cross-input rollback order mismatch: %#v", events)
	}
	if events, _ := secondRuntime.recorder.snapshot(); containsStopEvent(events) {
		t.Fatalf("cross-input rollback touched the other runtime: %#v", events)
	}
}

func TestSecondAttachmentCannotStealAnActiveSeatChannel(t *testing.T) {
	plane := newFakeWorkerDataPlane()
	worker, _ := createRoutedTestWorker(t, "worker-attach", 88, 0, plane)
	owner := connectAndAuthenticate(t, worker, channelControl)
	contender := connectAndAuthenticate(t, worker, channelControl)
	defer owner.connection.Close()
	defer contender.connection.Close()
	owner.attach(t)
	if err := writeFrame(contender.connection, frame{
		Channel: channelControl, Message: messageAttach,
		Slot:       worker.config.Identity.Slot,
		Generation: worker.config.Identity.Generation,
		Sequence:   contender.outgoing,
	}); err != nil {
		t.Fatal(err)
	}
	_ = contender.connection.SetReadDeadline(time.Now().Add(500 * time.Millisecond))
	if _, err := readFrame(
		contender.connection,
		channelControl,
		worker.config.Identity.Slot,
		worker.config.Identity.Generation,
	); err == nil {
		t.Fatal("second attachment received ownership")
	}
	owner.heartbeat(t)
}
