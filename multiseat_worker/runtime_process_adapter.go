package main

import (
	"context"
	"errors"
	"path/filepath"
	"strings"

	"github.com/papi-ux/polaris/multiseat_worker/internal/seatruntime"
)

const (
	defaultRuntimeHelperPath = "/usr/bin/polaris-seat-runtime"
	runtimeReadyFDSetting    = seatruntime.ReadyFDSetting
	runtimeReadyRecord       = seatruntime.ReadyRecord
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
	CompositorInput  bool
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
	request, err := seatRuntimeRequest(stage, allocation)
	if err != nil {
		return nil, err
	}
	return seatruntime.Environment(request)
}

func runtimeProcessArguments(
	stage runtimeStage,
	allocation runtimeAllocation,
) ([]string, error) {
	request, err := seatRuntimeRequest(stage, allocation)
	if err != nil {
		return nil, err
	}
	return seatruntime.Arguments(request)
}

func seatRuntimeRequest(
	stage runtimeStage,
	allocation runtimeAllocation,
) (seatruntime.Request, error) {
	if !validRuntimeStage(stage) || !validRuntimeProfile(allocation.RuntimeProfile) {
		return seatruntime.Request{}, errors.New("worker runtime helper stage is invalid")
	}
	request := seatruntime.Request{
		Stage:            seatruntime.Stage(stage.String()),
		RuntimeNamespace: allocation.RuntimeNamespace,
	}
	switch stage {
	case runtimeStageSessionBus:
		return request, nil
	case runtimeStageAudio:
		request.AudioSink = allocation.AudioSink
	case runtimeStageDisplayCapture:
		request.CaptureWaylandSocket = allocation.CaptureWaylandSocket
		request.RenderNode = allocation.RenderNode
		request.DisplayTopology = string(allocation.DisplayTopology)
		request.MediaPipeline = string(allocation.MediaPipeline)
		request.DisplayWidth = allocation.DisplayWidth
		request.DisplayHeight = allocation.DisplayHeight
		request.DisplayRefreshMillihertz = allocation.RefreshMillihz
		request.DisplayHDR = allocation.DisplayHDR
	case runtimeStageNestedCompositor:
		request.ParentWaylandSocket = allocation.CaptureWaylandSocket
		request.WaylandSocket = allocation.WaylandSocket
		request.RenderNode = allocation.RenderNode
		request.DisplayWidth = allocation.DisplayWidth
		request.DisplayHeight = allocation.DisplayHeight
		request.DisplayRefreshMillihertz = allocation.RefreshMillihz
		request.DisplayHDR = allocation.DisplayHDR
		request.Compositor = allocation.Compositor
	case runtimeStageVirtualInput:
		request.InputSeat = allocation.InputSeat
	case runtimeStageEncoder:
		request.LogicalGPU = allocation.Identity.LogicalGPU
		request.RenderNode = allocation.RenderNode
		request.EncoderSessions = allocation.EncoderSessions
		request.MediaPipeline = string(allocation.MediaPipeline)
	case runtimeStageLauncherProcessTree:
		request.RuntimeProfile = allocation.RuntimeProfile
		request.WorkloadKind = seatruntime.WorkloadKind(allocation.Workload.Kind)
		request.WorkloadID = allocation.Workload.TargetID
		request.WaylandSocket = allocation.WaylandSocket
		request.AudioSink = allocation.AudioSink
		request.InputSeat = allocation.InputSeat
	default:
		return seatruntime.Request{}, errors.New("worker runtime helper stage is invalid")
	}
	return request, nil
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
	request, err := seatRuntimeRequest(adapter.stage, allocation)
	if err != nil {
		return nil, err
	}
	if adapter.options.CompositorInput && adapter.stage == runtimeStageDisplayCapture {
		request.InputSeat = allocation.InputSeat
	}
	arguments, err := seatruntime.Arguments(request)
	if err != nil {
		return nil, err
	}
	environment, err := seatruntime.Environment(request)
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
