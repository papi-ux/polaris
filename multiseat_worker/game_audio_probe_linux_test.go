//go:build linux

package main

import (
	"encoding/json"
	"testing"
)

func TestEncodedAudioObservationRequiresAllocatedStereoToneAndBoundedPackets(t *testing.T) {
	valid := encodedAudioObservation{Source: "worker-pulse-monitor", Codec: "opus", Rate: 48000, Channels: 2,
		PacketMS: 5, EncodedPackets: 201, DecodedSamples: 48120, MaxPacketBytes: 60,
		LeftHz: 439, RightHz: 439, LeftRMS: 10625, RightRMS: 10625, Passed: true}
	content, _ := json.Marshal(valid)
	if got, err := parseEncodedAudioObservation(content); err != nil || got != valid {
		t.Fatalf("valid observation: %+v %v", got, err)
	}
	for _, test := range []struct {
		name   string
		change func(*encodedAudioObservation)
	}{
		{"synthetic", func(v *encodedAudioObservation) { v.Source = "synthetic" }},
		{"wrong codec", func(v *encodedAudioObservation) { v.Codec = "aac" }},
		{"wrong rate", func(v *encodedAudioObservation) { v.Rate = 44100 }},
		{"mono", func(v *encodedAudioObservation) { v.Channels = 1 }},
		{"wrong packet duration", func(v *encodedAudioObservation) { v.PacketMS = 20 }},
		{"short observation", func(v *encodedAudioObservation) { v.DecodedSamples = 47999 }},
		{"excess samples", func(v *encodedAudioObservation) { v.DecodedSamples = 48240 }},
		{"missing packets", func(v *encodedAudioObservation) { v.EncodedPackets = 200 }},
		{"excess packets", func(v *encodedAudioObservation) { v.EncodedPackets = 205 }},
		{"empty packets", func(v *encodedAudioObservation) { v.MaxPacketBytes = 0 }},
		{"oversized packets", func(v *encodedAudioObservation) { v.MaxPacketBytes = 1401 }},
		{"wrong left tone", func(v *encodedAudioObservation) { v.LeftHz = 880 }},
		{"wrong right tone", func(v *encodedAudioObservation) { v.RightHz = 880 }},
		{"silent left", func(v *encodedAudioObservation) { v.LeftRMS = 0 }},
		{"silent right", func(v *encodedAudioObservation) { v.RightRMS = 0 }},
		{"excess volume", func(v *encodedAudioObservation) { v.RightRMS = 30001 }},
		{"unsuccessful", func(v *encodedAudioObservation) { v.Passed = false }},
	} {
		t.Run(test.name, func(t *testing.T) {
			v := valid
			test.change(&v)
			data, _ := json.Marshal(v)
			if _, err := parseEncodedAudioObservation(data); err == nil {
				t.Fatal("invalid audio evidence accepted")
			}
		})
	}
	for _, data := range [][]byte{append(append([]byte{}, content...), []byte(" {}")...), []byte(`{}`), make([]byte, 1025)} {
		if _, err := parseEncodedAudioObservation(data); err == nil {
			t.Fatal("malformed audio evidence accepted")
		}
	}
	var fields map[string]json.RawMessage
	_ = json.Unmarshal(content, &fields)
	for name := range fields {
		copyFields := make(map[string]json.RawMessage)
		for key, value := range fields {
			copyFields[key] = value
		}
		delete(copyFields, name)
		data, _ := json.Marshal(copyFields)
		if _, err := parseEncodedAudioObservation(data); err == nil {
			t.Fatalf("missing %s accepted", name)
		}
		copyFields[name] = json.RawMessage("null")
		data, _ = json.Marshal(copyFields)
		if _, err := parseEncodedAudioObservation(data); err == nil {
			t.Fatalf("null %s accepted", name)
		}
	}
}
