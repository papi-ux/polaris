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
