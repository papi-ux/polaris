package main

import (
	"context"
	"errors"
	"reflect"
	"strings"
	"sync"
	"testing"
)

type fakeRuntimeProcessHost struct {
	mutex      sync.Mutex
	recorder   *fakeRuntimeRecorder
	specs      []runtimeProcessSpec
	start      func(runtimeProcessSpec) (runtimeLease, error)
	startCalls int
}

func (host *fakeRuntimeProcessHost) Start(
	_ context.Context,
	spec runtimeProcessSpec,
) (runtimeLease, error) {
	host.mutex.Lock()
	host.startCalls++
	host.specs = append(host.specs, runtimeProcessSpec{
		Stage:       spec.Stage,
		Executable:  spec.Executable,
		Arguments:   append([]string(nil), spec.Arguments...),
		Environment: append([]string(nil), spec.Environment...),
	})
	host.mutex.Unlock()
	if host.start != nil {
		return host.start(spec)
	}
	lease := newFakeRuntimeLease(spec.Stage, host.recorder)
	return lease, nil
}

func (host *fakeRuntimeProcessHost) snapshot() ([]runtimeProcessSpec, int) {
	host.mutex.Lock()
	defer host.mutex.Unlock()
	return append([]runtimeProcessSpec(nil), host.specs...), host.startCalls
}

func TestProcessRuntimeAdaptersBuildLeastAuthorityLiteralCommands(t *testing.T) {
	recorder := &fakeRuntimeRecorder{}
	host := &fakeRuntimeProcessHost{recorder: recorder}
	adapters, err := newProcessRuntimeAdapters(host, processRuntimeAdapterOptions{
		HelperExecutable: "/opt/polaris/bin/polaris-seat-runtime",
		RuntimeProfile:   "heroic",
	})
	if err != nil {
		t.Fatal(err)
	}
	config := runtimeTestConfig("worker-process-adapters", 71, 3)
	config.WorkloadKey = "heroic-game;$(touch /tmp/not-executed)"
	runtime, err := startWorkerRuntime(
		context.Background(),
		config,
		adapters,
		runtimeTestOptions(),
	)
	if err != nil {
		t.Fatal(err)
	}
	if err := runtime.Stop(); err != nil {
		t.Fatal(err)
	}
	specs, calls := host.snapshot()
	if calls != runtimeComponentCount || len(specs) != runtimeComponentCount {
		t.Fatalf("got %d process starts and %d specs", calls, len(specs))
	}
	wantStages := []runtimeStage{
		runtimeStageSessionBus,
		runtimeStageAudio,
		runtimeStageCompositor,
		runtimeStageVirtualInput,
		runtimeStageCapture,
		runtimeStageEncoderLease,
		runtimeStageLauncherProcessTree,
	}
	for index, spec := range specs {
		if spec.Stage != wantStages[index] {
			t.Fatalf("process stage %d = %s", index, spec.Stage)
		}
		if spec.Executable != "/opt/polaris/bin/polaris-seat-runtime" {
			t.Fatalf("unexpected helper path: %q", spec.Executable)
		}
		if len(spec.Arguments) < 3 || spec.Arguments[0] != "serve" ||
			spec.Arguments[1] != "--stage="+spec.Stage.String() ||
			spec.Arguments[2] != "--runtime-namespace="+config.RuntimeNamespace {
			t.Fatalf("invalid helper prefix for %s: %#v", spec.Stage, spec.Arguments)
		}
		joined := strings.Join(append(spec.Arguments, spec.Environment...), "\n")
		for _, forbidden := range []string{
			"POLARIS_AUTH_TOKEN",
			"--worker-name=",
			"--controller-epoch=",
		} {
			if strings.Contains(joined, forbidden) {
				t.Fatalf("%s helper received forbidden authority %q", spec.Stage, forbidden)
			}
		}
	}
	if got, want := specs[0].Arguments, []string{
		"serve",
		"--stage=session-bus",
		"--runtime-namespace=" + config.RuntimeNamespace,
	}; !reflect.DeepEqual(got, want) {
		t.Fatalf("session bus argv mismatch: %#v", got)
	}
	if got, want := specs[1].Arguments, []string{
		"serve",
		"--stage=audio",
		"--runtime-namespace=" + config.RuntimeNamespace,
		"--audio-sink=" + config.AudioSink,
	}; !reflect.DeepEqual(got, want) {
		t.Fatalf("audio argv mismatch: %#v", got)
	}
	if got, want := specs[2].Arguments, []string{
		"serve",
		"--stage=compositor",
		"--runtime-namespace=" + config.RuntimeNamespace,
		"--wayland-socket=" + config.WaylandSocket,
		"--render-node=" + config.RenderNode,
		"--compositor=gamescope",
	}; !reflect.DeepEqual(got, want) {
		t.Fatalf("compositor argv mismatch: %#v", got)
	}
	if got, want := specs[3].Arguments, []string{
		"serve",
		"--stage=virtual-input",
		"--runtime-namespace=" + config.RuntimeNamespace,
		"--input-seat=" + config.InputSeat,
	}; !reflect.DeepEqual(got, want) {
		t.Fatalf("virtual input argv mismatch: %#v", got)
	}
	if got, want := specs[4].Arguments, []string{
		"serve",
		"--stage=capture",
		"--runtime-namespace=" + config.RuntimeNamespace,
		"--wayland-socket=" + config.WaylandSocket,
		"--render-node=" + config.RenderNode,
	}; !reflect.DeepEqual(got, want) {
		t.Fatalf("capture argv mismatch: %#v", got)
	}
	if got, want := specs[5].Arguments, []string{
		"serve",
		"--stage=encoder-lease",
		"--runtime-namespace=" + config.RuntimeNamespace,
		"--logical-gpu-id=" + config.Identity.LogicalGPU,
		"--render-node=" + config.RenderNode,
		"--sessions=1",
	}; !reflect.DeepEqual(got, want) {
		t.Fatalf("encoder argv mismatch: %#v", got)
	}
	launcher := specs[6]
	wantWorkload := "--workload-key=" + config.WorkloadKey
	if !reflect.DeepEqual(launcher.Arguments, []string{
		"serve",
		"--stage=launcher-process-tree",
		"--runtime-namespace=" + config.RuntimeNamespace,
		"--runtime-profile=heroic",
		wantWorkload,
		"--wayland-socket=" + config.WaylandSocket,
		"--audio-sink=" + config.AudioSink,
		"--input-seat=" + config.InputSeat,
	}) {
		t.Fatalf("launcher argv mismatch: %#v", launcher.Arguments)
	}
	if count := strings.Count(strings.Join(launcher.Arguments, "\n"), wantWorkload); count != 1 {
		t.Fatalf("literal workload was split or duplicated: %#v", launcher.Arguments)
	}
	wantEnvironment, err := runtimeProcessEnvironment(
		runtimeStageLauncherProcessTree,
		runtime.allocation,
		"heroic",
	)
	if err != nil {
		t.Fatal(err)
	}
	if !reflect.DeepEqual(launcher.Environment, wantEnvironment) {
		t.Fatalf("launcher environment mismatch: %#v", launcher.Environment)
	}
	if !reflect.DeepEqual(specs[0].Environment, []string{
		"XDG_RUNTIME_DIR=/run/polaris",
		"DBUS_SESSION_BUS_ADDRESS=unix:path=/run/polaris/bus",
	}) {
		t.Fatalf("session bus environment was over-broad: %#v", specs[0].Environment)
	}
	if !reflect.DeepEqual(specs[5].Environment, []string{
		"POLARIS_RENDER_NODE=" + config.RenderNode,
	}) {
		t.Fatalf("encoder environment was over-broad: %#v", specs[5].Environment)
	}
}

func TestProcessRuntimeAdapterConfigurationFailsBeforeHostTouch(t *testing.T) {
	tests := []struct {
		name     string
		host     runtimeProcessHost
		options  processRuntimeAdapterOptions
		contains string
	}{
		{
			name: "missing host",
			options: processRuntimeAdapterOptions{
				RuntimeProfile: "steam",
			},
			contains: "process host is missing",
		},
		{
			name: "typed nil host",
			host: (*fakeRuntimeProcessHost)(nil),
			options: processRuntimeAdapterOptions{
				RuntimeProfile: "steam",
			},
			contains: "process host is missing",
		},
		{
			name: "relative helper",
			host: &fakeRuntimeProcessHost{},
			options: processRuntimeAdapterOptions{
				HelperExecutable: "polaris-seat-runtime",
				RuntimeProfile:   "steam",
			},
			contains: "helper path is invalid",
		},
		{
			name: "unknown profile",
			host: &fakeRuntimeProcessHost{},
			options: processRuntimeAdapterOptions{
				RuntimeProfile: "automatic",
			},
			contains: "runtime profile is invalid",
		},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			_, err := newProcessRuntimeAdapters(test.host, test.options)
			if err == nil || !strings.Contains(err.Error(), test.contains) {
				t.Fatalf("unexpected adapter validation result: %v", err)
			}
			if host, ok := test.host.(*fakeRuntimeProcessHost); ok && host != nil {
				_, calls := host.snapshot()
				if calls != 0 {
					t.Fatalf("adapter validation touched the host %d times", calls)
				}
			}
		})
	}
}

func TestProcessRuntimeAdaptersAcceptOnlyLockedRuntimeProfiles(t *testing.T) {
	for _, profile := range []string{"gamescope", "steam", "heroic", "lutris"} {
		t.Run(profile, func(t *testing.T) {
			host := &fakeRuntimeProcessHost{recorder: &fakeRuntimeRecorder{}}
			adapters, err := newProcessRuntimeAdapters(host, processRuntimeAdapterOptions{
				RuntimeProfile: profile,
			})
			if err != nil {
				t.Fatal(err)
			}
			allocation := runtimeAllocationForTest(
				t,
				runtimeTestConfig("worker-profile-"+profile, 73, 0),
			)
			lease, err := adapters.LauncherProcessTree.Start(context.Background(), allocation)
			if err != nil {
				t.Fatal(err)
			}
			if err := lease.Stop(context.Background()); err != nil {
				t.Fatal(err)
			}
			specs, calls := host.snapshot()
			if calls != 1 || len(specs) != 1 ||
				!containsString(specs[0].Arguments, "--runtime-profile="+profile) ||
				!containsString(specs[0].Environment, "POLARIS_RUNTIME_PROFILE="+profile) {
				t.Fatalf("runtime profile was not bound literally: %#v", specs)
			}
		})
	}
}

func TestProcessRuntimeAdapterRejectsInvalidAllocationBeforeHostTouch(t *testing.T) {
	host := &fakeRuntimeProcessHost{recorder: &fakeRuntimeRecorder{}}
	adapters, err := newProcessRuntimeAdapters(host, processRuntimeAdapterOptions{
		RuntimeProfile: "steam",
	})
	if err != nil {
		t.Fatal(err)
	}
	allocation := runtimeAllocationForTest(
		t,
		runtimeTestConfig("worker-invalid-process-allocation", 75, 0),
	)
	allocation.RenderNode = "../../dev/dri/renderD128"
	lease, err := adapters.Compositor.Start(context.Background(), allocation)
	if lease != nil || err == nil || !strings.Contains(err.Error(), "adapter is invalid") {
		t.Fatalf("invalid process allocation was accepted: %#v, %v", lease, err)
	}
	_, calls := host.snapshot()
	if calls != 0 {
		t.Fatalf("invalid allocation touched the process host %d times", calls)
	}
}

func containsString(values []string, expected string) bool {
	for _, value := range values {
		if value == expected {
			return true
		}
	}
	return false
}

func TestProcessRuntimeAdapterRedactsHostFailureAndReturnsPartialLease(t *testing.T) {
	recorder := &fakeRuntimeRecorder{}
	partial := newFakeRuntimeLease(runtimeStageSessionBus, recorder)
	host := &fakeRuntimeProcessHost{
		recorder: recorder,
		start: func(runtimeProcessSpec) (runtimeLease, error) {
			return partial, errors.New("private executable and device detail")
		},
	}
	adapters, err := newProcessRuntimeAdapters(host, processRuntimeAdapterOptions{
		RuntimeProfile: "gamescope",
	})
	if err != nil {
		t.Fatal(err)
	}
	lease, err := adapters.SessionBus.Start(
		context.Background(),
		runtimeAllocationForTest(t, runtimeTestConfig("worker-redaction", 72, 0)),
	)
	if lease != partial || err == nil || !strings.Contains(err.Error(), "did not become ready") {
		t.Fatalf("unexpected partial start result: %#v, %v", lease, err)
	}
	if strings.Contains(err.Error(), "private") || strings.Contains(err.Error(), "device") {
		t.Fatalf("process host detail escaped adapter boundary: %v", err)
	}
}

func runtimeAllocationForTest(t *testing.T, config workerConfig) runtimeAllocation {
	t.Helper()
	allocation, err := runtimeAllocationFromConfig(config)
	if err != nil {
		t.Fatal(err)
	}
	return allocation
}
