//go:build linux

package main

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"io"
	"os"
	"os/exec"
	"os/signal"
	"path/filepath"
	"syscall"
	"time"
)

type encodedAudioObservation struct {
	Source         string `json:"source"`
	Codec          string `json:"codec"`
	Rate           uint32 `json:"rate"`
	Channels       uint32 `json:"channels"`
	PacketMS       uint32 `json:"packet_ms"`
	EncodedPackets uint32 `json:"encoded_packets"`
	DecodedSamples uint32 `json:"decoded_samples"`
	MaxPacketBytes uint32 `json:"max_packet_bytes"`
	LeftHz         uint32 `json:"left_hz"`
	RightHz        uint32 `json:"right_hz"`
	LeftRMS        uint32 `json:"left_rms_million"`
	RightRMS       uint32 `json:"right_rms_million"`
	Passed         bool   `json:"passed"`
}

func parseEncodedAudioObservation(content []byte) (encodedAudioObservation, error) {
	var observation encodedAudioObservation
	var fields map[string]json.RawMessage
	if len(content) > 1024 || json.Unmarshal(content, &fields) != nil || len(fields) != 13 {
		return observation, errors.New("encoded audio observation fields invalid")
	}
	for _, field := range fields {
		if string(field) == "null" {
			return observation, errors.New("encoded audio observation contains null")
		}
	}
	decoder := json.NewDecoder(bytes.NewReader(content))
	decoder.DisallowUnknownFields()
	if decoder.Decode(&observation) != nil || decoder.Decode(new(any)) != io.EOF ||
		observation.Source != "worker-pulse-monitor" || observation.Codec != "opus" ||
		observation.Rate != 48000 || observation.Channels != 2 || observation.PacketMS != 5 ||
		observation.DecodedSamples < 48000 || observation.DecodedSamples >= 48240 ||
		observation.EncodedPackets < (observation.DecodedSamples+239)/240 ||
		observation.EncodedPackets > (observation.DecodedSamples+239)/240+3 ||
		observation.MaxPacketBytes < 2 || observation.MaxPacketBytes > 1400 ||
		observation.LeftHz < 430 || observation.LeftHz > 450 || observation.RightHz < 430 || observation.RightHz > 450 ||
		observation.LeftRMS < 2000 || observation.LeftRMS > 30000 || observation.RightRMS < 2000 || observation.RightRMS > 30000 ||
		!observation.Passed {
		return encodedAudioObservation{}, errors.New("encoded game audio observation failed")
	}
	return observation, nil
}

func physicalEncodedAudioProbe(path string) (result error) {
	parent, stop := signal.NotifyContext(context.Background(), syscall.SIGTERM, syscall.SIGINT)
	defer stop()
	ctx, cancel := context.WithTimeout(parent, 12*time.Second)
	defer cancel()
	ready, identity, err := openGameProbeSignal(path)
	if err != nil {
		return err
	}
	defer ready.Close()
	config, err := loadWorkerConfig(os.LookupEnv, workloadPlan{Kind: workloadKindGamescope, TargetID: "input-pong-v1"})
	if err != nil {
		return err
	}
	if config.RuntimeProfile != "gamescope" || config.Compositor != "gamescope" || config.DisplayHDR {
		return errors.New("audio probe allocation unsupported")
	}
	if err := checkHealthContext(ctx, config, productionPaths(), uint32(os.Geteuid())); err != nil {
		return err
	}
	claimPath := filepath.Join(containerStatePath, "physical-game.audio-encoding")
	claim, claimIdentity, err := createGameProbeSignal(claimPath)
	if err != nil {
		return err
	}
	defer func() { result = errors.Join(result, removeGameProbeSignal(claimPath, claimIdentity)); claim.Close() }()
	command := exec.CommandContext(ctx, "/usr/libexec/polaris-seat/encoded-audio-check", config.AudioSink)
	command.Env = []string{"HOME=/nonexistent", "LC_ALL=C", "XDG_RUNTIME_DIR=/run/polaris", "GST_REGISTRY=/dev/null", "GST_REGISTRY_1_0=/dev/null", "GST_PLUGIN_PATH=", "GST_PLUGIN_PATH_1_0="}
	command.SysProcAttr = &syscall.SysProcAttr{Pdeathsig: syscall.SIGKILL}
	command.WaitDelay = 100 * time.Millisecond
	var output, diagnostic gameProbeOutput
	command.Stdout = &output
	command.Stderr = &diagnostic
	if err := command.Run(); err != nil {
		_ = json.NewEncoder(os.Stdout).Encode(map[string]string{"error": "audio probe failed", "observation": output.String(), "diagnostic": diagnostic.String()})
		return errors.New("worker game audio encoding or decoding failed")
	}
	observation, err := parseEncodedAudioObservation(output.Bytes())
	if err != nil {
		return err
	}
	if after, err := identityOf(path); err != nil || after != identity {
		return errors.New("physical game retired during audio observation")
	}
	if err := checkHealthContext(ctx, config, productionPaths(), uint32(os.Geteuid())); err != nil {
		return err
	}
	return json.NewEncoder(os.Stdout).Encode(observation)
}
