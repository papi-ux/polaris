//go:build linux

package main

import "testing"

func TestStreamingWorkerRejectsMissingImageAccountBeforeResources(t *testing.T) {
	config := runtimeTestConfig("missing-image-user", 73, 0)
	config.DisplayHDR = false
	config.Compositor, config.RuntimeProfile = "gamescope", "gamescope"
	// UID -1 cannot represent a kernel user. Empty authority paths must not
	// be reached, and no provider may start before this image check passes.
	for _, workload := range []workloadPlan{
		{Kind: workloadKindGamescope, TargetID: "input-pong-v1"},
		{Kind: workloadKindSteam, TargetID: "big-picture-v1"},
		{Kind: workloadKindSteam, TargetID: "570"},
	} {
		config.Workload = workload
		config.RuntimeProfile = string(workload.Kind)
		err := runProductionSeatWorker(t.Context(), config, workerPaths{}, ^uint32(0))
		if err == nil || err.Error() != "worker UID has no account in the runtime image" {
			t.Fatalf("%s: missing image account did not fail before resource admission: %v", workload.TargetID, err)
		}
	}
}

func TestStreamingWorkerRejectsUntrustedSteamTargetBeforeResources(t *testing.T) {
	config := runtimeTestConfig("untrusted-steam-target", 74, 0)
	config.DisplayHDR = false
	config.Compositor, config.RuntimeProfile = "gamescope", "steam"
	for _, target := range []string{"0", "01", "570 --login user", "steam://rungameid/570", "/bin/sh"} {
		config.Workload = workloadPlan{Kind: workloadKindSteam, TargetID: target}
		err := runProductionSeatWorker(t.Context(), config, workerPaths{}, ^uint32(0))
		if err == nil || err.Error() != "streaming worker allocation is not supported" {
			t.Fatalf("untrusted target reached resource admission: %v", err)
		}
	}
}
