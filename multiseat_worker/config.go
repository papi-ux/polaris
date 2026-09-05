package main

import (
	"errors"
	"fmt"
	"path/filepath"
	"strconv"
	"strings"
)

const (
	containerIPCPath   = "/run/polaris-ipc"
	containerAuthPath  = "/run/polaris-auth"
	containerStatePath = "/run/polaris"
	capabilityFileName = "auth-token"
	controlSocketName  = "control.sock"
	mediaSocketName    = "media.sock"
	readyFileName      = "seat-worker.ready"
)

type workerConfig struct {
	Identity         endpointIdentity
	RuntimeNamespace string
	WaylandSocket    string
	AudioSink        string
	InputSeat        string
	RenderNode       string
	Compositor       string
	RuntimeProfile   string
	DisplayWidth     uint32
	DisplayHeight    uint32
	RefreshMillihz   uint32
	DisplayHDR       bool
	EncoderSessions  uint32
	WorkloadKey      string
}

type workerPaths struct {
	IPC   string
	Auth  string
	State string
}

func productionPaths() workerPaths {
	return workerPaths{IPC: containerIPCPath, Auth: containerAuthPath, State: containerStatePath}
}

func requiredEnvironment(lookup func(string) (string, bool), name string) (string, error) {
	value, present := lookup(name)
	if !present || value == "" {
		return "", fmt.Errorf("required worker setting %s is missing", name)
	}
	return value, nil
}

func parseUint(value string, bits int, field string) (uint64, error) {
	if value == "" || strings.HasPrefix(value, "+") ||
		(len(value) > 1 && value[0] == '0') {
		return 0, fmt.Errorf("worker setting %s is not canonical decimal", field)
	}
	parsed, err := strconv.ParseUint(value, 10, bits)
	if err != nil {
		return 0, fmt.Errorf("worker setting %s is invalid", field)
	}
	return parsed, nil
}

func validOpaqueReference(value string) bool {
	if value == "" || len(value) > 256 {
		return false
	}
	for _, character := range []byte(value) {
		if character < 0x20 || character > 0x7e {
			return false
		}
	}
	return true
}

func validRuntimeProfile(profile string) bool {
	switch profile {
	case "gamescope", "steam", "heroic", "lutris":
		return true
	default:
		return false
	}
}

func loadWorkerConfig(
	lookup func(string) (string, bool),
	workloadKey string,
) (workerConfig, error) {
	var config workerConfig
	var err error
	if config.Identity.ControllerEpoch, err = requiredEnvironment(lookup, "POLARIS_CONTROLLER_EPOCH"); err != nil {
		return config, err
	}
	if config.Identity.LogicalGPU, err = requiredEnvironment(lookup, "POLARIS_LOGICAL_GPU_ID"); err != nil {
		return config, err
	}
	if config.Identity.WorkerName, err = requiredEnvironment(lookup, "POLARIS_WORKER_NAME"); err != nil {
		return config, err
	}
	slot, err := requiredEnvironment(lookup, "POLARIS_SEAT_SLOT")
	if err != nil {
		return config, err
	}
	parsedSlot, err := parseUint(slot, 32, "POLARIS_SEAT_SLOT")
	if err != nil {
		return config, err
	}
	config.Identity.Slot = uint32(parsedSlot)
	generation, err := requiredEnvironment(lookup, "POLARIS_SEAT_GENERATION")
	if err != nil {
		return config, err
	}
	config.Identity.Generation, err = parseUint(generation, 64, "POLARIS_SEAT_GENERATION")
	if err != nil || config.Identity.Generation == 0 {
		return config, errors.New("worker generation must be positive canonical decimal")
	}
	if !validIdentity(config.Identity) {
		return config, errors.New("worker identity is invalid")
	}

	settings := []struct {
		name   string
		target *string
	}{
		{"POLARIS_RUNTIME_NAMESPACE", &config.RuntimeNamespace},
		{"WAYLAND_DISPLAY", &config.WaylandSocket},
		{"PULSE_SINK", &config.AudioSink},
		{"POLARIS_INPUT_SEAT", &config.InputSeat},
		{"POLARIS_RENDER_NODE", &config.RenderNode},
		{"POLARIS_COMPOSITOR", &config.Compositor},
	}
	for _, setting := range settings {
		if *setting.target, err = requiredEnvironment(lookup, setting.name); err != nil {
			return config, err
		}
	}
	if !validNameToken(config.RuntimeNamespace, 128) ||
		!validNameToken(config.WaylandSocket, 128) ||
		!validNameToken(config.AudioSink, 128) ||
		!validNameToken(config.InputSeat, 128) {
		return config, errors.New("worker resource name is invalid")
	}
	if !filepath.IsAbs(config.RenderNode) || filepath.Clean(config.RenderNode) != config.RenderNode ||
		!strings.HasPrefix(config.RenderNode, "/dev/") {
		return config, errors.New("worker render node is invalid")
	}
	if config.Compositor != "gamescope" && config.Compositor != "sway" && config.Compositor != "labwc" {
		return config, errors.New("worker compositor is not concrete")
	}
	if config.RuntimeProfile, err = requiredEnvironment(lookup, "POLARIS_RUNTIME_PROFILE"); err != nil {
		return config, err
	}
	if !validRuntimeProfile(config.RuntimeProfile) {
		return config, errors.New("worker runtime profile is invalid")
	}
	displaySettings := []struct {
		name   string
		target *uint32
		max    uint64
	}{
		{"POLARIS_DISPLAY_WIDTH", &config.DisplayWidth, 16384},
		{"POLARIS_DISPLAY_HEIGHT", &config.DisplayHeight, 16384},
		{"POLARIS_DISPLAY_REFRESH_MILLIHZ", &config.RefreshMillihz, 1000000},
	}
	for _, setting := range displaySettings {
		value, valueError := requiredEnvironment(lookup, setting.name)
		if valueError != nil {
			return config, valueError
		}
		parsed, valueError := parseUint(value, 32, setting.name)
		if valueError != nil {
			return config, valueError
		}
		if parsed == 0 || parsed > setting.max ||
			(setting.name == "POLARIS_DISPLAY_REFRESH_MILLIHZ" && parsed < 1000) {
			return config, fmt.Errorf("worker setting %s is outside the supported range", setting.name)
		}
		*setting.target = uint32(parsed)
	}
	hdr, err := requiredEnvironment(lookup, "POLARIS_DISPLAY_HDR")
	if err != nil {
		return config, err
	}
	switch hdr {
	case "0":
		config.DisplayHDR = false
	case "1":
		config.DisplayHDR = true
	default:
		return config, errors.New("worker display HDR setting is not canonical boolean")
	}
	encoders, err := requiredEnvironment(lookup, "POLARIS_ENCODER_SESSIONS")
	if err != nil {
		return config, err
	}
	parsedEncoders, err := parseUint(encoders, 32, "POLARIS_ENCODER_SESSIONS")
	if err != nil || parsedEncoders == 0 || parsedEncoders > 64 {
		return config, errors.New("worker encoder allocation is invalid")
	}
	config.EncoderSessions = uint32(parsedEncoders)
	if workloadKey != "" && !validOpaqueReference(workloadKey) {
		return config, errors.New("worker workload key is invalid")
	}
	config.WorkloadKey = workloadKey
	return config, nil
}

func parseRunArguments(arguments []string) (string, error) {
	if len(arguments) != 1 || !strings.HasPrefix(arguments[0], "--workload-key=") {
		return "", errors.New("run requires exactly one workload key")
	}
	value := strings.TrimPrefix(arguments[0], "--workload-key=")
	if !validOpaqueReference(value) {
		return "", errors.New("workload key is invalid")
	}
	return value, nil
}
