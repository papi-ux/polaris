//go:build linux

package main

import (
	"context"
	"errors"
	"sync"
)

// mediaSource is one seat's encoder, seen from the data plane. Production
// binds this to the seat's own encoder provider; tests bind it to a fixture.
// Every method must honor cancellation, and no implementation may be shared
// between seats: one encoder instance, one queue, one seat.
type mediaSource interface {
	// Contract is what this encoder produces for the life of the seat. It is
	// asked for once, before anything is read, and never changes afterwards.
	Contract(context.Context) (mediaConfig, error)
	// Next blocks for the next encoded frame: its message, its prefix and the
	// encoded bytes that follow it.
	Next(context.Context) (message, mediaFrame, []byte, error)
	// Keyframe asks for the next produced frame to stand alone.
	Keyframe(context.Context) error
	// Invalidate retires an inclusive span of frames the client cannot use.
	Invalidate(context.Context, frameRange) error
}

// inputSink is where controller input lands inside the worker.
type inputSink interface {
	RouteInput(context.Context, routedInput) error
}

var (
	errContractNotAcknowledged = errors.New("worker media contract was acknowledged twice")
	errUnrepresentableContract = errors.New("worker encoder announced an unrepresentable media contract")
	errUnexpectedMediaControl  = errors.New("worker received an unknown media control message")
)

// seatDataPlane is one seat's bridge between its encoder and its controller.
//
// It announces the encoder's contract as the first thing on the media channel
// and then produces nothing until the controller has acknowledged it, so a
// client can never be handed frames it did not agree to decode. The
// controller's instructions reach the encoder through the same object, which
// is what keeps one seat's keyframe request from reaching another's encoder.
type seatDataPlane struct {
	identity endpointIdentity
	source   mediaSource
	input    inputSink

	mutex        sync.Mutex
	announced    bool
	acknowledged chan struct{}
	released     bool
}

func newSeatDataPlane(identity endpointIdentity, source mediaSource, input inputSink) *seatDataPlane {
	return &seatDataPlane{
		identity:     identity,
		source:       source,
		input:        input,
		acknowledged: make(chan struct{}),
	}
}

func (plane *seatDataPlane) RouteInput(ctx context.Context, input routedInput) error {
	if plane.input == nil {
		return errors.New("worker has no input sink for its seat")
	}
	if input.Identity != plane.identity {
		return errors.New("worker input arrived for another seat")
	}
	return plane.input.RouteInput(ctx, input)
}

func (plane *seatDataPlane) RouteMediaControl(ctx context.Context, control routedMediaControl) error {
	if control.Identity != plane.identity {
		return errors.New("worker media control arrived for another seat")
	}
	switch control.Message {
	case messageMediaConfigAck:
		plane.mutex.Lock()
		defer plane.mutex.Unlock()
		if plane.released {
			// One contract, one acknowledgement. A second one would mean the
			// controller believes it negotiated something else.
			return errContractNotAcknowledged
		}
		plane.released = true
		close(plane.acknowledged)
		return nil
	case messageRequestIDR:
		return plane.source.Keyframe(ctx)
	case messageInvalidateReferenceFrames:
		return plane.source.Invalidate(ctx, control.Range)
	default:
		return errUnexpectedMediaControl
	}
}

// NextFeedback has nothing to report while the worker carries no controller
// feedback of its own. It waits rather than returning, because returning would
// retire the seat's control channel.
func (plane *seatDataPlane) NextFeedback(ctx context.Context) (routedOutput, error) {
	<-ctx.Done()
	return routedOutput{}, ctx.Err()
}

func (plane *seatDataPlane) NextMedia(ctx context.Context) (routedOutput, error) {
	plane.mutex.Lock()
	announced := plane.announced
	plane.announced = true
	plane.mutex.Unlock()

	if !announced {
		contract, err := plane.source.Contract(ctx)
		if err != nil {
			return routedOutput{}, err
		}
		encoded, err := encodeMediaConfig(contract)
		if err != nil {
			return routedOutput{}, errUnrepresentableContract
		}
		return routedOutput{
			Identity: plane.identity,
			Message:  messageMediaConfig,
			Payload:  encoded,
		}, nil
	}

	select {
	case <-plane.acknowledged:
	case <-ctx.Done():
		return routedOutput{}, ctx.Err()
	}

	kind, frame, encoded, err := plane.source.Next(ctx)
	if err != nil {
		return routedOutput{}, err
	}
	if kind != messageVideo && kind != messageAudio {
		return routedOutput{
			Identity: plane.identity,
			Message:  kind,
		}, nil
	}
	payload, err := encodeMediaFrame(frame, encoded)
	if err != nil {
		return routedOutput{}, err
	}
	return routedOutput{
		Identity: plane.identity,
		Message:  kind,
		Payload:  payload,
	}, nil
}
