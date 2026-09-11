//go:build linux

package main

import (
	"context"
	"errors"
	"testing"
	"time"
)

type fixtureMediaSource struct {
	contract     mediaConfig
	contractErr  error
	frames       chan fixtureFrame
	keyframes    chan struct{}
	invalidated  chan frameRange
	contractAsks int
}

type fixtureFrame struct {
	kind    message
	frame   mediaFrame
	encoded []byte
}

func newFixtureMediaSource() *fixtureMediaSource {
	return &fixtureMediaSource{
		contract:    goldenMediaConfig(),
		frames:      make(chan fixtureFrame, 4),
		keyframes:   make(chan struct{}, 4),
		invalidated: make(chan frameRange, 4),
	}
}

func (source *fixtureMediaSource) Contract(context.Context) (mediaConfig, error) {
	source.contractAsks++
	return source.contract, source.contractErr
}

func (source *fixtureMediaSource) Next(ctx context.Context) (message, mediaFrame, []byte, error) {
	select {
	case value := <-source.frames:
		return value.kind, value.frame, value.encoded, nil
	case <-ctx.Done():
		return 0, mediaFrame{}, nil, ctx.Err()
	}
}

func (source *fixtureMediaSource) Keyframe(ctx context.Context) error {
	select {
	case source.keyframes <- struct{}{}:
		return nil
	case <-ctx.Done():
		return ctx.Err()
	}
}

func (source *fixtureMediaSource) Invalidate(ctx context.Context, span frameRange) error {
	select {
	case source.invalidated <- span:
		return nil
	case <-ctx.Done():
		return ctx.Err()
	}
}

type recordingInputSink struct {
	inputs chan routedInput
}

func (sink *recordingInputSink) RouteInput(ctx context.Context, input routedInput) error {
	select {
	case sink.inputs <- input:
		return nil
	case <-ctx.Done():
		return ctx.Err()
	}
}

func testSeatIdentity() endpointIdentity {
	return endpointIdentity{
		ControllerEpoch: "controller-seat-plane",
		LogicalGPU:      "gpu-primary",
		Slot:            3,
		Generation:      97,
		WorkerName:      "polaris-worker-controller-seat-plane-97",
	}
}

func TestSeatDataPlaneAnnouncesItsContractBeforeAnyFrame(t *testing.T) {
	source := newFixtureMediaSource()
	plane := newSeatDataPlane(testSeatIdentity(), source, &recordingInputSink{inputs: make(chan routedInput, 1)})
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	announced, err := plane.NextMedia(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if announced.Message != messageMediaConfig || announced.Identity != testSeatIdentity() {
		t.Fatalf("the first media output was not this seat's contract: %+v", announced)
	}
	decoded, err := parseMediaConfig(announced.Payload)
	if err != nil || decoded != goldenMediaConfig() {
		t.Fatalf("announced contract did not survive encoding: %+v, %v", decoded, err)
	}
	if source.contractAsks != 1 {
		t.Fatalf("the encoder was asked for its contract %d times", source.contractAsks)
	}
}

func TestSeatDataPlaneProducesNothingUntilTheContractIsAcknowledged(t *testing.T) {
	source := newFixtureMediaSource()
	plane := newSeatDataPlane(testSeatIdentity(), source, &recordingInputSink{inputs: make(chan routedInput, 1)})
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	if _, err := plane.NextMedia(ctx); err != nil {
		t.Fatal(err)
	}
	source.frames <- fixtureFrame{
		kind:    messageVideo,
		frame:   mediaFrame{FrameIndex: 4, IDR: true},
		encoded: []byte("encoded-video"),
	}

	// A frame is ready, but nothing may leave until the controller agrees.
	waiting, blocked := context.WithTimeout(ctx, 150*time.Millisecond)
	defer blocked()
	if _, err := plane.NextMedia(waiting); !errors.Is(err, context.DeadlineExceeded) {
		t.Fatalf("a frame left the worker before its contract was acknowledged: %v", err)
	}

	if err := plane.RouteMediaControl(ctx, routedMediaControl{
		Identity: testSeatIdentity(),
		Message:  messageMediaConfigAck,
	}); err != nil {
		t.Fatal(err)
	}
	output, err := plane.NextMedia(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if output.Message != messageVideo {
		t.Fatalf("the released frame was not the one produced: %+v", output)
	}
	frame, encoded, err := parseMediaFrame(output.Payload)
	if err != nil {
		t.Fatal(err)
	}
	if frame.FrameIndex != 4 || !frame.IDR || string(encoded) != "encoded-video" {
		t.Fatalf("the frame changed on its way out: %+v, %q", frame, encoded)
	}

	// One contract, one acknowledgement.
	if err := plane.RouteMediaControl(ctx, routedMediaControl{
		Identity: testSeatIdentity(),
		Message:  messageMediaConfigAck,
	}); !errors.Is(err, errContractNotAcknowledged) {
		t.Fatalf("a second acknowledgement was accepted: %v", err)
	}
}

func TestSeatDataPlaneCarriesControllerAsksToItsOwnEncoder(t *testing.T) {
	source := newFixtureMediaSource()
	sink := &recordingInputSink{inputs: make(chan routedInput, 1)}
	plane := newSeatDataPlane(testSeatIdentity(), source, sink)
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	if err := plane.RouteMediaControl(ctx, routedMediaControl{
		Identity: testSeatIdentity(),
		Message:  messageRequestIDR,
	}); err != nil {
		t.Fatal(err)
	}
	select {
	case <-source.keyframes:
	default:
		t.Fatal("the keyframe request never reached the encoder")
	}

	if err := plane.RouteMediaControl(ctx, routedMediaControl{
		Identity: testSeatIdentity(),
		Message:  messageInvalidateReferenceFrames,
		Range:    frameRange{First: 12, Last: 15},
	}); err != nil {
		t.Fatal(err)
	}
	select {
	case span := <-source.invalidated:
		if span != (frameRange{First: 12, Last: 15}) {
			t.Fatalf("the retired span changed on the way: %+v", span)
		}
	default:
		t.Fatal("the reference invalidation never reached the encoder")
	}

	if err := plane.RouteInput(ctx, routedInput{Identity: testSeatIdentity(), Payload: []byte("input")}); err != nil {
		t.Fatal(err)
	}
	select {
	case input := <-sink.inputs:
		if string(input.Payload) != "input" {
			t.Fatalf("input changed on the way: %+v", input)
		}
	default:
		t.Fatal("input never reached the seat")
	}
}

func TestSeatDataPlaneRefusesAnotherSeatsTraffic(t *testing.T) {
	source := newFixtureMediaSource()
	sink := &recordingInputSink{inputs: make(chan routedInput, 1)}
	plane := newSeatDataPlane(testSeatIdentity(), source, sink)
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	other := testSeatIdentity()
	other.Generation++
	if err := plane.RouteMediaControl(ctx, routedMediaControl{Identity: other, Message: messageRequestIDR}); err == nil {
		t.Fatal("another seat's keyframe request reached this encoder")
	}
	if err := plane.RouteInput(ctx, routedInput{Identity: other, Payload: []byte("input")}); err == nil {
		t.Fatal("another seat's input reached this seat")
	}
	if len(source.keyframes) != 0 || len(sink.inputs) != 0 {
		t.Fatal("another seat's traffic was acted on before it was refused")
	}
}

func TestSeatDataPlaneRefusesAnUnrepresentableContract(t *testing.T) {
	source := newFixtureMediaSource()
	source.contract.Width = 1921 // odd, and so not representable
	plane := newSeatDataPlane(testSeatIdentity(), source, &recordingInputSink{inputs: make(chan routedInput, 1)})
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	if _, err := plane.NextMedia(ctx); !errors.Is(err, errUnrepresentableContract) {
		t.Fatalf("an unrepresentable contract was announced: %v", err)
	}
}

func TestSeatDataPlaneFeedbackWaitsRatherThanRetiringTheChannel(t *testing.T) {
	plane := newSeatDataPlane(testSeatIdentity(), newFixtureMediaSource(), &recordingInputSink{inputs: make(chan routedInput, 1)})
	ctx, cancel := context.WithTimeout(context.Background(), 50*time.Millisecond)
	defer cancel()
	if _, err := plane.NextFeedback(ctx); !errors.Is(err, context.DeadlineExceeded) {
		t.Fatalf("feedback returned something other than its own cancellation: %v", err)
	}
}
