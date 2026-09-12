//go:build linux

package main

import "testing"

func TestStreamingWorkerRejectsMissingImageAccountBeforeResources(t *testing.T) {
	config := runtimeTestConfig("missing-image-user", 73, 0)
	config.DisplayHDR = false
	config.Compositor, config.RuntimeProfile = "gamescope", "gamescope"
	config.Workload = workloadPlan{Kind: workloadKindGamescope, TargetID: "input-pong-v1"}
	// UID -1 cannot represent a kernel user. Empty authority paths must not
	// be reached, and no provider may start before this image check passes.
	err := runProductionSeatWorker(t.Context(), config, workerPaths{}, ^uint32(0))
	if err == nil || err.Error() != "worker UID has no account in the runtime image" {
		t.Fatalf("missing image account did not fail before resource admission: %v", err)
	}
}
