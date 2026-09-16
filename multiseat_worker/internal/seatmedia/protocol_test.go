package seatmedia

import (
	"bytes"
	"encoding/binary"
	"testing"
)

func TestPacketsAreBoundedBeforeReadingTheirBodies(t *testing.T) {
	for _, entry := range []struct {
		kind  byte
		size  uint32
		valid bool
	}{
		{Config, 32, true}, {Config, 31, false}, {Video, 33, true}, {Video, MaxPayload + 1, false},
		{Audio, 1432, true}, {Audio, 1433, false}, {Video, 32, false}, {99, 32, false},
	} {
		header := make([]byte, 12)
		copy(header, "PME1")
		header[4] = entry.kind
		binary.BigEndian.PutUint32(header[8:], entry.size)
		_, _, err := Read(bytes.NewReader(header))
		if err == nil {
			t.Fatal("missing body accepted")
		}
		if !entry.valid && err.Error() != "invalid private encoder packet" {
			t.Fatal("invalid length reached body reader", err)
		}
	}
}

func TestPacketsRoundTripAndRejectHeaderDrift(t *testing.T) {
	for _, kind := range []byte{Config, Video, Audio} {
		body := bytes.Repeat([]byte{0x42}, 33)
		if kind == Config {
			body = body[:32]
		}
		var wire bytes.Buffer
		if err := Write(&wire, kind, body); err != nil {
			t.Fatal(err)
		}
		original := append([]byte(nil), wire.Bytes()...)
		actual, payload, err := Read(&wire)
		if err != nil || actual != kind || !bytes.Equal(payload, body) {
			t.Fatal("round trip failed", err)
		}
		for _, offset := range []int{0, 5, 6, 7} {
			mutated := append([]byte(nil), original...)
			mutated[offset] ^= 1
			if _, _, err := Read(bytes.NewReader(mutated)); err == nil {
				t.Fatal("invalid header accepted")
			}
		}
	}
}
