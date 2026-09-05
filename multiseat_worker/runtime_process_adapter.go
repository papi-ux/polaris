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
	RuntimeProfile   string
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

func validRuntimeProfile(profile string) bool {
	switch profile {
	case "gamescope", "steam", "heroic", "lutris":
		return true
	default:
		return false
	}
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
	if !validRuntimeProfile(options.RuntimeProfile) {
		return processRuntimeAdapterOptions{}, errors.New("worker runtime profile is invalid")
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
		Compositor:          adapter(runtimeStageCompositor),
		VirtualInput:        adapter(runtimeStageVirtualInput),
		Capture:             adapter(runtimeStageCapture),
		EncoderLease:        adapter(runtimeStageEncoderLease),
		LauncherProcessTree: adapter(runtimeStageLauncherProcessTree),
	}, nil
}

func runtimeProcessEnvironment(
	stage runtimeStage,
	allocation runtimeAllocation,
	profile string,
) ([]string, error) {
	if !validRuntimeStage(stage) || !validRuntimeProfile(profile) {
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
	case runtimeStageCompositor:
		return append(runtime,
			dbus,
			"WAYLAND_DISPLAY="+allocation.WaylandSocket,
			"POLARIS_RENDER_NODE="+allocation.RenderNode,
		), nil
	case runtimeStageVirtualInput:
		return append(runtime,
			"WAYLAND_DISPLAY="+allocation.WaylandSocket,
			"POLARIS_INPUT_SEAT="+allocation.InputSeat,
		), nil
	case runtimeStageCapture:
		return append(runtime,
			"WAYLAND_DISPLAY="+allocation.WaylandSocket,
			"POLARIS_RENDER_NODE="+allocation.RenderNode,
		), nil
	case runtimeStageEncoderLease:
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
			"POLARIS_RUNTIME_PROFILE="+profile,
		), nil
	default:
		return nil, errors.New("worker runtime helper stage is invalid")
	}
}

func runtimeProcessArguments(
	stage runtimeStage,
	allocation runtimeAllocation,
	profile string,
) ([]string, error) {
	if !validRuntimeStage(stage) || !validRuntimeProfile(profile) {
		return nil, errors.New("worker runtime helper stage is invalid")
	}
	arguments := []string{
		"serve",
		"--stage=" + stage.String(),
		"--runtime-namespace=" + allocation.RuntimeNamespace,
	}
	switch stage {
	case runtimeStageSessionBus:
		return arguments, nil
	case runtimeStageAudio:
		return append(arguments,
			"--audio-sink="+allocation.AudioSink,
		), nil
	case runtimeStageCompositor:
		return append(arguments,
			"--wayland-socket="+allocation.WaylandSocket,
			"--render-node="+allocation.RenderNode,
			"--compositor="+allocation.Compositor,
		), nil
	case runtimeStageVirtualInput:
		return append(arguments,
			"--input-seat="+allocation.InputSeat,
		), nil
	case runtimeStageCapture:
		return append(arguments,
			"--wayland-socket="+allocation.WaylandSocket,
			"--render-node="+allocation.RenderNode,
		), nil
	case runtimeStageEncoderLease:
		return append(arguments,
			"--logical-gpu-id="+allocation.Identity.LogicalGPU,
			"--render-node="+allocation.RenderNode,
			"--sessions="+strconv.FormatUint(uint64(allocation.EncoderSessions), 10),
		), nil
	case runtimeStageLauncherProcessTree:
		return append(arguments,
			"--runtime-profile="+profile,
			"--workload-key="+allocation.WorkloadKey,
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
		Identity:         allocation.Identity,
		RuntimeNamespace: allocation.RuntimeNamespace,
		WaylandSocket:    allocation.WaylandSocket,
		AudioSink:        allocation.AudioSink,
		InputSeat:        allocation.InputSeat,
		RenderNode:       allocation.RenderNode,
		Compositor:       allocation.Compositor,
		EncoderSessions:  allocation.EncoderSessions,
		WorkloadKey:      allocation.WorkloadKey,
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
		adapter.options.RuntimeProfile,
	)
	if err != nil {
		return nil, err
	}
	environment, err := runtimeProcessEnvironment(
		adapter.stage,
		allocation,
		adapter.options.RuntimeProfile,
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
