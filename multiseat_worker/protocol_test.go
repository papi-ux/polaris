package main

import (
	"bytes"
	"encoding/hex"
	"testing"
)

func goldenIdentity() endpointIdentity {
	return endpointIdentity{
		ControllerEpoch: "controller-a1b2",
		LogicalGPU:      "gpu-primary",
		Slot:            7,
		Generation:      42,
		WorkerName:      "polaris-worker-controller-a1b2-42",
	}
}

func goldenCapability() [capabilitySize]byte {
	var capability [capabilitySize]byte
	for index := range capability {
		capability[index] = byte(index)
	}
	return capability
}

func goldenChallenge() [challengeSize]byte {
	var challenge [challengeSize]byte
	for index := range challenge {
		challenge[index] = byte(0xa0 + index)
	}
	return challenge
}

func TestProtocolGoldenAuthenticationVectors(t *testing.T) {
	controller, err := authenticationProof(
		goldenCapability(), proofController, channelControl, goldenIdentity(), goldenChallenge(),
	)
	if err != nil {
		t.Fatal(err)
	}
	worker, err := authenticationProof(
		goldenCapability(), proofWorker, channelControl, goldenIdentity(), goldenChallenge(),
	)
	if err != nil {
		t.Fatal(err)
	}
	if got, want := hex.EncodeToString(controller[:]), "f2bf394498b9a0bf65a3bc7cc9fb75b48eecf54f5f12923ad2aeb8b7f30d73db"; got != want {
		t.Fatalf("controller proof = %s, want %s", got, want)
	}
	if got, want := hex.EncodeToString(worker[:]), "86dfcf28d826940e7695bfd705f40b14ffa1d1efb5179a1e6dbcd76ae05247c3"; got != want {
		t.Fatalf("worker proof = %s, want %s", got, want)
	}
	if verifyProof(controller, worker[:]) {
		t.Fatal("role-separated proofs unexpectedly matched")
	}
}

func TestProtocolGoldenFrameVector(t *testing.T) {
	encoded, err := encodeFrame(frame{
		Channel: channelControl, Message: messageHeartbeat,
		Slot: 7, Generation: 42, Sequence: 3,
	})
	if err != nil {
		t.Fatal(err)
	}
	if got, want := hex.EncodeToString(encoded), "50535731010400000000000000000007000000000000002a0000000000000003"; got != want {
		t.Fatalf("frame = %s, want %s", got, want)
	}
	decoded, err := readFrame(bytes.NewReader(encoded), channelControl, 7, 42)
	if err != nil {
		t.Fatal(err)
	}
	if decoded.Message != messageHeartbeat || decoded.Sequence != 3 || len(decoded.Payload) != 0 {
		t.Fatalf("unexpected decoded frame: %+v", decoded)
	}
}

func TestReadFrameRejectsAdvertisedOversizeBeforeBody(t *testing.T) {
	encoded, err := encodeFrame(frame{
		Channel: channelControl, Message: messageHeartbeat,
		Slot: 7, Generation: 42, Sequence: 1,
	})
	if err != nil {
		t.Fatal(err)
	}
	encoded[5] = byte(messageInput)
	encoded[8], encoded[9], encoded[10], encoded[11] = 0, 1, 0, 1
	reader := bytes.NewReader(encoded)
	if _, err := readFrame(reader, channelControl, 7, 42); err == nil {
		t.Fatal("oversized control frame was accepted")
	}
	if reader.Len() != 0 {
		t.Fatalf("reader consumed an unexpected body: %d bytes remain", reader.Len())
	}
}

func TestProtocolRejectsWrongAuthorityAndMessageShape(t *testing.T) {
	encoded, err := encodeFrame(frame{
		Channel: channelControl, Message: messageHeartbeat,
		Slot: 7, Generation: 42, Sequence: 1,
	})
	if err != nil {
		t.Fatal(err)
	}
	for _, test := range []struct {
		name       string
		channel    channel
		slot       uint32
		generation uint64
	}{
		{"channel", channelMedia, 7, 42},
		{"slot", channelControl, 8, 42},
		{"generation", channelControl, 7, 43},
	} {
		t.Run(test.name, func(t *testing.T) {
			if _, err := readFrame(bytes.NewReader(encoded), test.channel, test.slot, test.generation); err == nil {
				t.Fatal("authority mismatch was accepted")
			}
		})
	}
	if _, err := encodeFrame(frame{
		Channel: channelControl, Message: messageVideo,
		Slot: 7, Generation: 42, Sequence: 1, Payload: []byte{1},
	}); err == nil {
		t.Fatal("media payload on control channel was accepted")
	}
}

func TestDataPlaneAttachmentAndDirectionShapesAreBounded(t *testing.T) {
	for _, selectedChannel := range []channel{channelControl, channelMedia} {
		if _, err := encodeFrame(frame{
			Channel: selectedChannel, Message: messageAttach,
			Slot: 7, Generation: 42, Sequence: 1,
		}); err != nil {
			t.Fatalf("attach rejected on %s: %v", selectedChannel, err)
		}
	}
	for _, invalid := range []frame{
		{Channel: channelMedia, Message: messageInputAck, Slot: 7, Generation: 42, Sequence: 1},
		{Channel: channelControl, Message: messageAttached, Slot: 7, Generation: 42, Sequence: 1, Payload: []byte{1}},
		{Channel: channelControl, Message: messageFeedback, Slot: 7, Generation: 42, Sequence: 1},
		{Channel: channelMedia, Message: messageVideo, Slot: 7, Generation: 42, Sequence: 1},
	} {
		if _, err := encodeFrame(invalid); err == nil {
			t.Fatalf("invalid data-plane frame was accepted: %+v", invalid)
		}
	}
}

func TestSequenceGuardRejectsReplayAndGap(t *testing.T) {
	guard := newSequenceGuard()
	if !guard.accept(1) || guard.accept(1) || guard.accept(3) || !guard.accept(2) {
		t.Fatal("sequence guard did not enforce exact monotonic order")
	}
}

func TestCapabilityParserIsCanonical(t *testing.T) {
	capability := goldenCapability()
	encoded := hex.EncodeToString(capability[:])
	if _, err := parseCapability(encoded); err != nil {
		t.Fatal(err)
	}
	for _, invalid := range []string{encoded[:63], encoded + "00", "A" + encoded[1:], "g" + encoded[1:]} {
		if _, err := parseCapability(invalid); err == nil {
			t.Fatalf("invalid capability %q was accepted", invalid)
		}
	}
}

func goldenMediaConfig() mediaConfig {
	return mediaConfig{
		VideoCodec: videoCodecH264, ProfileIDC: 100, LevelIDC: 41,
		Width: 1920, Height: 1080, FPSNumerator: 60, FPSDenominator: 1,
		BitrateCeilingKbps: 20000,
		AudioCodec:         audioCodecOpus, AudioChannels: 2,
		AudioFrameDurationUS: 5000, AudioSampleRate: 48000,
	}
}

func TestMediaConfigGoldenVectorAndRejections(t *testing.T) {
	encoded, err := encodeMediaConfig(goldenMediaConfig())
	if err != nil {
		t.Fatal(err)
	}
	const want = "01016429078004380000003c0000000100004e20010213880000bb8000000000"
	if got := hex.EncodeToString(encoded); got != want {
		t.Fatalf("media config = %s, want %s", got, want)
	}
	decoded, err := parseMediaConfig(encoded)
	if err != nil || decoded != goldenMediaConfig() {
		t.Fatalf("media config round trip mismatch: %+v, %v", decoded, err)
	}
	if _, err := parseMediaConfig(encoded[:mediaConfigSize-1]); err == nil {
		t.Fatal("short media config body was accepted")
	}
	for _, mutate := range []struct {
		name   string
		mutate func(*mediaConfig)
	}{
		{"unknown video codec", func(c *mediaConfig) { c.VideoCodec = 2 }},
		{"unknown audio codec", func(c *mediaConfig) { c.AudioCodec = 2 }},
		{"odd width", func(c *mediaConfig) { c.Width = 1921 }},
		{"tiny height", func(c *mediaConfig) { c.Height = 8 }},
		{"zero fps", func(c *mediaConfig) { c.FPSNumerator = 0 }},
		{"zero fps denominator", func(c *mediaConfig) { c.FPSDenominator = 0 }},
		{"sub-hertz fps", func(c *mediaConfig) { c.FPSNumerator, c.FPSDenominator = 1, 2 }},
		{"kilohertz fps", func(c *mediaConfig) { c.FPSNumerator = 1001 }},
		{"zero bitrate", func(c *mediaConfig) { c.BitrateCeilingKbps = 0 }},
		{"wrong sample rate", func(c *mediaConfig) { c.AudioSampleRate = 44100 }},
		{"no channels", func(c *mediaConfig) { c.AudioChannels = 0 }},
		{"illegal frame duration", func(c *mediaConfig) { c.AudioFrameDurationUS = 3000 }},
		{"unknown profile", func(c *mediaConfig) { c.ProfileIDC = 1 }},
		{"unknown level", func(c *mediaConfig) { c.LevelIDC = 99 }},
	} {
		t.Run(mutate.name, func(t *testing.T) {
			config := goldenMediaConfig()
			mutate.mutate(&config)
			if validMediaConfig(config) {
				t.Fatal("invalid media config was accepted")
			}
			if _, err := encodeMediaConfig(config); err == nil {
				t.Fatal("invalid media config was encoded")
			}
		})
	}
	for _, corrupt := range []struct {
		name   string
		offset int
		value  byte
	}{
		{"version", 0, 2},
		{"reserved", 31, 1},
		{"codec", 1, 7},
	} {
		t.Run(corrupt.name, func(t *testing.T) {
			body := append([]byte(nil), encoded...)
			body[corrupt.offset] = corrupt.value
			if _, err := parseMediaConfig(body); err == nil {
				t.Fatal("corrupted media config body was accepted")
			}
		})
	}
}

func TestMediaFrameGoldenVectorAndRejections(t *testing.T) {
	frame := mediaFrame{FrameIndex: 7, IDR: true, CaptureTimestampNS: 1000000000, EncodeTimestampNS: 1000500000}
	payload, err := encodeMediaFrame(frame, []byte("h264"))
	if err != nil {
		t.Fatal(err)
	}
	const want = "01010000000000000000000000000007000000003b9aca00000000003ba26b2068323634"
	if got := hex.EncodeToString(payload); got != want {
		t.Fatalf("media frame = %s, want %s", got, want)
	}
	decoded, encoded, err := parseMediaFrame(payload)
	if err != nil || decoded != frame || string(encoded) != "h264" {
		t.Fatalf("media frame round trip mismatch: %+v, %q, %v", decoded, encoded, err)
	}
	if _, err := encodeMediaFrame(frame, nil); err == nil {
		t.Fatal("empty media frame was encoded")
	}
	if _, _, err := parseMediaFrame(payload[:mediaFramePrefixSize]); err == nil {
		t.Fatal("prefix without encoded bytes was accepted")
	}
	for _, corrupt := range []struct {
		name   string
		offset int
		value  byte
	}{
		{"version", 0, 0},
		{"unknown flag", 1, 0x03},
		{"reserved16", 2, 1},
		{"reserved32", 7, 1},
	} {
		t.Run(corrupt.name, func(t *testing.T) {
			body := append([]byte(nil), payload...)
			body[corrupt.offset] = corrupt.value
			if _, _, err := parseMediaFrame(body); err == nil {
				t.Fatal("corrupted media frame prefix was accepted")
			}
		})
	}
}

func TestFrameRangeGoldenVectorAndRejections(t *testing.T) {
	encoded, err := encodeFrameRange(frameRange{First: 5, Last: 9})
	if err != nil {
		t.Fatal(err)
	}
	if got, want := hex.EncodeToString(encoded), "00000000000000050000000000000009"; got != want {
		t.Fatalf("frame range = %s, want %s", got, want)
	}
	decoded, err := parseFrameRange(encoded)
	if err != nil || decoded != (frameRange{First: 5, Last: 9}) {
		t.Fatalf("frame range round trip mismatch: %+v, %v", decoded, err)
	}
	if _, err := encodeFrameRange(frameRange{First: 9, Last: 5}); err == nil {
		t.Fatal("inverted frame range was encoded")
	}
	inverted, _ := encodeFrameRange(frameRange{First: 9, Last: 9})
	inverted[15] = 5
	if _, err := parseFrameRange(inverted); err == nil {
		t.Fatal("inverted frame range body was accepted")
	}
	if _, err := parseFrameRange(encoded[:8]); err == nil {
		t.Fatal("short frame range body was accepted")
	}
}

func TestMediaContractMessagesAreControlOnlyAndFixedSize(t *testing.T) {
	config, _ := encodeMediaConfig(goldenMediaConfig())
	span, _ := encodeFrameRange(frameRange{First: 1, Last: 2})
	for _, valid := range []frame{
		{Channel: channelMedia, Message: messageMediaConfig, Slot: 7, Generation: 42, Sequence: 1, Payload: config},
		{Channel: channelControl, Message: messageMediaConfigAck, Slot: 7, Generation: 42, Sequence: 1},
		{Channel: channelControl, Message: messageRequestIDR, Slot: 7, Generation: 42, Sequence: 1},
		{Channel: channelControl, Message: messageMediaControlAck, Slot: 7, Generation: 42, Sequence: 1},
		{Channel: channelControl, Message: messageInvalidateReferenceFrames, Slot: 7, Generation: 42, Sequence: 1, Payload: span},
	} {
		if _, err := encodeFrame(valid); err != nil {
			t.Fatalf("valid media contract frame rejected: %+v: %v", valid, err)
		}
	}
	for _, invalid := range []frame{
		{Channel: channelControl, Message: messageMediaConfig, Slot: 7, Generation: 42, Sequence: 1, Payload: config},
		{Channel: channelMedia, Message: messageMediaConfig, Slot: 7, Generation: 42, Sequence: 1, Payload: config[:31]},
		{Channel: channelMedia, Message: messageMediaControlAck, Slot: 7, Generation: 42, Sequence: 1},
		{Channel: channelControl, Message: messageMediaConfigAck, Slot: 7, Generation: 42, Sequence: 1, Payload: []byte{1}},
		{Channel: channelMedia, Message: messageRequestIDR, Slot: 7, Generation: 42, Sequence: 1},
		{Channel: channelControl, Message: messageMediaControlAck, Slot: 7, Generation: 42, Sequence: 1, Payload: []byte{1}},
		{Channel: channelControl, Message: messageInvalidateReferenceFrames, Slot: 7, Generation: 42, Sequence: 1, Payload: span[:8]},
	} {
		if _, err := encodeFrame(invalid); err == nil {
			t.Fatalf("invalid media contract frame was accepted: %+v", invalid)
		}
	}
}
