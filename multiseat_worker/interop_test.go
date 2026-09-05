//go:build linux

package main

import (
	"bytes"
	"context"
	"os"
	"reflect"
	"testing"
)

// TestNativeControllerInteropServer is launched as a standalone go test binary
// by the C++ integration fixture. It exercises the real Go worker server and
// injected runtime lifecycle while keeping resource activation outside the
// production worker CLI.
func TestNativeControllerInteropServer(t *testing.T) {
	if os.Getenv("POLARIS_NATIVE_INTEROP") != "1" {
		t.Skip("native controller interop helper is not active")
	}
	if os.Geteuid() == 0 {
		t.Fatal("native controller interop refuses root")
	}
	lookup := os.LookupEnv
	workloadKindValue, err := requiredEnvironment(lookup, "POLARIS_INTEROP_WORKLOAD_KIND")
	if err != nil {
		t.Fatal(err)
	}
	workloadID, err := requiredEnvironment(lookup, "POLARIS_INTEROP_WORKLOAD_ID")
	if err != nil {
		t.Fatal(err)
	}
	workload := workloadPlan{Kind: workloadKind(workloadKindValue), TargetID: workloadID}
	config, err := loadWorkerConfig(lookup, workload)
	if err != nil {
		t.Fatal(err)
	}
	ipc, err := requiredEnvironment(lookup, "POLARIS_INTEROP_IPC_PATH")
	if err != nil {
		t.Fatal(err)
	}
	auth, err := requiredEnvironment(lookup, "POLARIS_INTEROP_AUTH_PATH")
	if err != nil {
		t.Fatal(err)
	}
	state, err := requiredEnvironment(lookup, "POLARIS_INTEROP_STATE_PATH")
	if err != nil {
		t.Fatal(err)
	}
	runtimeSet := newFakeRuntimeSet()
	dataPlane := newFakeWorkerDataPlane()
	dataPlane.feedback <- routedOutput{
		Identity: config.Identity,
		Message:  messageFeedback,
		Payload:  []byte("native-feedback"),
	}
	dataPlane.media <- routedOutput{
		Identity: config.Identity,
		Message:  messageVideo,
		Payload:  []byte("native-video"),
	}
	if err := runWorkerWithRuntimeAndDataPlane(
		context.Background(),
		config,
		workerPaths{IPC: ipc, Auth: auth, State: state},
		uint32(os.Geteuid()),
		&runtimeSet.adapters,
		dataPlane,
		defaultRuntimeOptions(),
	); err != nil {
		t.Fatal(err)
	}
	events, _ := runtimeSet.recorder.snapshot()
	if !reflect.DeepEqual(events, completeRuntimeEvents()) {
		t.Fatalf("authenticated shutdown runtime order mismatch: %#v", events)
	}
	select {
	case input := <-dataPlane.inputs:
		if input.Identity != config.Identity || !bytes.Equal(input.Payload, []byte("native-input")) {
			t.Fatalf("native controller input was misrouted: %+v", input)
		}
	default:
		t.Fatal("native controller input did not reach the worker data plane")
	}
}
