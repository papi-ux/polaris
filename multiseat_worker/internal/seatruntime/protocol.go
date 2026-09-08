package seatruntime

import (
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"path/filepath"
	"slices"
	"strconv"
	"strings"
)

const (
	ReadyFDSetting     = "POLARIS_RUNTIME_READY_FD"
	ReadyRecord        = "POLARIS-RUNTIME-READY/1\n"
	DefaultCatalogPath = "/run/polaris-auth/runtime-providers.json"
)

type Stage string

const (
	StageSessionBus       Stage = "session-bus"
	StageAudio            Stage = "audio"
	StageDisplayCapture   Stage = "display-capture"
	StageNestedCompositor Stage = "nested-compositor"
	StageVirtualInput     Stage = "virtual-input"
	StageEncoder          Stage = "encoder"
	StageLauncher         Stage = "launcher-process-tree"
)

type WorkloadKind string

const (
	WorkloadGamescope WorkloadKind = "gamescope"
	WorkloadSteam     WorkloadKind = "steam"
	WorkloadHeroic    WorkloadKind = "heroic"
	WorkloadLutris    WorkloadKind = "lutris"
)

const (
	DisplayTopologyCaptureHostNested = "capture-host-with-nested-compositor"
	MediaPipelineWorkerLocal         = "worker-local-capture-encode"
	captureMediaSocketDomain         = "polaris-capture-media-v1\x00"
	captureMediaSocketPrefix         = "polaris-frames-"
)

// CaptureMediaSocketName derives the fixed worker-local raw-frame endpoint
// shared by the display-capture and encoder providers. The controller does not
// need authority over this internal transport, and the digest keeps its Unix
// socket path bounded even when the opaque runtime namespace is long.
func CaptureMediaSocketName(runtimeNamespace string) (string, error) {
	if !validNameToken(runtimeNamespace, 128) {
		return "", errors.New("runtime namespace is invalid")
	}
	digest := sha256.Sum256([]byte(captureMediaSocketDomain + runtimeNamespace))
	return captureMediaSocketPrefix + hex.EncodeToString(digest[:]), nil
}

// Request is the complete least-authority contract for exactly one runtime
// resource. Fields that do not belong to the selected stage must remain zero.
type Request struct {
	Stage                    Stage
	RuntimeNamespace         string
	AudioSink                string
	CaptureWaylandSocket     string
	ParentWaylandSocket      string
	WaylandSocket            string
	RenderNode               string
	DisplayTopology          string
	MediaPipeline            string
	DisplayWidth             uint32
	DisplayHeight            uint32
	DisplayRefreshMillihertz uint32
	DisplayHDR               bool
	Compositor               string
	InputSeat                string
	LogicalGPU               string
	EncoderSessions          uint32
	RuntimeProfile           string
	WorkloadKind             WorkloadKind
	WorkloadID               string
}

func validStage(stage Stage) bool {
	switch stage {
	case StageSessionBus, StageAudio, StageDisplayCapture,
		StageNestedCompositor, StageVirtualInput, StageEncoder,
		StageLauncher:
		return true
	default:
		return false
	}
}

func asciiAlphaNumeric(value byte) bool {
	return value >= 'a' && value <= 'z' ||
		value >= 'A' && value <= 'Z' ||
		value >= '0' && value <= '9'
}

func validNameToken(value string, maximum int) bool {
	if len(value) == 0 || len(value) > maximum || !asciiAlphaNumeric(value[0]) {
		return false
	}
	for index := 1; index < len(value); index++ {
		character := value[index]
		if !asciiAlphaNumeric(character) && character != '-' &&
			character != '_' && character != '.' {
			return false
		}
	}
	return true
}

func validRenderNode(path string) bool {
	return filepath.IsAbs(path) && filepath.Clean(path) == path &&
		strings.HasPrefix(path, "/dev/") &&
		!strings.ContainsAny(path, "\x00\n\r")
}

func validCompositor(compositor string) bool {
	switch compositor {
	case "gamescope", "sway", "labwc":
		return true
	default:
		return false
	}
}

func validWorkloadKind(kind WorkloadKind) bool {
	switch kind {
	case WorkloadGamescope, WorkloadSteam, WorkloadHeroic, WorkloadLutris:
		return true
	default:
		return false
	}
}

func validRuntimeProfile(profile string) bool {
	return validWorkloadKind(WorkloadKind(profile))
}

func validateRequest(request Request) error {
	if !validStage(request.Stage) ||
		!validNameToken(request.RuntimeNamespace, 128) {
		return errors.New("runtime request is invalid")
	}
	base := Request{
		Stage:            request.Stage,
		RuntimeNamespace: request.RuntimeNamespace,
	}
	switch request.Stage {
	case StageSessionBus:
		if request != base {
			return errors.New("runtime request contains authority outside its stage")
		}
	case StageAudio:
		base.AudioSink = request.AudioSink
		if request != base || !validNameToken(request.AudioSink, 128) {
			return errors.New("runtime audio request is invalid")
		}
	case StageDisplayCapture:
		base.InputSeat = request.InputSeat
		base.CaptureWaylandSocket = request.CaptureWaylandSocket
		base.RenderNode = request.RenderNode
		base.DisplayTopology = request.DisplayTopology
		base.MediaPipeline = request.MediaPipeline
		base.DisplayWidth = request.DisplayWidth
		base.DisplayHeight = request.DisplayHeight
		base.DisplayRefreshMillihertz = request.DisplayRefreshMillihertz
		base.DisplayHDR = request.DisplayHDR
		if request != base ||
			(request.InputSeat != "" && !validNameToken(request.InputSeat, 128)) ||
			!validNameToken(request.CaptureWaylandSocket, 128) ||
			!validRenderNode(request.RenderNode) ||
			request.DisplayTopology != DisplayTopologyCaptureHostNested ||
			request.MediaPipeline != MediaPipelineWorkerLocal ||
			request.DisplayWidth == 0 || request.DisplayWidth > 16384 ||
			request.DisplayHeight == 0 || request.DisplayHeight > 16384 ||
			request.DisplayRefreshMillihertz < 1000 ||
			request.DisplayRefreshMillihertz > 1000000 {
			return errors.New("runtime display request is invalid")
		}
	case StageNestedCompositor:
		base.ParentWaylandSocket = request.ParentWaylandSocket
		base.WaylandSocket = request.WaylandSocket
		base.RenderNode = request.RenderNode
		base.DisplayWidth = request.DisplayWidth
		base.DisplayHeight = request.DisplayHeight
		base.DisplayRefreshMillihertz = request.DisplayRefreshMillihertz
		base.DisplayHDR = request.DisplayHDR
		base.Compositor = request.Compositor
		if request != base ||
			!validNameToken(request.ParentWaylandSocket, 128) ||
			!validNameToken(request.WaylandSocket, 128) ||
			request.ParentWaylandSocket == request.WaylandSocket ||
			!validRenderNode(request.RenderNode) ||
			request.DisplayWidth == 0 || request.DisplayWidth > 16384 ||
			request.DisplayHeight == 0 || request.DisplayHeight > 16384 ||
			request.DisplayRefreshMillihertz < 1000 ||
			request.DisplayRefreshMillihertz > 1000000 ||
			!validCompositor(request.Compositor) {
			return errors.New("runtime compositor request is invalid")
		}
	case StageVirtualInput:
		base.InputSeat = request.InputSeat
		if request != base || !validNameToken(request.InputSeat, 128) {
			return errors.New("runtime input request is invalid")
		}
	case StageEncoder:
		base.LogicalGPU = request.LogicalGPU
		base.RenderNode = request.RenderNode
		base.EncoderSessions = request.EncoderSessions
		base.MediaPipeline = request.MediaPipeline
		if request != base ||
			!validNameToken(request.LogicalGPU, 128) ||
			!validRenderNode(request.RenderNode) ||
			request.EncoderSessions == 0 || request.EncoderSessions > 64 ||
			request.MediaPipeline != MediaPipelineWorkerLocal {
			return errors.New("runtime encoder request is invalid")
		}
	case StageLauncher:
		base.RuntimeProfile = request.RuntimeProfile
		base.WorkloadKind = request.WorkloadKind
		base.WorkloadID = request.WorkloadID
		base.WaylandSocket = request.WaylandSocket
		base.AudioSink = request.AudioSink
		base.InputSeat = request.InputSeat
		if request != base || !validRuntimeProfile(request.RuntimeProfile) ||
			!validWorkloadKind(request.WorkloadKind) ||
			request.RuntimeProfile != string(request.WorkloadKind) ||
			!validNameToken(request.WorkloadID, 128) ||
			!validNameToken(request.WaylandSocket, 128) ||
			!validNameToken(request.AudioSink, 128) ||
			!validNameToken(request.InputSeat, 128) {
			return errors.New("runtime launcher request is invalid")
		}
	default:
		return errors.New("runtime request is invalid")
	}
	return nil
}

func canonicalUint(value uint32) string {
	return strconv.FormatUint(uint64(value), 10)
}

// Arguments returns the one canonical argv accepted by the runtime helper.
func Arguments(request Request) ([]string, error) {
	if err := validateRequest(request); err != nil {
		return nil, err
	}
	arguments := []string{
		"serve",
		"--stage=" + string(request.Stage),
		"--runtime-namespace=" + request.RuntimeNamespace,
	}
	switch request.Stage {
	case StageSessionBus:
		return arguments, nil
	case StageAudio:
		return append(arguments, "--audio-sink="+request.AudioSink), nil
	case StageDisplayCapture:
		hdr := "0"
		if request.DisplayHDR {
			hdr = "1"
		}
		arguments = append(arguments,
			"--capture-wayland-socket="+request.CaptureWaylandSocket,
			"--render-node="+request.RenderNode,
			"--display-topology="+request.DisplayTopology,
			"--media-pipeline="+request.MediaPipeline,
			"--display-width="+canonicalUint(request.DisplayWidth),
			"--display-height="+canonicalUint(request.DisplayHeight),
			"--display-refresh-millihz="+canonicalUint(request.DisplayRefreshMillihertz),
			"--display-hdr="+hdr,
		)
		if request.InputSeat != "" {
			arguments = append(arguments, "--input-seat="+request.InputSeat)
		}
		return arguments, nil
	case StageNestedCompositor:
		hdr := "0"
		if request.DisplayHDR {
			hdr = "1"
		}
		return append(arguments,
			"--parent-wayland-socket="+request.ParentWaylandSocket,
			"--wayland-socket="+request.WaylandSocket,
			"--render-node="+request.RenderNode,
			"--display-width="+canonicalUint(request.DisplayWidth),
			"--display-height="+canonicalUint(request.DisplayHeight),
			"--display-refresh-millihz="+canonicalUint(request.DisplayRefreshMillihertz),
			"--display-hdr="+hdr,
			"--compositor="+request.Compositor,
		), nil
	case StageVirtualInput:
		return append(arguments, "--input-seat="+request.InputSeat), nil
	case StageEncoder:
		return append(arguments,
			"--logical-gpu-id="+request.LogicalGPU,
			"--render-node="+request.RenderNode,
			"--sessions="+canonicalUint(request.EncoderSessions),
			"--media-pipeline="+request.MediaPipeline,
		), nil
	case StageLauncher:
		return append(arguments,
			"--runtime-profile="+request.RuntimeProfile,
			"--workload-kind="+string(request.WorkloadKind),
			"--workload-id="+request.WorkloadID,
			"--wayland-socket="+request.WaylandSocket,
			"--audio-sink="+request.AudioSink,
			"--input-seat="+request.InputSeat,
		), nil
	default:
		return nil, errors.New("runtime request is invalid")
	}
}

// Environment returns the exact stage environment, excluding the readiness
// descriptor that the process host appends when it starts the helper.
func Environment(request Request) ([]string, error) {
	if err := validateRequest(request); err != nil {
		return nil, err
	}
	runtime := []string{"XDG_RUNTIME_DIR=/run/polaris"}
	dbus := "DBUS_SESSION_BUS_ADDRESS=unix:path=/run/polaris/bus"
	switch request.Stage {
	case StageSessionBus:
		return append(runtime, dbus), nil
	case StageAudio:
		return append(runtime,
			dbus,
			"PIPEWIRE_RUNTIME_DIR=/run/polaris",
			"PIPEWIRE_NODE="+request.AudioSink,
			"PULSE_SERVER=unix:/run/polaris/pulse/native",
			"PULSE_SINK="+request.AudioSink,
		), nil
	case StageDisplayCapture:
		return append(runtime,
			dbus,
			"POLARIS_RENDER_NODE="+request.RenderNode,
		), nil
	case StageNestedCompositor:
		return append(runtime,
			dbus,
			"WAYLAND_DISPLAY="+request.ParentWaylandSocket,
			"POLARIS_RENDER_NODE="+request.RenderNode,
		), nil
	case StageVirtualInput:
		return append(runtime,
			"WAYLAND_DISPLAY="+request.WaylandSocket,
			"POLARIS_INPUT_SEAT="+request.InputSeat,
		), nil
	case StageEncoder:
		return []string{"POLARIS_RENDER_NODE=" + request.RenderNode}, nil
	case StageLauncher:
		return append(runtime,
			"HOME=/var/lib/polaris-seat",
			"XDG_CONFIG_HOME=/var/lib/polaris-seat/.config",
			"XDG_CACHE_HOME=/var/lib/polaris-seat/.cache",
			"XDG_DATA_HOME=/var/lib/polaris-seat/.local/share",
			dbus,
			"PIPEWIRE_RUNTIME_DIR=/run/polaris",
			"PIPEWIRE_NODE="+request.AudioSink,
			"PULSE_SERVER=unix:/run/polaris/pulse/native",
			"PULSE_SINK="+request.AudioSink,
			"WAYLAND_DISPLAY="+request.WaylandSocket,
			"POLARIS_INPUT_SEAT="+request.InputSeat,
			"POLARIS_RENDER_NODE="+request.RenderNode,
			"POLARIS_RUNTIME_PROFILE="+request.RuntimeProfile,
		), nil
	default:
		return nil, errors.New("runtime request is invalid")
	}
}

func argumentValue(arguments []string, index int, prefix string) (string, error) {
	if index >= len(arguments) || !strings.HasPrefix(arguments[index], prefix) {
		return "", errors.New("runtime helper argv is invalid")
	}
	value := strings.TrimPrefix(arguments[index], prefix)
	if value == "" {
		return "", errors.New("runtime helper argv is invalid")
	}
	return value, nil
}

func parseCanonicalUint(value string, bits int) (uint64, error) {
	if value == "" || strings.HasPrefix(value, "+") ||
		(len(value) > 1 && value[0] == '0') {
		return 0, errors.New("runtime helper decimal is invalid")
	}
	parsed, err := strconv.ParseUint(value, 10, bits)
	if err != nil {
		return 0, errors.New("runtime helper decimal is invalid")
	}
	return parsed, nil
}

func parseStage(value string) (Stage, error) {
	stage := Stage(value)
	if !validStage(stage) {
		return "", errors.New("runtime helper stage is invalid")
	}
	return stage, nil
}

func parseArguments(arguments []string) (Request, error) {
	if len(arguments) < 3 || arguments[0] != "serve" {
		return Request{}, errors.New("runtime helper argv is invalid")
	}
	stageValue, err := argumentValue(arguments, 1, "--stage=")
	if err != nil {
		return Request{}, err
	}
	stage, err := parseStage(stageValue)
	if err != nil {
		return Request{}, err
	}
	runtimeNamespace, err := argumentValue(arguments, 2, "--runtime-namespace=")
	if err != nil {
		return Request{}, err
	}
	request := Request{Stage: stage, RuntimeNamespace: runtimeNamespace}
	value := func(index int, prefix string) (string, error) {
		return argumentValue(arguments, index, prefix)
	}
	switch stage {
	case StageSessionBus:
		if len(arguments) != 3 {
			return Request{}, errors.New("runtime helper argv is invalid")
		}
	case StageAudio:
		if len(arguments) != 4 {
			return Request{}, errors.New("runtime helper argv is invalid")
		}
		request.AudioSink, err = value(3, "--audio-sink=")
	case StageDisplayCapture:
		if len(arguments) != 11 && len(arguments) != 12 {
			return Request{}, errors.New("runtime helper argv is invalid")
		}
		if request.CaptureWaylandSocket, err = value(3, "--capture-wayland-socket="); err == nil {
			request.RenderNode, err = value(4, "--render-node=")
		}
		if err == nil {
			request.DisplayTopology, err = value(5, "--display-topology=")
		}
		if err == nil {
			request.MediaPipeline, err = value(6, "--media-pipeline=")
		}
		var parsed uint64
		if err == nil {
			var raw string
			raw, err = value(7, "--display-width=")
			if err == nil {
				parsed, err = parseCanonicalUint(raw, 32)
				request.DisplayWidth = uint32(parsed)
			}
		}
		if err == nil {
			var raw string
			raw, err = value(8, "--display-height=")
			if err == nil {
				parsed, err = parseCanonicalUint(raw, 32)
				request.DisplayHeight = uint32(parsed)
			}
		}
		if err == nil {
			var raw string
			raw, err = value(9, "--display-refresh-millihz=")
			if err == nil {
				parsed, err = parseCanonicalUint(raw, 32)
				request.DisplayRefreshMillihertz = uint32(parsed)
			}
		}
		if err == nil {
			var raw string
			raw, err = value(10, "--display-hdr=")
			switch raw {
			case "0":
				request.DisplayHDR = false
			case "1":
				request.DisplayHDR = true
			default:
				err = errors.New("runtime helper HDR flag is invalid")
			}
		}
		if err == nil && len(arguments) == 12 {
			request.InputSeat, err = value(11, "--input-seat=")
		}
	case StageNestedCompositor:
		if len(arguments) != 11 {
			return Request{}, errors.New("runtime helper argv is invalid")
		}
		if request.ParentWaylandSocket, err = value(3, "--parent-wayland-socket="); err == nil {
			request.WaylandSocket, err = value(4, "--wayland-socket=")
		}
		if err == nil {
			request.RenderNode, err = value(5, "--render-node=")
		}
		if err == nil {
			var raw string
			raw, err = value(6, "--display-width=")
			if err == nil {
				parsed, parseError := parseCanonicalUint(raw, 32)
				err = parseError
				request.DisplayWidth = uint32(parsed)
			}
		}
		if err == nil {
			var raw string
			raw, err = value(7, "--display-height=")
			if err == nil {
				parsed, parseError := parseCanonicalUint(raw, 32)
				err = parseError
				request.DisplayHeight = uint32(parsed)
			}
		}
		if err == nil {
			var raw string
			raw, err = value(8, "--display-refresh-millihz=")
			if err == nil {
				parsed, parseError := parseCanonicalUint(raw, 32)
				err = parseError
				request.DisplayRefreshMillihertz = uint32(parsed)
			}
		}
		if err == nil {
			var raw string
			raw, err = value(9, "--display-hdr=")
			switch raw {
			case "0":
				request.DisplayHDR = false
			case "1":
				request.DisplayHDR = true
			default:
				err = errors.New("runtime helper HDR flag is invalid")
			}
		}
		if err == nil {
			request.Compositor, err = value(10, "--compositor=")
		}
	case StageVirtualInput:
		if len(arguments) != 4 {
			return Request{}, errors.New("runtime helper argv is invalid")
		}
		request.InputSeat, err = value(3, "--input-seat=")
	case StageEncoder:
		if len(arguments) != 7 {
			return Request{}, errors.New("runtime helper argv is invalid")
		}
		if request.LogicalGPU, err = value(3, "--logical-gpu-id="); err == nil {
			request.RenderNode, err = value(4, "--render-node=")
		}
		if err == nil {
			var raw string
			raw, err = value(5, "--sessions=")
			if err == nil {
				parsed, parseError := parseCanonicalUint(raw, 32)
				err = parseError
				request.EncoderSessions = uint32(parsed)
			}
		}
		if err == nil {
			request.MediaPipeline, err = value(6, "--media-pipeline=")
		}
	case StageLauncher:
		if len(arguments) != 9 {
			return Request{}, errors.New("runtime helper argv is invalid")
		}
		if request.RuntimeProfile, err = value(3, "--runtime-profile="); err == nil {
			var workloadKind string
			workloadKind, err = value(4, "--workload-kind=")
			request.WorkloadKind = WorkloadKind(workloadKind)
		}
		if err == nil {
			request.WorkloadID, err = value(5, "--workload-id=")
		}
		if err == nil {
			request.WaylandSocket, err = value(6, "--wayland-socket=")
		}
		if err == nil {
			request.AudioSink, err = value(7, "--audio-sink=")
		}
		if err == nil {
			request.InputSeat, err = value(8, "--input-seat=")
		}
	}
	if err != nil || validateRequest(request) != nil {
		return Request{}, errors.New("runtime helper argv is invalid")
	}
	canonical, err := Arguments(request)
	if err != nil || !slices.Equal(canonical, arguments) {
		return Request{}, errors.New("runtime helper argv is not canonical")
	}
	return request, nil
}

func validateEnvironment(request Request, environment []string) error {
	expected, err := Environment(request)
	if err != nil {
		return err
	}
	expected = append(expected, ReadyFDSetting+"=3")
	if len(environment) != len(expected) {
		return errors.New("runtime helper environment is invalid")
	}
	values := make(map[string]string, len(environment))
	for _, setting := range environment {
		name, value, present := strings.Cut(setting, "=")
		if !present || name == "" || strings.ContainsAny(setting, "\x00\n\r") {
			return errors.New("runtime helper environment is invalid")
		}
		if _, duplicate := values[name]; duplicate {
			return errors.New("runtime helper environment is invalid")
		}
		values[name] = value
	}
	for _, setting := range expected {
		name, value, _ := strings.Cut(setting, "=")
		if actual, present := values[name]; !present || actual != value {
			return errors.New("runtime helper environment is invalid")
		}
	}
	return nil
}

// ParseInvocation rejects any non-canonical argv or ambient environment before
// a provider catalog is consulted.
func ParseInvocation(arguments []string, environment []string) (Request, error) {
	request, err := parseArguments(arguments)
	if err != nil {
		return Request{}, err
	}
	if err := validateEnvironment(request, environment); err != nil {
		return Request{}, err
	}
	return request, nil
}

func selectorFor(request Request) string {
	switch request.Stage {
	case StageNestedCompositor:
		return request.Compositor
	case StageLauncher:
		return string(request.WorkloadKind)
	default:
		return ""
	}
}
