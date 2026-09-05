package main

import (
	"context"
	"errors"
	"path/filepath"
	"strconv"
	"strings"
)

const (
	defaultRuntimeHelperPath = "/usr/bin/polaris-seat-runtime"
	runtimeReadyFDSetting    = "POLARIS_RUNTIME_READY_FD"
	runtimeReadyRecord       = "POLARIS-RUNTIME-READY/1\n"
)

type runtimeProcessSpec struct {
	Stage       runtimeStage
	Executable  string
	Arguments   []string
	Environment []string
}

// runtimeProcessHost is the only operating-system boundary used by the
// process-backed adapters. Start must not invoke a shell. It returns only
// after the helper writes the exact readiness record on its inherited ready
// descriptor, and the returned lease owns the helper's complete process group.
type runtimeProcessHost interface {
	Start(context.Context, runtimeProcessSpec) (runtimeLease, error)
}

type processRuntimeAdapterOptions struct {
	HelperExecutable string
}

type processRuntimeAdapter struct {
	stage   runtimeStage
	host    runtimeProcessHost
	options processRuntimeAdapterOptions
}

func validRuntimeStage(stage runtimeStage) bool {
	return stage >= runtimeStageSessionBus &&
		stage <= runtimeStageLauncherProcessTree
}

func validRuntimeHelperPath(path string) bool {
	return filepath.IsAbs(path) &&
		filepath.Clean(path) == path &&
		strings.HasPrefix(path, "/") &&
		!strings.ContainsAny(path, "\x00\n\r")
}

func normalizeProcessRuntimeAdapterOptions(
	options processRuntimeAdapterOptions,
) (processRuntimeAdapterOptions, error) {
	if options.HelperExecutable == "" {
		options.HelperExecutable = defaultRuntimeHelperPath
	}
	if !validRuntimeHelperPath(options.HelperExecutable) {
		return processRuntimeAdapterOptions{}, errors.New("worker runtime helper path is invalid")
	}
	return options, nil
}

func newProcessRuntimeAdapters(
	host runtimeProcessHost,
	options processRuntimeAdapterOptions,
) (runtimeAdapters, error) {
	if nilRuntimeInterface(host) {
		return runtimeAdapters{}, errors.New("worker runtime process host is missing")
	}
	normalized, err := normalizeProcessRuntimeAdapterOptions(options)
	if err != nil {
		return runtimeAdapters{}, err
	}
	adapter := func(stage runtimeStage) runtimeAdapter {
		return &processRuntimeAdapter{
			stage:   stage,
			host:    host,
			options: normalized,
		}
	}
	return runtimeAdapters{
		SessionBus:          adapter(runtimeStageSessionBus),
		Audio:               adapter(runtimeStageAudio),
		DisplayCapture:      adapter(runtimeStageDisplayCapture),
		NestedCompositor:    adapter(runtimeStageNestedCompositor),
		VirtualInput:        adapter(runtimeStageVirtualInput),
		Encoder:             adapter(runtimeStageEncoder),
		LauncherProcessTree: adapter(runtimeStageLauncherProcessTree),
	}, nil
}

func runtimeProcessEnvironment(
	stage runtimeStage,
	allocation runtimeAllocation,
) ([]string, error) {
	if !validRuntimeStage(stage) || !validRuntimeProfile(allocation.RuntimeProfile) {
		return nil, errors.New("worker runtime helper stage is invalid")
	}
	runtime := []string{"XDG_RUNTIME_DIR=/run/polaris"}
	dbus := "DBUS_SESSION_BUS_ADDRESS=unix:path=/run/polaris/bus"
	switch stage {
	case runtimeStageSessionBus:
		return append(runtime, dbus), nil
	case runtimeStageAudio:
		return append(runtime,
			dbus,
			"PIPEWIRE_RUNTIME_DIR=/run/polaris",
			"PULSE_SERVER=unix:/run/polaris/pulse/native",
			"PULSE_SINK="+allocation.AudioSink,
		), nil
	case runtimeStageDisplayCapture:
		return append(runtime,
			dbus,
			"POLARIS_RENDER_NODE="+allocation.RenderNode,
		), nil
	case runtimeStageNestedCompositor:
		return append(runtime,
			dbus,
			"WAYLAND_DISPLAY="+allocation.CaptureWaylandSocket,
			"POLARIS_RENDER_NODE="+allocation.RenderNode,
		), nil
	case runtimeStageVirtualInput:
		return append(runtime,
			"WAYLAND_DISPLAY="+allocation.WaylandSocket,
			"POLARIS_INPUT_SEAT="+allocation.InputSeat,
		), nil
	case runtimeStageEncoder:
		return []string{
			"POLARIS_RENDER_NODE=" + allocation.RenderNode,
		}, nil
	case runtimeStageLauncherProcessTree:
		return append(runtime,
			"HOME=/var/lib/polaris-seat",
			"XDG_CONFIG_HOME=/var/lib/polaris-seat/.config",
			"XDG_CACHE_HOME=/var/lib/polaris-seat/.cache",
			"XDG_DATA_HOME=/var/lib/polaris-seat/.local/share",
			dbus,
			"PIPEWIRE_RUNTIME_DIR=/run/polaris",
			"PULSE_SERVER=unix:/run/polaris/pulse/native",
			"PULSE_SINK="+allocation.AudioSink,
			"WAYLAND_DISPLAY="+allocation.WaylandSocket,
			"POLARIS_INPUT_SEAT="+allocation.InputSeat,
			"POLARIS_RENDER_NODE="+allocation.RenderNode,
			"POLARIS_RUNTIME_PROFILE="+allocation.RuntimeProfile,
		), nil
	default:
		return nil, errors.New("worker runtime helper stage is invalid")
	}
}

func runtimeProcessArguments(
	stage runtimeStage,
	allocation runtimeAllocation,
) ([]string, error) {
	if !validRuntimeStage(stage) || !validRuntimeProfile(allocation.RuntimeProfile) {
		return nil, errors.New("worker runtime helper stage is invalid")
	}
	arguments := []string{
		"serve",
		"--stage=" + stage.String(),
		"--runtime-namespace=" + allocation.RuntimeNamespace,
	}
	displayHDR := "0"
	if allocation.DisplayHDR {
		displayHDR = "1"
	}
	switch stage {
	case runtimeStageSessionBus:
		return arguments, nil
	case runtimeStageAudio:
		return append(arguments,
			"--audio-sink="+allocation.AudioSink,
		), nil
	case runtimeStageDisplayCapture:
		return append(arguments,
			"--capture-wayland-socket="+allocation.CaptureWaylandSocket,
			"--render-node="+allocation.RenderNode,
			"--display-topology="+string(allocation.DisplayTopology),
			"--media-pipeline="+string(allocation.MediaPipeline),
			"--display-width="+strconv.FormatUint(uint64(allocation.DisplayWidth), 10),
			"--display-height="+strconv.FormatUint(uint64(allocation.DisplayHeight), 10),
			"--display-refresh-millihz="+strconv.FormatUint(uint64(allocation.RefreshMillihz), 10),
			"--display-hdr="+displayHDR,
		), nil
	case runtimeStageNestedCompositor:
		return append(arguments,
			"--parent-wayland-socket="+allocation.CaptureWaylandSocket,
			"--wayland-socket="+allocation.WaylandSocket,
			"--render-node="+allocation.RenderNode,
			"--compositor="+allocation.Compositor,
		), nil
	case runtimeStageVirtualInput:
		return append(arguments,
			"--input-seat="+allocation.InputSeat,
		), nil
	case runtimeStageEncoder:
		return append(arguments,
			"--logical-gpu-id="+allocation.Identity.LogicalGPU,
			"--render-node="+allocation.RenderNode,
			"--sessions="+strconv.FormatUint(uint64(allocation.EncoderSessions), 10),
			"--media-pipeline="+string(allocation.MediaPipeline),
		), nil
	case runtimeStageLauncherProcessTree:
		return append(arguments,
			"--runtime-profile="+allocation.RuntimeProfile,
			"--workload-kind="+string(allocation.Workload.Kind),
			"--workload-id="+allocation.Workload.TargetID,
			"--wayland-socket="+allocation.WaylandSocket,
			"--audio-sink="+allocation.AudioSink,
			"--input-seat="+allocation.InputSeat,
		), nil
	default:
		return nil, errors.New("worker runtime helper stage is invalid")
	}
}

func validProcessRuntimeAllocation(allocation runtimeAllocation) bool {
	validated, err := runtimeAllocationFromConfig(workerConfig{
		Identity:             allocation.Identity,
		RuntimeNamespace:     allocation.RuntimeNamespace,
		CaptureWaylandSocket: allocation.CaptureWaylandSocket,
		WaylandSocket:        allocation.WaylandSocket,
		AudioSink:            allocation.AudioSink,
		InputSeat:            allocation.InputSeat,
		RenderNode:           allocation.RenderNode,
		Compositor:           allocation.Compositor,
		RuntimeProfile:       allocation.RuntimeProfile,
		DisplayTopology:      allocation.DisplayTopology,
		MediaPipeline:        allocation.MediaPipeline,
		DisplayWidth:         allocation.DisplayWidth,
		DisplayHeight:        allocation.DisplayHeight,
		RefreshMillihz:       allocation.RefreshMillihz,
		DisplayHDR:           allocation.DisplayHDR,
		EncoderSessions:      allocation.EncoderSessions,
		Workload:             allocation.Workload,
	})
	return err == nil && validated == allocation
}

func (adapter *processRuntimeAdapter) Start(
	context context.Context,
	allocation runtimeAllocation,
) (runtimeLease, error) {
	if adapter == nil || nilRuntimeInterface(adapter.host) ||
		!validRuntimeStage(adapter.stage) ||
		!validProcessRuntimeAllocation(allocation) {
		return nil, errors.New("worker runtime process adapter is invalid")
	}
	arguments, err := runtimeProcessArguments(
		adapter.stage,
		allocation,
	)
	if err != nil {
		return nil, err
	}
	environment, err := runtimeProcessEnvironment(
		adapter.stage,
		allocation,
	)
	if err != nil {
		return nil, err
	}
	lease, err := adapter.host.Start(context, runtimeProcessSpec{
		Stage:       adapter.stage,
		Executable:  adapter.options.HelperExecutable,
		Arguments:   arguments,
		Environment: environment,
	})
	if err != nil {
		return lease, errors.New("runtime helper did not become ready")
	}
	return lease, nil
}
