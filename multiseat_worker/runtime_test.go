package main

import (
	"context"
	"errors"
	"reflect"
	"strings"
	"sync"
	"testing"
	"time"
)

type fakeRuntimeRecorder struct {
	mutex       sync.Mutex
	events      []string
	allocations []runtimeAllocation
}

func (recorder *fakeRuntimeRecorder) record(
	event string,
	allocation *runtimeAllocation,
) {
	recorder.mutex.Lock()
	defer recorder.mutex.Unlock()
	recorder.events = append(recorder.events, event)
	if allocation != nil {
		recorder.allocations = append(recorder.allocations, *allocation)
	}
}

func (recorder *fakeRuntimeRecorder) snapshot() ([]string, []runtimeAllocation) {
	recorder.mutex.Lock()
	defer recorder.mutex.Unlock()
	return append([]string(nil), recorder.events...),
		append([]runtimeAllocation(nil), recorder.allocations...)
}

type fakeRuntimeLease struct {
	stage      runtimeStage
	recorder   *fakeRuntimeRecorder
	done       chan error
	completion func() <-chan error
	exitOnce   sync.Once
	stop       func(context.Context, *fakeRuntimeLease) error
}

func newFakeRuntimeLease(
	stage runtimeStage,
	recorder *fakeRuntimeRecorder,
) *fakeRuntimeLease {
	return &fakeRuntimeLease{
		stage:    stage,
		recorder: recorder,
		done:     make(chan error, 1),
	}
}

func (lease *fakeRuntimeLease) Done() <-chan error {
	if lease.completion != nil {
		return lease.completion()
	}
	return lease.done
}

func (lease *fakeRuntimeLease) finish(err error) {
	lease.exitOnce.Do(func() {
		if err != nil {
			lease.done <- err
		}
		close(lease.done)
	})
}

func (lease *fakeRuntimeLease) Stop(context context.Context) error {
	lease.recorder.record("stop:"+lease.stage.String(), nil)
	if lease.stop != nil {
		return lease.stop(context, lease)
	}
	lease.finish(nil)
	return nil
}

type fakeRuntimeAdapter struct {
	stage    runtimeStage
	recorder *fakeRuntimeRecorder
	lease    *fakeRuntimeLease
	start    func(context.Context, runtimeAllocation) (runtimeLease, error)
}

func (adapter *fakeRuntimeAdapter) Start(
	context context.Context,
	allocation runtimeAllocation,
) (runtimeLease, error) {
	adapter.recorder.record("start:"+adapter.stage.String(), &allocation)
	if adapter.start != nil {
		return adapter.start(context, allocation)
	}
	return adapter.lease, nil
}

type fakeRuntimeSet struct {
	adapters runtimeAdapters
	recorder *fakeRuntimeRecorder
	byStage  map[runtimeStage]*fakeRuntimeAdapter
	leases   map[runtimeStage]*fakeRuntimeLease
}

func newFakeRuntimeSet() *fakeRuntimeSet {
	recorder := &fakeRuntimeRecorder{}
	set := &fakeRuntimeSet{
		recorder: recorder,
		byStage:  make(map[runtimeStage]*fakeRuntimeAdapter),
		leases:   make(map[runtimeStage]*fakeRuntimeLease),
	}
	stages := []runtimeStage{
		runtimeStageSessionBus,
		runtimeStageAudio,
		runtimeStageCompositor,
		runtimeStageVirtualInput,
		runtimeStageCapture,
		runtimeStageEncoderLease,
		runtimeStageLauncherProcessTree,
	}
	for _, stage := range stages {
		lease := newFakeRuntimeLease(stage, recorder)
		adapter := &fakeRuntimeAdapter{stage: stage, recorder: recorder, lease: lease}
		set.leases[stage] = lease
		set.byStage[stage] = adapter
	}
	set.adapters = runtimeAdapters{
		SessionBus:          set.byStage[runtimeStageSessionBus],
		Audio:               set.byStage[runtimeStageAudio],
		Compositor:          set.byStage[runtimeStageCompositor],
		VirtualInput:        set.byStage[runtimeStageVirtualInput],
		Capture:             set.byStage[runtimeStageCapture],
		EncoderLease:        set.byStage[runtimeStageEncoderLease],
		LauncherProcessTree: set.byStage[runtimeStageLauncherProcessTree],
	}
	return set
}

func runtimeTestConfig(name string, generation uint64, slot uint32) workerConfig {
	return workerConfig{
		Identity: endpointIdentity{
			ControllerEpoch: "controller-runtime",
			LogicalGPU:      "gpu-primary",
			Slot:            slot,
			Generation:      generation,
			WorkerName:      name,
		},
		RuntimeNamespace: "runtime-" + name,
		WaylandSocket:    "wayland-" + name,
		AudioSink:        "audio-" + name,
		InputSeat:        "input-" + name,
		RenderNode:       "/dev/dri/renderD128",
		Compositor:       "gamescope",
		RuntimeProfile:   "steam",
		DisplayWidth:     1920,
		DisplayHeight:    1080,
		RefreshMillihz:   60000,
		DisplayHDR:       false,
		EncoderSessions:  1,
		WorkloadKey:      "synthetic",
	}
}

func runtimeTestOptions() runtimeOptions {
	return runtimeOptions{
		StartupTimeout:       2 * time.Second,
		ComponentStopTimeout: 250 * time.Millisecond,
	}
}

func completeRuntimeEvents() []string {
	return []string{
		"start:session-bus",
		"start:audio",
		"start:compositor",
		"start:virtual-input",
		"start:capture",
		"start:encoder-lease",
		"start:launcher-process-tree",
		"stop:launcher-process-tree",
		"stop:encoder-lease",
		"stop:capture",
		"stop:virtual-input",
		"stop:compositor",
		"stop:audio",
		"stop:session-bus",
	}
}

func waitForRuntimeEvent(
	t *testing.T,
	recorder *fakeRuntimeRecorder,
	event string,
) {
	t.Helper()
	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		events, _ := recorder.snapshot()
		for _, candidate := range events {
			if candidate == event {
				return
			}
		}
		time.Sleep(time.Millisecond)
	}
	t.Fatalf("runtime event %q was not recorded", event)
}

func TestRuntimeStartsInDependencyOrderAndStopsExactlyOnceInReverse(t *testing.T) {
	set := newFakeRuntimeSet()
	config := runtimeTestConfig("worker-order", 51, 0)
	runtime, err := startWorkerRuntime(
		context.Background(),
		config,
		set.adapters,
		runtimeTestOptions(),
	)
	if err != nil {
		t.Fatal(err)
	}
	stopErrors := make(chan error, 8)
	var callers sync.WaitGroup
	for index := 0; index < cap(stopErrors); index++ {
		callers.Add(1)
		go func() {
			defer callers.Done()
			stopErrors <- runtime.Stop()
		}()
	}
	callers.Wait()
	close(stopErrors)
	for stopError := range stopErrors {
		if stopError != nil {
			t.Fatal(stopError)
		}
	}
	if err := runtime.Stop(); err != nil {
		t.Fatal(err)
	}
	events, allocations := set.recorder.snapshot()
	if !reflect.DeepEqual(events, completeRuntimeEvents()) {
		t.Fatalf("runtime lifecycle order mismatch: %#v", events)
	}
	want, err := runtimeAllocationFromConfig(config)
	if err != nil {
		t.Fatal(err)
	}
	if len(allocations) != runtimeComponentCount {
		t.Fatalf("got %d adapter allocations, want %d", len(allocations), runtimeComponentCount)
	}
	for _, allocation := range allocations {
		if allocation != want {
			t.Fatalf("adapter received mutated allocation: %#v", allocation)
		}
	}
	select {
	case failure := <-runtime.Failures():
		t.Fatalf("normal teardown reported a runtime failure: %v", failure)
	default:
	}
}

func TestRuntimeRejectsInvalidInputsBeforeStartingResources(t *testing.T) {
	tests := []struct {
		name     string
		mutate   func(*workerConfig, *runtimeAdapters, *runtimeOptions)
		contains string
	}{
		{
			name: "missing adapter",
			mutate: func(_ *workerConfig, adapters *runtimeAdapters, _ *runtimeOptions) {
				adapters.Capture = nil
			},
			contains: "capture adapter is missing",
		},
		{
			name: "typed nil adapter",
			mutate: func(_ *workerConfig, adapters *runtimeAdapters, _ *runtimeOptions) {
				var missing *fakeRuntimeAdapter
				adapters.Capture = missing
			},
			contains: "capture adapter is missing",
		},
		{
			name: "missing workload",
			mutate: func(config *workerConfig, _ *runtimeAdapters, _ *runtimeOptions) {
				config.WorkloadKey = ""
			},
			contains: "workload key is invalid",
		},
		{
			name: "relative render node",
			mutate: func(config *workerConfig, _ *runtimeAdapters, _ *runtimeOptions) {
				config.RenderNode = "renderD128"
			},
			contains: "render node is invalid",
		},
		{
			name: "partial zero options",
			mutate: func(_ *workerConfig, _ *runtimeAdapters, options *runtimeOptions) {
				options.StartupTimeout = 0
			},
			contains: "startup timeout is invalid",
		},
		{
			name: "unbounded stop options",
			mutate: func(_ *workerConfig, _ *runtimeAdapters, options *runtimeOptions) {
				options.ComponentStopTimeout = maximumRuntimeComponentStopTimeout + time.Second
			},
			contains: "stop timeout is invalid",
		},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			set := newFakeRuntimeSet()
			config := runtimeTestConfig("worker-invalid", 52, 0)
			options := runtimeTestOptions()
			test.mutate(&config, &set.adapters, &options)
			_, err := startWorkerRuntime(
				context.Background(),
				config,
				set.adapters,
				options,
			)
			if err == nil || !strings.Contains(err.Error(), test.contains) {
				t.Fatalf("unexpected validation result: %v", err)
			}
			events, _ := set.recorder.snapshot()
			if len(events) != 0 {
				t.Fatalf("validation touched runtime resources: %#v", events)
			}
		})
	}
}

func TestRuntimeStartFailureCleansPartialLeaseThenRollsBack(t *testing.T) {
	set := newFakeRuntimeSet()
	set.byStage[runtimeStageCapture].start = func(
		context.Context,
		runtimeAllocation,
	) (runtimeLease, error) {
		return set.leases[runtimeStageCapture], errors.New("sensitive adapter detail")
	}
	_, err := startWorkerRuntime(
		context.Background(),
		runtimeTestConfig("worker-start-failure", 53, 0),
		set.adapters,
		runtimeTestOptions(),
	)
	if err == nil || !strings.Contains(err.Error(), "capture start failed") {
		t.Fatalf("unexpected start failure: %v", err)
	}
	if strings.Contains(err.Error(), "sensitive adapter") {
		t.Fatalf("adapter detail escaped the runtime boundary: %v", err)
	}
	events, _ := set.recorder.snapshot()
	want := []string{
		"start:session-bus",
		"start:audio",
		"start:compositor",
		"start:virtual-input",
		"start:capture",
		"stop:capture",
		"stop:virtual-input",
		"stop:compositor",
		"stop:audio",
		"stop:session-bus",
	}
	if !reflect.DeepEqual(events, want) {
		t.Fatalf("partial rollback order mismatch: %#v", events)
	}
}

func TestRuntimeRejectsMalformedAndEarlyExitedLeases(t *testing.T) {
	t.Run("nil lease", func(t *testing.T) {
		set := newFakeRuntimeSet()
		set.byStage[runtimeStageSessionBus].start = func(
			context.Context,
			runtimeAllocation,
		) (runtimeLease, error) {
			return nil, nil
		}
		_, err := startWorkerRuntime(
			context.Background(),
			runtimeTestConfig("worker-nil-lease", 54, 0),
			set.adapters,
			runtimeTestOptions(),
		)
		if err == nil || !strings.Contains(err.Error(), "returned no lease") {
			t.Fatalf("unexpected nil lease result: %v", err)
		}
		events, _ := set.recorder.snapshot()
		if !reflect.DeepEqual(events, []string{"start:session-bus"}) {
			t.Fatalf("nil lease touched unexpected resources: %#v", events)
		}
	})

	t.Run("nil completion signal", func(t *testing.T) {
		set := newFakeRuntimeSet()
		set.leases[runtimeStageSessionBus].done = nil
		set.leases[runtimeStageSessionBus].stop = func(
			context.Context,
			*fakeRuntimeLease,
		) error {
			return nil
		}
		_, err := startWorkerRuntime(
			context.Background(),
			runtimeTestConfig("worker-nil-done", 54, 0),
			set.adapters,
			runtimeTestOptions(),
		)
		if err == nil || !strings.Contains(err.Error(), "returned no completion signal") {
			t.Fatalf("unexpected nil completion result: %v", err)
		}
		events, _ := set.recorder.snapshot()
		if !reflect.DeepEqual(events, []string{"start:session-bus", "stop:session-bus"}) {
			t.Fatalf("malformed lease was not cleaned: %#v", events)
		}
	})

	t.Run("blocked completion signal", func(t *testing.T) {
		set := newFakeRuntimeSet()
		release := make(chan struct{})
		set.leases[runtimeStageSessionBus].completion = func() <-chan error {
			<-release
			return set.leases[runtimeStageSessionBus].done
		}
		options := runtimeTestOptions()
		options.StartupTimeout = 50 * time.Millisecond
		options.ComponentStopTimeout = 50 * time.Millisecond
		started := time.Now()
		_, err := startWorkerRuntime(
			context.Background(),
			runtimeTestConfig("worker-blocked-done", 55, 0),
			set.adapters,
			options,
		)
		if err == nil || !strings.Contains(err.Error(), "startup did not complete") {
			t.Fatalf("unexpected blocked completion result: %v", err)
		}
		if elapsed := time.Since(started); elapsed > time.Second {
			t.Fatalf("blocked completion was not bounded: %v", elapsed)
		}
		close(release)
		events, _ := set.recorder.snapshot()
		if !reflect.DeepEqual(events, []string{"start:session-bus", "stop:session-bus"}) {
			t.Fatalf("blocked completion cleanup mismatch: %#v", events)
		}
	})

	t.Run("early exit", func(t *testing.T) {
		set := newFakeRuntimeSet()
		set.leases[runtimeStageSessionBus].finish(errors.New("private adapter error"))
		_, err := startWorkerRuntime(
			context.Background(),
			runtimeTestConfig("worker-early-exit", 55, 0),
			set.adapters,
			runtimeTestOptions(),
		)
		if err == nil || !strings.Contains(err.Error(), "exited during startup") {
			t.Fatalf("unexpected early-exit result: %v", err)
		}
		if strings.Contains(err.Error(), "private adapter") {
			t.Fatalf("adapter detail escaped the runtime boundary: %v", err)
		}
		events, _ := set.recorder.snapshot()
		if !reflect.DeepEqual(events, []string{"start:session-bus", "stop:session-bus"}) {
			t.Fatalf("early lease was not cleaned: %#v", events)
		}
	})

	t.Run("adapter panic", func(t *testing.T) {
		set := newFakeRuntimeSet()
		set.byStage[runtimeStageSessionBus].start = func(
			context.Context,
			runtimeAllocation,
		) (runtimeLease, error) {
			panic("private panic detail")
		}
		_, err := startWorkerRuntime(
			context.Background(),
			runtimeTestConfig("worker-panic", 56, 0),
			set.adapters,
			runtimeTestOptions(),
		)
		if err == nil || !strings.Contains(err.Error(), "session-bus start failed") {
			t.Fatalf("unexpected panic result: %v", err)
		}
		if strings.Contains(err.Error(), "private panic") {
			t.Fatalf("panic detail escaped the runtime boundary: %v", err)
		}
	})
}

func TestRuntimeStartupTimeoutIsBoundedAndCleansLateLease(t *testing.T) {
	set := newFakeRuntimeSet()
	release := make(chan struct{})
	set.byStage[runtimeStageAudio].start = func(
		context.Context,
		runtimeAllocation,
	) (runtimeLease, error) {
		<-release
		return set.leases[runtimeStageAudio], nil
	}
	options := runtimeTestOptions()
	options.StartupTimeout = 50 * time.Millisecond
	started := time.Now()
	_, err := startWorkerRuntime(
		context.Background(),
		runtimeTestConfig("worker-timeout", 57, 0),
		set.adapters,
		options,
	)
	if err == nil || !strings.Contains(err.Error(), "startup did not complete") {
		t.Fatalf("unexpected timeout result: %v", err)
	}
	if elapsed := time.Since(started); elapsed > time.Second {
		t.Fatalf("startup timeout was not bounded: %v", elapsed)
	}
	close(release)
	waitForRuntimeEvent(t, set.recorder, "stop:audio")
	events, _ := set.recorder.snapshot()
	want := []string{
		"start:session-bus",
		"start:audio",
		"stop:session-bus",
		"stop:audio",
	}
	if !reflect.DeepEqual(events, want) {
		t.Fatalf("late-start cleanup mismatch: %#v", events)
	}
}

func TestRuntimeBrokenStopsCannotStarveReverseCleanup(t *testing.T) {
	set := newFakeRuntimeSet()
	set.leases[runtimeStageLauncherProcessTree].stop = func(
		context.Context,
		*fakeRuntimeLease,
	) error {
		panic("private launcher panic")
	}
	set.leases[runtimeStageEncoderLease].stop = func(
		context context.Context,
		_ *fakeRuntimeLease,
	) error {
		<-context.Done()
		return context.Err()
	}
	set.leases[runtimeStageCapture].stop = func(
		_ context.Context,
		lease *fakeRuntimeLease,
	) error {
		lease.finish(nil)
		return errors.New("private capture stop detail")
	}
	options := runtimeTestOptions()
	options.ComponentStopTimeout = 50 * time.Millisecond
	runtime, err := startWorkerRuntime(
		context.Background(),
		runtimeTestConfig("worker-broken-stop", 58, 0),
		set.adapters,
		options,
	)
	if err != nil {
		t.Fatal(err)
	}
	stopError := runtime.Stop()
	set.leases[runtimeStageLauncherProcessTree].finish(nil)
	set.leases[runtimeStageEncoderLease].finish(nil)
	if stopError == nil {
		t.Fatal("broken teardown returned success")
	}
	if strings.Contains(stopError.Error(), "private") {
		t.Fatalf("adapter detail escaped the runtime boundary: %v", stopError)
	}
	events, _ := set.recorder.snapshot()
	if !reflect.DeepEqual(events, completeRuntimeEvents()) {
		t.Fatalf("broken adapter starved reverse cleanup: %#v", events)
	}
	if repeated := runtime.Stop(); repeated == nil || repeated.Error() != stopError.Error() {
		t.Fatalf("repeated stop did not preserve the terminal result: %v", repeated)
	}
	repeatedEvents, _ := set.recorder.snapshot()
	if !reflect.DeepEqual(repeatedEvents, events) {
		t.Fatalf("repeated stop touched resources: %#v", repeatedEvents)
	}
}

func TestRuntimeReportsUnexpectedExitAndTwoInstancesRemainIndependent(t *testing.T) {
	firstSet := newFakeRuntimeSet()
	secondSet := newFakeRuntimeSet()
	first, err := startWorkerRuntime(
		context.Background(),
		runtimeTestConfig("worker-first", 59, 0),
		firstSet.adapters,
		runtimeTestOptions(),
	)
	if err != nil {
		t.Fatal(err)
	}
	second, err := startWorkerRuntime(
		context.Background(),
		runtimeTestConfig("worker-second", 60, 1),
		secondSet.adapters,
		runtimeTestOptions(),
	)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		_ = first.Stop()
		_ = second.Stop()
	})

	firstSet.leases[runtimeStageCapture].finish(errors.New("private device path"))
	select {
	case failure := <-first.Failures():
		if failure == nil || !strings.Contains(failure.Error(), "capture exited unexpectedly") {
			t.Fatalf("unexpected first runtime failure: %v", failure)
		}
		if strings.Contains(failure.Error(), "private device") {
			t.Fatalf("adapter detail escaped the runtime boundary: %v", failure)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("first runtime did not report component exit")
	}
	select {
	case failure := <-second.Failures():
		t.Fatalf("first runtime failure crossed seat boundary: %v", failure)
	default:
	}
	if err := first.Stop(); err != nil {
		t.Fatal(err)
	}
	secondEvents, _ := secondSet.recorder.snapshot()
	for _, event := range secondEvents {
		if strings.HasPrefix(event, "stop:") {
			t.Fatalf("stopping first runtime touched second: %#v", secondEvents)
		}
	}
	if err := second.Stop(); err != nil {
		t.Fatal(err)
	}
}
