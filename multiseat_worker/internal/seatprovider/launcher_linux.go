//go:build linux

package seatprovider

import (
	"context"
	"errors"
	"io"
	"os"
	"path/filepath"
	"strconv"
	"time"

	"github.com/papi-ux/polaris/multiseat_worker/internal/seatinput"
	"github.com/papi-ux/polaris/multiseat_worker/internal/seatruntime"
)

const launcherHome = "/var/lib/polaris-seat"

// Workloads are image-owned executable policy. Controller input selects only
// this bounded key; paths, argv, shell text and ambient environment never cross.
func launcherExecutable(request seatruntime.Request) (string, error) {
	if _, err := seatruntime.Arguments(request); err != nil {
		return "", err
	}
	if request.Stage == seatruntime.StageLauncher && request.WorkloadKind == seatruntime.WorkloadGamescope && request.WorkloadID == "input-pong-v1" {
		return "/usr/libexec/polaris-seat/workloads/input-pong-v1", nil
	}
	return "", errors.New("workload is not implemented in this image")
}

func launcherEnvironment(request seatruntime.Request, session launcherSession) ([]string, error) {
	environment, err := seatruntime.Environment(request)
	if err != nil {
		return nil, err
	}
	return append(environment,
		"PATH=/usr/bin", "LC_ALL=C",
		"DISPLAY="+session.display, "STEAM_GAME_DISPLAY_0="+session.display,
		"GAMESCOPE_WAYLAND_DISPLAY="+request.WaylandSocket,
		"POLARIS_DISPLAY_WIDTH="+strconv.FormatUint(uint64(session.width), 10),
		"POLARIS_DISPLAY_HEIGHT="+strconv.FormatUint(uint64(session.height), 10),
		"POLARIS_DISPLAY_REFRESH_MILLIHZ="+strconv.FormatUint(uint64(session.refresh), 10),
		"GST_PLUGIN_PATH=", "GST_PLUGIN_PATH_1_0=", "GST_REGISTRY=/dev/null", "GST_REGISTRY_1_0=/dev/null",
	), nil
}

func RunLauncher(arguments, environment []string) error {
	request, err := parseProviderInvocation(seatruntime.StageLauncher, arguments, environment)
	if err != nil {
		return err
	}
	ready, err := openReadinessWriter()
	if err != nil {
		return err
	}
	parent, cancel := signalContext()
	defer cancel()
	return runLauncher(parent, request, ready, defaultProviderOptions())
}

func runLauncher(parent context.Context, request seatruntime.Request, ready io.WriteCloser, options providerOptions) (result error) {
	if parent == nil || ready == nil {
		if ready != nil {
			_ = ready.Close()
		}
		return errors.New("launcher invocation is invalid")
	}
	defer ready.Close()
	executable, err := launcherExecutable(request)
	if err != nil {
		return err
	}
	options, err = normalizeProviderOptions(options)
	if err != nil {
		return err
	}
	runtime, err := openRuntimeDirectory(options.runtimeDirectory, options.runtimeOwnerUID)
	if err != nil {
		return err
	}
	defer runtime.close()
	// The volume is private to this seat. The provider never initializes,
	// chmods or recursively changes an existing user's launcher profile.
	home, err := openRuntimeDirectory(launcherHome, options.runtimeOwnerUID)
	if err != nil {
		return errors.New("launcher profile must be a private owned directory")
	}
	defer home.close()
	inputs, err := seatinput.Open(seatinput.Directory, request.InputSeat)
	if err != nil {
		return err
	}
	defer inputs.Close()
	session, err := readLauncherSession(parent, request, runtime, options)
	if err != nil {
		return err
	}
	environment, err := launcherEnvironment(request, session)
	if err != nil {
		return err
	}
	if err := parent.Err(); err != nil {
		return err
	}
	if err := enableWorkloadSubreaper(); err != nil {
		return err
	}
	// All prerequisites and probes finish before starting the only workload
	// child; subsequent Wait4 calls can therefore reap only owned descendants.
	if current, err := os.Getwd(); err != nil || filepath.Clean(current) != launcherHome {
		return errors.New("launcher working directory is invalid")
	}
	child, err := startManagedChildWithUmask(executable, options.executableOwnerUID, nil, environment, nil, 0o077)
	if err != nil {
		return err
	}
	defer func() { result = errors.Join(result, stopWorkload(child, 4*time.Second)) }()
	if child.exited() {
		return errors.New("workload exited during startup")
	}
	if err := publishReadiness(ready); err != nil {
		return err
	}
	// This readiness means supervised process startup. Frame production and
	// successful client presentation are separate media/acceptance evidence.
	ticker := time.NewTicker(time.Second)
	defer ticker.Stop()
	for {
		select {
		case <-parent.Done():
			return nil
		case <-child.done:
			return nil
		case <-ticker.C:
			if err := inputs.Verify(); err != nil {
				return err
			}
			if err := runtime.verify(); err != nil {
				return err
			}
			if err := home.verify(); err != nil {
				return err
			}
		}
	}
}
