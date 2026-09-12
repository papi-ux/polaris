//go:build linux

package seatprovider

import (
	"strings"
	"testing"

	"github.com/papi-ux/polaris/multiseat_worker/internal/seatruntime"
)

func encoderTestRequest() seatruntime.Request {
	return seatruntime.Request{
		Stage:                    seatruntime.StageEncoder,
		RuntimeNamespace:         "seat-encoder-contract",
		LogicalGPU:               "physical-gpu",
		RenderNode:               "/dev/dri/renderD128",
		EncoderSessions:          1,
		MediaPipeline:            seatruntime.MediaPipelineWorkerLocal,
		DisplayWidth:             1920,
		DisplayHeight:            1080,
		DisplayRefreshMillihertz: 60000,
	}
}

// Raw frames and encoded packets must never meet on one endpoint. A collision
// would put a seat's uncompressed capture where the data plane expects packets,
// which is the one thing data_plane.go says must not leave the worker.
func TestEncodedEndpointNeverCollidesWithTheCaptureEndpoint(t *testing.T) {
	for _, namespace := range []string{"a", "seat-encoder-contract", strings.Repeat("n", 128)} {
		capture, err := seatruntime.CaptureMediaSocketName(namespace)
		if err != nil {
			t.Fatalf("capture endpoint for %q: %v", namespace, err)
		}
		encoded, err := seatruntime.EncodedMediaSocketName(namespace)
		if err != nil {
			t.Fatalf("encoded endpoint for %q: %v", namespace, err)
		}
		if capture == encoded {
			t.Fatalf("namespace %q shares one endpoint for raw and encoded media", namespace)
		}
		again, err := seatruntime.EncodedMediaSocketName(namespace)
		if err != nil || again != encoded {
			t.Fatalf("encoded endpoint for %q is not stable: %q then %q", namespace, encoded, again)
		}
	}
}

// The pipeline has a direction: it reads the capture endpoint and publishes the
// encoded one. Reversing those would be silent, because both are Unix sockets
// in the same runtime directory.
func TestEncoderPipelineReadsCaptureAndPublishesEncoded(t *testing.T) {
	arguments := encoderArguments(encoderTestRequest(), "/run/capture.sock", "/run/encoded.sock", true)
	joined := strings.Join(arguments, " ")

	source := strings.Index(joined, "unixfdsrc socket-path=/run/capture.sock")
	sink := strings.Index(joined, "unixfdsink socket-path=/run/encoded.sock")
	if source < 0 {
		t.Fatalf("encoder must read the capture endpoint, got: %s", joined)
	}
	if sink < 0 {
		t.Fatalf("encoder must publish the encoded endpoint, got: %s", joined)
	}
	if source > sink {
		t.Fatalf("encoder reads and writes the wrong way round, got: %s", joined)
	}
	if !strings.Contains(joined, "openh264enc") {
		t.Fatalf("encoder must name its encoder element, got: %s", joined)
	}
	if !strings.Contains(joined, "video/x-h264") {
		t.Fatalf("encoder must constrain its output caps, got: %s", joined)
	}
}

// The probe reads the encoded endpoint, never the capture one, or readiness
// would be published on the strength of a raw frame the client can never use.
func TestEncoderProbeReadsOnlyTheEncodedEndpoint(t *testing.T) {
	joined := strings.Join(encoderProbeArguments("/run/encoded.sock"), " ")
	if !strings.Contains(joined, "unixfdsrc socket-path=/run/encoded.sock") {
		t.Fatalf("probe must read the encoded endpoint, got: %s", joined)
	}
	if !strings.Contains(joined, "video/x-h264") {
		t.Fatalf("probe must require an encoded packet, got: %s", joined)
	}
	if strings.Contains(joined, "num-buffers=0") {
		t.Fatalf("probe must demand at least one packet, got: %s", joined)
	}
}

// Only the worker-local pipeline exists. Accepting an unknown one would start a
// seat that encodes into nothing.
func TestEncoderRefusesAnUnsupportedMediaPipeline(t *testing.T) {
	request := encoderTestRequest()
	request.MediaPipeline = "client-remote-encode"
	if err := runEncoder(t.Context(), request, nopReadyWriter{}, defaultProviderOptions()); err == nil {
		t.Fatal("an unsupported media pipeline was accepted")
	}
}

type nopReadyWriter struct{}

func (nopReadyWriter) Write(payload []byte) (int, error) { return len(payload), nil }
func (nopReadyWriter) Close() error                      { return nil }
