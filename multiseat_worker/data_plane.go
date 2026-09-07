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

// workerDataPlane owns the exact seat-local bridges behind authenticated IPC.
// Every method must honor cancellation. Implementations must never share an
// input device, raw-frame path, encoder instance, or output queue across seats.
type workerDataPlane interface {
	RouteInput(context.Context, routedInput) error
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
