//go:build linux

package main

import (
	"context"
	"errors"
	"os"
	"os/user"
	"path/filepath"
	"strconv"
)

// The literal final option is controller-owned. Environment variables cannot
// activate gameplay, and the default supervisor mode remains available to the
// lifecycle and isolated physical probes.
func parseWorkerRunMode(arguments []string) (workloadPlan, bool, error) {
	enabled := len(arguments) > 0 && arguments[len(arguments)-1] == "--media=enabled"
	if enabled {
		arguments = arguments[:len(arguments)-1]
	}
	workload, err := parseRunArguments(arguments)
	return workload, enabled, err
}

func runProductionSeatWorker(parent context.Context, config workerConfig, paths workerPaths, uid uint32) error {
	if parent == nil || uid == 0 || config.DisplayHDR || config.RuntimeProfile != "gamescope" ||
		config.Compositor != "gamescope" || config.Workload.Kind != workloadKindGamescope || config.Workload.TargetID != "input-pong-v1" {
		return errors.New("streaming worker allocation is not supported")
	}
	// D-Bus requires an NSS entry even with numeric EXTERNAL authentication.
	// Docker does not synthesize /etc/passwd entries for --user as Podman can.
	// Reject before creating any runtime resources when the image lacks it.
	if _, err := user.LookupId(strconv.FormatUint(uint64(uid), 10)); err != nil {
		return errors.New("worker UID has no account in the runtime image")
	}
	// Authenticate the local authority before any provider can touch resources.
	for _, directory := range []string{paths.IPC, paths.Auth, paths.State} {
		if err := privateDirectory(directory, uid); err != nil {
			return err
		}
	}
	if _, err := readCapability(filepath.Join(paths.Auth, capabilityFileName), uid); err != nil {
		return err
	}
	adapters, err := newProcessRuntimeAdapters(osRuntimeProcessHost{diagnostics: os.Stderr}, processRuntimeAdapterOptions{CompositorInput: true})
	if err != nil {
		return err
	}
	source := newEncoderMediaSource(config, "/run/polaris", uid)
	defer source.Close()
	// Input/rumble are already routed by the host's generation-bound input
	// authority. This media plane must never provide a second injection route.
	plane := newSeatDataPlane(config.Identity, source, nil)
	return runWorkerWithRuntimeAndDataPlane(parent, config, paths, uid, &adapters, plane, runtimeOptions{})
}
