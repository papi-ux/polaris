//go:build linux

package main

import (
	"encoding/json"
	"testing"
)

func TestEncodedGameObservationRequiresCapturedDecodedMovingGame(t *testing.T) {
	valid := encodedGameObservation{Source: "worker-capture", Encoder: "openh264", Width: 1920, Height: 1080,
		EncodedFrames: 60, DecodedFrames: 60, SceneFrames: 60, MotionFrames: 59, Keyframes: 2, EncodedBytes: 100000, MaxFrameBytes: 10000, Passed: true}
	content, _ := json.Marshal(valid)
	if got, err := parseEncodedGameObservation(content, 1920, 1080); err != nil || got != valid {
		t.Fatalf("valid observation: %+v %v", got, err)
	}
	cases := []struct {
		name   string
		change func(*encodedGameObservation)
	}{
		{"synthetic", func(v *encodedGameObservation) { v.Source = "synthetic" }},
		{"wrong width", func(v *encodedGameObservation) { v.Width = 1280 }},
		{"wrong height", func(v *encodedGameObservation) { v.Height = 720 }},
		{"unknown encoder", func(v *encodedGameObservation) { v.Encoder = "automatic" }},
		{"missing encoded frames", func(v *encodedGameObservation) { v.EncodedFrames = 59 }},
		{"missing decoded frames", func(v *encodedGameObservation) { v.DecodedFrames = 59 }},
		{"empty scene", func(v *encodedGameObservation) { v.SceneFrames = 0 }},
		{"frozen scene", func(v *encodedGameObservation) { v.MotionFrames = 0 }},
		{"no keyframe", func(v *encodedGameObservation) { v.Keyframes = 0 }},
		{"oversized frame", func(v *encodedGameObservation) { v.MaxFrameBytes = 16*1024*1024 + 1 }},
		{"impossible byte total", func(v *encodedGameObservation) { v.EncodedBytes = 1 }},
		{"excess byte total", func(v *encodedGameObservation) { v.EncodedBytes = 60*v.MaxFrameBytes + 1 }},
		{"unsuccessful", func(v *encodedGameObservation) { v.Passed = false }},
	}
	for _, test := range cases {
		t.Run(test.name, func(t *testing.T) {
			v := valid
			test.change(&v)
			data, _ := json.Marshal(v)
			if _, err := parseEncodedGameObservation(data, 1920, 1080); err == nil {
				t.Fatal("accepted invalid evidence")
			}
		})
	}
	for _, data := range [][]byte{append(append([]byte{}, content...), []byte(" {}")...), []byte(`{}`), make([]byte, 1025)} {
		if _, err := parseEncodedGameObservation(data, 1920, 1080); err == nil {
			t.Fatal("accepted malformed evidence")
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
		if _, err := parseEncodedGameObservation(data, 1920, 1080); err == nil {
			t.Fatalf("accepted missing %s", name)
		}
		copyFields[name] = json.RawMessage("null")
		data, _ = json.Marshal(copyFields)
		if _, err := parseEncodedGameObservation(data, 1920, 1080); err == nil {
			t.Fatalf("accepted null %s", name)
		}
	}
}
