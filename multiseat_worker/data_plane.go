package main

import "context"

// routedInput is the only controller-to-worker data-plane unit. Identity is
// repeated at the adapter boundary so a shared or faulty router cannot move an
// input packet into another seat after transport authentication succeeded.
type routedInput struct {
	Identity endpointIdentity
	Payload  []byte
}

// routedOutput is worker-to-controller feedback or already encoded media.
// Raw captured frames remain inside the worker-local capture/encode pipeline.
type routedOutput struct {
	Identity endpointIdentity
	Message  message
	Payload  []byte
}

// routedMediaControl is a controller-to-worker instruction about the media
// this seat produces: the acknowledgement that releases the stream, a request
// for the next frame to be an IDR, or a span of frames whose references the
// client can no longer use. Identity is repeated for the same reason as
// routedInput, so a faulty router cannot steer one seat's encoder from
// another seat's control channel.
type routedMediaControl struct {
	Identity endpointIdentity
	Message  message
	Range    frameRange
}

// workerDataPlane owns the exact seat-local bridges behind authenticated IPC.
// Every method must honor cancellation. Implementations must never share an
// input device, raw-frame path, encoder instance, or output queue across seats.
type workerDataPlane interface {
	RouteInput(context.Context, routedInput) error
	RouteMediaControl(context.Context, routedMediaControl) error
	NextFeedback(context.Context) (routedOutput, error)
	NextMedia(context.Context) (routedOutput, error)
}

func validRoutedOutput(
	output routedOutput,
	identity endpointIdentity,
	selectedChannel channel,
) bool {
	if output.Identity != identity || len(output.Payload) > payloadLimit(selectedChannel) {
		return false
	}
	switch selectedChannel {
	case channelControl:
		return output.Message == messageFeedback && len(output.Payload) > 0
	case channelMedia:
		switch output.Message {
		case messageMediaConfig:
			// The contract announcement is exact; the controller refuses any
			// other size before it reads a single frame.
			return len(output.Payload) == mediaConfigSize
		case messageVideo, messageAudio:
			return len(output.Payload) > 0
		case messageEndOfStream, messageDiscontinuity:
			return len(output.Payload) == 0
		default:
			return false
		}
	default:
		return false
	}
}
