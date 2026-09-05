//go:build linux

package main

import (
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
	workload, err := requiredEnvironment(lookup, "POLARIS_INTEROP_WORKLOAD_KEY")
	if err != nil {
		t.Fatal(err)
	}
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
	if err := runWorkerWithRuntime(
		context.Background(),
		config,
		workerPaths{IPC: ipc, Auth: auth, State: state},
		uint32(os.Geteuid()),
		&runtimeSet.adapters,
		defaultRuntimeOptions(),
	); err != nil {
		t.Fatal(err)
	}
	events, _ := runtimeSet.recorder.snapshot()
	if !reflect.DeepEqual(events, completeRuntimeEvents()) {
		t.Fatalf("authenticated shutdown runtime order mismatch: %#v", events)
	}
}
