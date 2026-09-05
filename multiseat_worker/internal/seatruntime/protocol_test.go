package seatruntime

import (
	"reflect"
	"slices"
	"strings"
	"testing"
)

func protocolTestRequests() []Request {
	const namespace = "seat-7-generation-19"
	return []Request{
		{Stage: StageSessionBus, RuntimeNamespace: namespace},
		{
			Stage: StageAudio, RuntimeNamespace: namespace,
			AudioSink: "polaris-seat-7",
		},
		{
			Stage: StageDisplayCapture, RuntimeNamespace: namespace,
			CaptureWaylandSocket: "polaris-capture-7",
			RenderNode:           "/dev/dri/renderD128",
			DisplayTopology:      DisplayTopologyCaptureHostNested,
			MediaPipeline:        MediaPipelineWorkerLocal,
			DisplayWidth:         3840, DisplayHeight: 2160,
			DisplayRefreshMillihertz: 97000, DisplayHDR: true,
		},
		{
			Stage: StageNestedCompositor, RuntimeNamespace: namespace,
			ParentWaylandSocket: "polaris-capture-7",
			WaylandSocket:       "polaris-wayland-7",
			RenderNode:          "/dev/dri/renderD128", Compositor: "gamescope",
		},
		{
			Stage: StageVirtualInput, RuntimeNamespace: namespace,
			InputSeat: "polaris-input-7",
		},
		{
			Stage: StageEncoder, RuntimeNamespace: namespace,
			LogicalGPU: "gpu-amd-0", RenderNode: "/dev/dri/renderD128",
			EncoderSessions: 1, MediaPipeline: MediaPipelineWorkerLocal,
		},
		{
			Stage: StageLauncher, RuntimeNamespace: namespace,
			RuntimeProfile: "heroic", WorkloadKind: WorkloadHeroic,
			WorkloadID: "heroic-catalog-id", WaylandSocket: "polaris-wayland-7",
			AudioSink: "polaris-seat-7", InputSeat: "polaris-input-7",
		},
	}
}

func invocationForTest(t *testing.T, request Request) ([]string, []string) {
	t.Helper()
	arguments, err := Arguments(request)
	if err != nil {
		t.Fatal(err)
	}
	environment, err := Environment(request)
	if err != nil {
		t.Fatal(err)
	}
	environment = append(environment, ReadyFDSetting+"=3")
	return arguments, environment
}

func TestCanonicalInvocationsRoundTripEveryStage(t *testing.T) {
	for _, request := range protocolTestRequests() {
		t.Run(string(request.Stage), func(t *testing.T) {
			arguments, environment := invocationForTest(t, request)
			parsed, err := ParseInvocation(arguments, environment)
			if err != nil {
				t.Fatal(err)
			}
			if parsed != request {
				t.Fatalf("round trip changed request: %#v", parsed)
			}
			reordered := append([]string(nil), environment...)
			slices.Reverse(reordered)
			parsed, err = ParseInvocation(arguments, reordered)
			if err != nil || parsed != request {
				t.Fatalf("environment order changed authority: %#v, %v", parsed, err)
			}
		})
	}
}

func TestInvocationRejectsNonCanonicalOrExpandedAuthority(t *testing.T) {
	display := protocolTestRequests()[2]
	arguments, environment := invocationForTest(t, display)
	tests := map[string]func() ([]string, []string){
		"extra argument": func() ([]string, []string) {
			return append(append([]string(nil), arguments...), "--extra=value"), environment
		},
		"reordered argument": func() ([]string, []string) {
			changed := append([]string(nil), arguments...)
			changed[3], changed[4] = changed[4], changed[3]
			return changed, environment
		},
		"noncanonical decimal": func() ([]string, []string) {
			changed := append([]string(nil), arguments...)
			changed[7] = "--display-width=03840"
			return changed, environment
		},
		"wrong ready descriptor": func() ([]string, []string) {
			changed := append([]string(nil), environment...)
			changed[len(changed)-1] = ReadyFDSetting + "=4"
			return arguments, changed
		},
		"ambient setting": func() ([]string, []string) {
			return arguments, append(append([]string(nil), environment...), "PATH=/tmp")
		},
		"duplicate setting": func() ([]string, []string) {
			return arguments, append(append([]string(nil), environment...), environment[0])
		},
	}
	for name, mutate := range tests {
		t.Run(name, func(t *testing.T) {
			changedArguments, changedEnvironment := mutate()
			if _, err := ParseInvocation(changedArguments, changedEnvironment); err == nil {
				t.Fatal("expanded helper authority was accepted")
			}
		})
	}
}

func TestRequestValidationRejectsCrossStageAndUntypedLauncherData(t *testing.T) {
	session := protocolTestRequests()[0]
	session.AudioSink = "hidden-authority"
	if _, err := Arguments(session); err == nil {
		t.Fatal("cross-stage authority was accepted")
	}
	launcher := protocolTestRequests()[6]
	for name, mutate := range map[string]func(*Request){
		"profile mismatch": func(request *Request) {
			request.RuntimeProfile = "steam"
		},
		"shell fragment": func(request *Request) {
			request.WorkloadID = "game;touch-pwned"
		},
		"unknown kind": func(request *Request) {
			request.RuntimeProfile = "custom"
			request.WorkloadKind = "custom"
		},
	} {
		t.Run(name, func(t *testing.T) {
			changed := launcher
			mutate(&changed)
			if _, err := Arguments(changed); err == nil {
				t.Fatal("invalid launcher request was accepted")
			}
		})
	}
}

func TestInvocationErrorsDoNotEchoUntrustedValues(t *testing.T) {
	launcher := protocolTestRequests()[6]
	arguments, environment := invocationForTest(t, launcher)
	hostile := "secret-value;$(touch marker)"
	arguments[5] = "--workload-id=" + hostile
	_, err := ParseInvocation(arguments, environment)
	if err == nil || strings.Contains(err.Error(), hostile) {
		t.Fatalf("untrusted helper value leaked through error: %v", err)
	}
}

func TestProtocolConstantsRemainStable(t *testing.T) {
	if got, want := ReadyRecord, "POLARIS-RUNTIME-READY/1\n"; got != want {
		t.Fatalf("readiness record changed: %q", got)
	}
	if got, want := DefaultCatalogPath,
		"/run/polaris-auth/runtime-providers.json"; got != want {
		t.Fatalf("catalog path changed: %q", got)
	}
	if !reflect.DeepEqual(
		[]WorkloadKind{WorkloadGamescope, WorkloadSteam, WorkloadHeroic, WorkloadLutris},
		[]WorkloadKind{"gamescope", "steam", "heroic", "lutris"},
	) {
		t.Fatal("runtime workload vocabulary changed")
	}
}
