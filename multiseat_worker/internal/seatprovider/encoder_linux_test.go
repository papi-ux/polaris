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
		AudioSink:                "encoder-audio",
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

func TestEncoderUsesOnlyAllocatedCaptureAudioAndDisplay(t *testing.T) {
	request := encoderTestRequest()
	arguments := encoderArguments(request, "/run/capture.sock", true)
	expected := []string{"/run/capture.sock", "/dev/dri/renderD128", "encoder-audio", "1920", "1080", "60000", "true"}
	if strings.Join(arguments, "\x00") != strings.Join(expected, "\x00") {
		t.Fatalf("encoder arguments differ from allocation: %v", arguments)
	}
}

func TestEncoderRejectsMissingAudioGeometryAndHDR(t *testing.T) {
	mutations := []func(*seatruntime.Request){
		func(r *seatruntime.Request) { r.AudioSink = "" },
		func(r *seatruntime.Request) { r.DisplayWidth = 0 },
		func(r *seatruntime.Request) { r.DisplayHeight = 1079 },
		func(r *seatruntime.Request) { r.DisplayHDR = true },
	}
	for _, mutate := range mutations {
		request := encoderTestRequest()
		mutate(&request)
		if err := runEncoder(t.Context(), request, nopReadyWriter{}, defaultProviderOptions()); err == nil {
			t.Fatal("unsupported encoder allocation accepted")
		}
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

func TestEncoderReadinessRequiresItsExactMediaContract(t *testing.T) {
	// Literal protocol fixture: constrained baseline, 1920x1080 at 60 Hz,
	// 8 Mbps video and stereo 48 kHz Opus in five millisecond packets.
	contract := []byte{1, 1, 66, 40, 7, 128, 4, 56, 0, 0, 234, 96, 0, 0, 3, 232, 0, 0, 31, 64, 1, 2, 19, 136, 0, 0, 187, 128, 0, 0, 0, 0}
	if !validEncoderContract(contract, encoderTestRequest()) {
		t.Fatal("valid native contract rejected")
	}
	for index := range contract {
		changed := append([]byte(nil), contract...)
		changed[index] ^= 128
		if validEncoderContract(changed, encoderTestRequest()) {
			t.Fatalf("malformed native contract accepted at offset %d", index)
		}
	}
	if validEncoderContract(contract[:31], encoderTestRequest()) {
		t.Fatal("truncated native contract accepted")
	}
}
