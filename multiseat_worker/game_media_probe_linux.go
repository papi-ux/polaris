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
	"strconv"
	"syscall"
	"time"

	"github.com/papi-ux/polaris/multiseat_worker/internal/seatruntime"
)

// This observation is an explicitly selected software H.264 round trip inside
// the allocated worker. It never claims network delivery or client playback.
type encodedGameObservation struct {
	Source        string `json:"source"`
	Encoder       string `json:"encoder"`
	Width         uint32 `json:"width"`
	Height        uint32 `json:"height"`
	EncodedFrames uint32 `json:"encoded_frames"`
	DecodedFrames uint32 `json:"decoded_frames"`
	SceneFrames   uint32 `json:"scene_frames"`
	ChangedFrames uint32 `json:"changed_frames"`
	Keyframes     uint32 `json:"keyframes"`
	EncodedBytes  uint64 `json:"encoded_bytes"`
	MaxFrameBytes uint64 `json:"max_frame_bytes"`
	Passed        bool   `json:"passed"`
}

func parseEncodedGameObservation(content []byte, width, height uint32) (encodedGameObservation, error) {
	var observation encodedGameObservation
	var fields map[string]json.RawMessage
	if len(content) > 1024 || json.Unmarshal(content, &fields) != nil || len(fields) != 12 {
		return observation, errors.New("encoded observation fields invalid")
	}
	for _, field := range fields {
		if string(field) == "null" {
			return observation, errors.New("encoded observation contains null")
		}
	}
	decoder := json.NewDecoder(bytes.NewReader(content))
	decoder.DisallowUnknownFields()
	if decoder.Decode(&observation) != nil || decoder.Decode(new(any)) != io.EOF ||
		observation.Source != "worker-capture" || observation.Encoder != "openh264" ||
		observation.Width != width || observation.Height != height ||
		observation.EncodedFrames != 60 || observation.DecodedFrames != 60 ||
		observation.SceneFrames < 30 || observation.SceneFrames > 60 ||
		observation.ChangedFrames < 10 || observation.ChangedFrames > 59 ||
		observation.Keyframes == 0 || observation.Keyframes > 60 ||
		observation.MaxFrameBytes == 0 || observation.MaxFrameBytes > 16*1024*1024 ||
		observation.EncodedBytes < observation.MaxFrameBytes || observation.EncodedBytes > 60*observation.MaxFrameBytes || !observation.Passed {
		return encodedGameObservation{}, errors.New("encoded game observation failed")
	}
	return observation, nil
}

func physicalEncodedGameProbe(path string) (result error) {
	ready, identity, err := openGameProbeSignal(path)
	if err != nil {
		return err
	}
	defer ready.Close()
	config, err := loadWorkerConfig(os.LookupEnv, workloadPlan{Kind: workloadKindGamescope, TargetID: "input-pong-v1"})
	if err != nil {
		return err
	}
	if config.RuntimeProfile != "gamescope" || config.Compositor != "gamescope" || config.DisplayHDR ||
		config.DisplayWidth < 320 || config.DisplayWidth > 3840 || config.DisplayWidth%2 != 0 ||
		config.DisplayHeight < 320 || config.DisplayHeight > 3840 || config.DisplayHeight%2 != 0 {
		return errors.New("encoded probe allocation unsupported")
	}
	if err := checkHealth(config, productionPaths(), uint32(os.Geteuid())); err != nil {
		return err
	}
	claimPath := filepath.Join(containerStatePath, "physical-game.encoding")
	claim, claimIdentity, err := createGameProbeSignal(claimPath)
	if err != nil {
		return err
	}
	defer func() { result = errors.Join(result, removeGameProbeSignal(claimPath, claimIdentity)); claim.Close() }()
	name, err := seatruntime.CaptureMediaSocketName(config.RuntimeNamespace)
	if err != nil {
		return err
	}
	parent, stop := signal.NotifyContext(context.Background(), syscall.SIGTERM, syscall.SIGINT)
	defer stop()
	ctx, cancel := context.WithTimeout(parent, 12*time.Second)
	defer cancel()
	command := exec.CommandContext(ctx, "/usr/libexec/polaris-seat/encoded-game-check", filepath.Join("/run/polaris", name),
		strconv.FormatUint(uint64(config.DisplayWidth), 10), strconv.FormatUint(uint64(config.DisplayHeight), 10), config.RenderNode)
	command.Env = []string{"HOME=/nonexistent", "LC_ALL=C", "XDG_RUNTIME_DIR=/run/polaris", "GST_REGISTRY=/dev/null", "GST_REGISTRY_1_0=/dev/null", "GST_PLUGIN_PATH=", "GST_PLUGIN_PATH_1_0="}
	command.SysProcAttr = &syscall.SysProcAttr{Pdeathsig: syscall.SIGKILL}
	command.WaitDelay = 100 * time.Millisecond
	var output, diagnostic gameProbeOutput
	command.Stdout = &output
	command.Stderr = &diagnostic
	if err := command.Run(); err != nil {
		_ = json.NewEncoder(os.Stdout).Encode(map[string]string{"error": "encoded probe failed", "observation": output.String(), "diagnostic": diagnostic.String()})
		return errors.New("worker game encoding or decoding failed")
	}
	observation, err := parseEncodedGameObservation(output.Bytes(), config.DisplayWidth, config.DisplayHeight)
	if err != nil {
		return err
	}
	if after, err := identityOf(path); err != nil || after != identity {
		return errors.New("physical game retired during encoded observation")
	}
	if err := checkHealth(config, productionPaths(), uint32(os.Geteuid())); err != nil {
		return err
	}
	return json.NewEncoder(os.Stdout).Encode(observation)
}
