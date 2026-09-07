package main

import (
	"context"
	"errors"
	"fmt"
	"path/filepath"
	"reflect"
	"strings"
	"sync"
	"time"
)

const (
	defaultRuntimeStartupTimeout       = 120 * time.Second
	defaultRuntimeComponentStopTimeout = 5 * time.Second
	maximumRuntimeStartupTimeout       = 5 * time.Minute
	maximumRuntimeComponentStopTimeout = 30 * time.Second
	runtimeComponentCount              = 7
)

type runtimeStage uint8

const (
	runtimeStageSessionBus runtimeStage = iota + 1
	runtimeStageAudio
	runtimeStageDisplayCapture
	runtimeStageNestedCompositor
	runtimeStageVirtualInput
	runtimeStageEncoder
	runtimeStageLauncherProcessTree
)

func (stage runtimeStage) String() string {
	switch stage {
	case runtimeStageSessionBus:
		return "session-bus"
	case runtimeStageAudio:
		return "audio"
	case runtimeStageDisplayCapture:
		return "display-capture"
	case runtimeStageNestedCompositor:
		return "nested-compositor"
	case runtimeStageVirtualInput:
		return "virtual-input"
	case runtimeStageEncoder:
		return "encoder"
	case runtimeStageLauncherProcessTree:
		return "launcher-process-tree"
	default:
		return "unknown"
	}
}

// runtimeAllocation is a value-only copy of controller-owned seat authority.
// Adapters cannot mutate workerConfig or obtain client identity/profile data.
type runtimeAllocation struct {
	Identity             endpointIdentity
	RuntimeNamespace     string
	CaptureWaylandSocket string
	WaylandSocket        string
	AudioSink            string
	InputSeat            string
	RenderNode           string
	Compositor           string
	RuntimeProfile       string
	DisplayTopology      displayTopology
	MediaPipeline        mediaPipeline
	DisplayWidth         uint32
	DisplayHeight        uint32
	RefreshMillihz       uint32
	DisplayHDR           bool
	EncoderSessions      uint32
	Workload             workloadPlan
}

// runtimeLease represents one ready resource. Done must signal its terminal
// state exactly once. Stop must honor its context and remain idempotent.
type runtimeLease interface {
	Done() <-chan error
	Stop(context.Context) error
}

// runtimeAdapter must return only after its resource is ready. Start must honor
// the shared startup context; it may return a lease with an error when partial
// allocation needs cleanup.
type runtimeAdapter interface {
	Start(context.Context, runtimeAllocation) (runtimeLease, error)
}

type runtimeAdapters struct {
	SessionBus          runtimeAdapter
	Audio               runtimeAdapter
	DisplayCapture      runtimeAdapter
	NestedCompositor    runtimeAdapter
	VirtualInput        runtimeAdapter
	Encoder             runtimeAdapter
	LauncherProcessTree runtimeAdapter
}

type runtimeOptions struct {
	StartupTimeout       time.Duration
	ComponentStopTimeout time.Duration
}

type runtimeStageAdapter struct {
	stage   runtimeStage
	adapter runtimeAdapter
}

type runtimeComponent struct {
	stage  runtimeStage
	lease  runtimeLease
	exited chan struct{}
}

type workerRuntime struct {
	allocation runtimeAllocation
	options    runtimeOptions
	components []*runtimeComponent
	failures   chan error

	stateMutex sync.Mutex
	stopping   bool
	stopOnce   sync.Once
	stopErr    error
}

type runtimeStartResult struct {
	lease  runtimeLease
	failed bool
}

type runtimeDoneResult struct {
	done  <-chan error
	valid bool
}

func defaultRuntimeOptions() runtimeOptions {
	return runtimeOptions{
		StartupTimeout:       defaultRuntimeStartupTimeout,
		ComponentStopTimeout: defaultRuntimeComponentStopTimeout,
	}
}

func normalizeRuntimeOptions(options runtimeOptions) (runtimeOptions, error) {
	if options == (runtimeOptions{}) {
		options = defaultRuntimeOptions()
	}
	if options.StartupTimeout <= 0 ||
		options.StartupTimeout > maximumRuntimeStartupTimeout {
		return runtimeOptions{}, errors.New("worker runtime startup timeout is invalid")
	}
	if options.ComponentStopTimeout <= 0 ||
		options.ComponentStopTimeout > maximumRuntimeComponentStopTimeout {
		return runtimeOptions{}, errors.New("worker runtime stop timeout is invalid")
	}
	return options, nil
}

func runtimeAllocationFromConfig(config workerConfig) (runtimeAllocation, error) {
	if !validIdentity(config.Identity) {
		return runtimeAllocation{}, errors.New("worker runtime identity is invalid")
	}
	if !validNameToken(config.RuntimeNamespace, 128) ||
		!validNameToken(config.CaptureWaylandSocket, 128) ||
		!validNameToken(config.WaylandSocket, 128) ||
		!validNameToken(config.AudioSink, 128) ||
		!validNameToken(config.InputSeat, 128) {
		return runtimeAllocation{}, errors.New("worker runtime resource name is invalid")
	}
	if !filepath.IsAbs(config.RenderNode) ||
		filepath.Clean(config.RenderNode) != config.RenderNode ||
		!strings.HasPrefix(config.RenderNode, "/dev/") {
		return runtimeAllocation{}, errors.New("worker runtime render node is invalid")
	}
	if config.Compositor != "gamescope" &&
		config.Compositor != "sway" &&
		config.Compositor != "labwc" {
		return runtimeAllocation{}, errors.New("worker runtime compositor is invalid")
	}
	if !validRuntimeProfile(config.RuntimeProfile) {
		return runtimeAllocation{}, errors.New("worker runtime profile allocation is invalid")
	}
	if !validDataPlane(config.DisplayTopology, config.MediaPipeline) {
		return runtimeAllocation{}, errors.New("worker runtime data plane allocation is invalid")
	}
	if config.DisplayWidth == 0 || config.DisplayWidth > 16384 ||
		config.DisplayHeight == 0 || config.DisplayHeight > 16384 ||
		config.RefreshMillihz < 1000 || config.RefreshMillihz > 1000000 {
		return runtimeAllocation{}, errors.New("worker runtime display allocation is invalid")
	}
	if config.EncoderSessions == 0 || config.EncoderSessions > 64 {
		return runtimeAllocation{}, errors.New("worker runtime encoder allocation is invalid")
	}
	if !validWorkloadPlan(config.Workload) ||
		!workloadMatchesRuntimeProfile(config.Workload, config.RuntimeProfile) {
		return runtimeAllocation{}, errors.New("worker runtime workload plan is invalid")
	}
	return runtimeAllocation{
		Identity:             config.Identity,
		RuntimeNamespace:     config.RuntimeNamespace,
		CaptureWaylandSocket: config.CaptureWaylandSocket,
		WaylandSocket:        config.WaylandSocket,
		AudioSink:            config.AudioSink,
		InputSeat:            config.InputSeat,
		RenderNode:           config.RenderNode,
		Compositor:           config.Compositor,
		RuntimeProfile:       config.RuntimeProfile,
		DisplayTopology:      config.DisplayTopology,
		MediaPipeline:        config.MediaPipeline,
		DisplayWidth:         config.DisplayWidth,
		DisplayHeight:        config.DisplayHeight,
		RefreshMillihz:       config.RefreshMillihz,
		DisplayHDR:           config.DisplayHDR,
		EncoderSessions:      config.EncoderSessions,
		Workload:             config.Workload,
	}, nil
}

func (adapters runtimeAdapters) ordered() []runtimeStageAdapter {
	return []runtimeStageAdapter{
		{runtimeStageSessionBus, adapters.SessionBus},
		{runtimeStageAudio, adapters.Audio},
		{runtimeStageDisplayCapture, adapters.DisplayCapture},
		{runtimeStageNestedCompositor, adapters.NestedCompositor},
		{runtimeStageVirtualInput, adapters.VirtualInput},
		{runtimeStageEncoder, adapters.Encoder},
		{runtimeStageLauncherProcessTree, adapters.LauncherProcessTree},
	}
}

func nilRuntimeInterface(value any) bool {
	if value == nil {
		return true
	}
	reflected := reflect.ValueOf(value)
	switch reflected.Kind() {
	case reflect.Chan, reflect.Func, reflect.Interface, reflect.Map,
		reflect.Pointer, reflect.Slice:
		return reflected.IsNil()
	default:
		return false
	}
}

func validateRuntimeAdapters(adapters runtimeAdapters) ([]runtimeStageAdapter, error) {
	ordered := adapters.ordered()
	if len(ordered) != runtimeComponentCount {
		return nil, errors.New("worker runtime adapter set is incomplete")
	}
	for _, selected := range ordered {
		if nilRuntimeInterface(selected.adapter) {
			return nil, fmt.Errorf("worker runtime %s adapter is missing", selected.stage)
		}
	}
	return ordered, nil
}

func callRuntimeStart(
	context context.Context,
	adapter runtimeAdapter,
	allocation runtimeAllocation,
) (result runtimeStartResult) {
	defer func() {
		if recover() != nil {
			result = runtimeStartResult{failed: true}
		}
	}()
	var err error
	result.lease, err = adapter.Start(context, allocation)
	result.failed = err != nil
	return result
}

func callRuntimeStop(context context.Context, lease runtimeLease) (failed bool) {
	defer func() {
		if recover() != nil {
			failed = true
		}
	}()
	return lease.Stop(context) != nil
}

func runtimeDone(lease runtimeLease) (done <-chan error, valid bool) {
	defer func() {
		if recover() != nil {
			done = nil
			valid = false
		}
	}()
	done = lease.Done()
	return done, done != nil
}

func beginRuntimeDone(lease runtimeLease) <-chan runtimeDoneResult {
	result := make(chan runtimeDoneResult, 1)
	go func() {
		done, valid := runtimeDone(lease)
		result <- runtimeDoneResult{done: done, valid: valid}
	}()
	return result
}

func beginRuntimeStart(
	context context.Context,
	adapter runtimeAdapter,
	allocation runtimeAllocation,
) <-chan runtimeStartResult {
	result := make(chan runtimeStartResult, 1)
	go func() {
		result <- callRuntimeStart(context, adapter, allocation)
	}()
	return result
}

func stopDetachedRuntimeLease(
	stage runtimeStage,
	lease runtimeLease,
	timeout time.Duration,
) error {
	if nilRuntimeInterface(lease) {
		return nil
	}
	context, cancel := context.WithTimeout(context.Background(), timeout)
	defer cancel()
	result := make(chan bool, 1)
	go func() {
		result <- callRuntimeStop(context, lease)
	}()
	select {
	case failed := <-result:
		if failed {
			return fmt.Errorf("worker runtime %s partial cleanup failed", stage)
		}
	case <-context.Done():
		return fmt.Errorf("worker runtime %s partial cleanup timed out", stage)
	}
	var completion runtimeDoneResult
	select {
	case completion = <-beginRuntimeDone(lease):
	case <-context.Done():
		return fmt.Errorf("worker runtime %s partial cleanup completion timed out", stage)
	}
	if !completion.valid {
		return nil
	}
	select {
	case <-completion.done:
		return nil
	case <-context.Done():
		return fmt.Errorf("worker runtime %s partial cleanup did not exit", stage)
	}
}

func cleanupLateRuntimeStart(
	stage runtimeStage,
	result <-chan runtimeStartResult,
	timeout time.Duration,
) {
	go func() {
		completed := <-result
		_ = stopDetachedRuntimeLease(stage, completed.lease, timeout)
	}()
}

func (runtime *workerRuntime) reportFailure(stage runtimeStage) {
	runtime.stateMutex.Lock()
	defer runtime.stateMutex.Unlock()
	if runtime.stopping {
		return
	}
	select {
	case runtime.failures <- fmt.Errorf("worker runtime %s exited unexpectedly", stage):
	default:
	}
}

func (runtime *workerRuntime) watch(
	component *runtimeComponent,
	done <-chan error,
) {
	<-done
	close(component.exited)
	runtime.reportFailure(component.stage)
}

func (runtime *workerRuntime) beginStopping() {
	runtime.stateMutex.Lock()
	runtime.stopping = true
	runtime.stateMutex.Unlock()
}

func stopRuntimeComponent(
	component *runtimeComponent,
	timeout time.Duration,
) error {
	context, cancel := context.WithTimeout(context.Background(), timeout)
	defer cancel()
	result := make(chan bool, 1)
	go func() {
		result <- callRuntimeStop(context, component.lease)
	}()
	var stopError error
	select {
	case failed := <-result:
		if failed {
			stopError = fmt.Errorf("worker runtime %s stop failed", component.stage)
		}
	case <-context.Done():
		return fmt.Errorf("worker runtime %s stop timed out", component.stage)
	}
	select {
	case <-component.exited:
		return stopError
	case <-context.Done():
		return errors.Join(
			stopError,
			fmt.Errorf("worker runtime %s did not exit", component.stage),
		)
	}
}

func stopRuntimeComponents(
	components []*runtimeComponent,
	timeout time.Duration,
) error {
	var joined error
	for index := len(components) - 1; index >= 0; index-- {
		joined = errors.Join(joined, stopRuntimeComponent(components[index], timeout))
	}
	return joined
}

func (runtime *workerRuntime) Stop() error {
	runtime.stopOnce.Do(func() {
		runtime.beginStopping()
		runtime.stopErr = stopRuntimeComponents(
			runtime.components,
			runtime.options.ComponentStopTimeout,
		)
	})
	return runtime.stopErr
}

func (runtime *workerRuntime) Failures() <-chan error {
	return runtime.failures
}

func startWorkerRuntime(
	parent context.Context,
	config workerConfig,
	adapters runtimeAdapters,
	options runtimeOptions,
) (*workerRuntime, error) {
	if parent == nil {
		return nil, errors.New("worker runtime context is missing")
	}
	allocation, err := runtimeAllocationFromConfig(config)
	if err != nil {
		return nil, err
	}
	options, err = normalizeRuntimeOptions(options)
	if err != nil {
		return nil, err
	}
	ordered, err := validateRuntimeAdapters(adapters)
	if err != nil {
		return nil, err
	}

	runtime := &workerRuntime{
		allocation: allocation,
		options:    options,
		components: make([]*runtimeComponent, 0, runtimeComponentCount),
		failures:   make(chan error, 1),
	}
	startupContext, cancel := context.WithTimeout(parent, options.StartupTimeout)
	defer cancel()

	failStart := func(startError error) (*workerRuntime, error) {
		runtime.beginStopping()
		return nil, errors.Join(
			startError,
			stopRuntimeComponents(runtime.components, options.ComponentStopTimeout),
		)
	}

	for _, selected := range ordered {
		resultChannel := beginRuntimeStart(startupContext, selected.adapter, allocation)
		var result runtimeStartResult
		select {
		case result = <-resultChannel:
		case failure := <-runtime.failures:
			cancel()
			cleanupLateRuntimeStart(selected.stage, resultChannel, options.ComponentStopTimeout)
			return failStart(failure)
		case <-startupContext.Done():
			cleanupLateRuntimeStart(selected.stage, resultChannel, options.ComponentStopTimeout)
			return failStart(errors.New("worker runtime startup did not complete"))
		}

		if result.failed {
			cleanupError := stopDetachedRuntimeLease(
				selected.stage,
				result.lease,
				options.ComponentStopTimeout,
			)
			_, startError := failStart(
				fmt.Errorf("worker runtime %s start failed", selected.stage),
			)
			return nil, errors.Join(startError, cleanupError)
		}
		if nilRuntimeInterface(result.lease) {
			return failStart(fmt.Errorf("worker runtime %s returned no lease", selected.stage))
		}
		var completion runtimeDoneResult
		select {
		case completion = <-beginRuntimeDone(result.lease):
		case failure := <-runtime.failures:
			cancel()
			cleanupError := stopDetachedRuntimeLease(
				selected.stage,
				result.lease,
				options.ComponentStopTimeout,
			)
			_, startError := failStart(failure)
			return nil, errors.Join(startError, cleanupError)
		case <-startupContext.Done():
			cleanupError := stopDetachedRuntimeLease(
				selected.stage,
				result.lease,
				options.ComponentStopTimeout,
			)
			_, startError := failStart(
				errors.New("worker runtime startup did not complete"),
			)
			return nil, errors.Join(startError, cleanupError)
		}
		if !completion.valid {
			cleanupError := stopDetachedRuntimeLease(
				selected.stage,
				result.lease,
				options.ComponentStopTimeout,
			)
			_, startError := failStart(
				fmt.Errorf("worker runtime %s returned no completion signal", selected.stage),
			)
			return nil, errors.Join(startError, cleanupError)
		}
		component := &runtimeComponent{
			stage:  selected.stage,
			lease:  result.lease,
			exited: make(chan struct{}),
		}
		runtime.components = append(runtime.components, component)
		select {
		case <-completion.done:
			close(component.exited)
			return failStart(
				fmt.Errorf("worker runtime %s exited during startup", selected.stage),
			)
		default:
			go runtime.watch(component, completion.done)
		}
	}

	select {
	case failure := <-runtime.failures:
		return failStart(failure)
	case <-startupContext.Done():
		return failStart(errors.New("worker runtime startup did not complete"))
	default:
		return runtime, nil
	}
}
