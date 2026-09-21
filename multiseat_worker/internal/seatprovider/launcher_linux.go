//go:build linux

package seatprovider

import (
	"context"
	"errors"
	"io"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"time"

	"github.com/papi-ux/polaris/multiseat_worker/internal/seatinput"
	"github.com/papi-ux/polaris/multiseat_worker/internal/seatruntime"
)

const launcherHome = "/var/lib/polaris-seat"

type launcherCommand struct {
	executable        string
	arguments         []string
	packageScript     string
	retainDescendants bool
}

// Where the image puts Heroic. The package the lock pins installs it here, and
// check-runtime.py asserts it before an image is accepted.
const heroicExecutable = "/opt/Heroic/heroic"

// Workloads are image-owned executable policy. Controller input selects only
// this bounded key; paths, argv, shell text and ambient environment never cross.
func planLauncher(request seatruntime.Request) (launcherCommand, error) {
	if _, err := seatruntime.Arguments(request); err != nil {
		return launcherCommand{}, err
	}
	if request.Stage != seatruntime.StageLauncher || !seatruntime.StreamingWorkloadSupported(request.RuntimeProfile, request.WorkloadKind, request.WorkloadID) {
		return launcherCommand{}, errors.New("workload is not implemented in this image")
	}
	switch request.WorkloadKind {
	case seatruntime.WorkloadGamescope:
		return launcherCommand{executable: "/usr/libexec/polaris-seat/workloads/input-pong-v1"}, nil
	case seatruntime.WorkloadHeroic:
		return planHeroicLauncher(request)
	}
	// The immutable package script sets STEAMSCRIPT from $0. Interpreting it
	// through its canonical path keeps Steam updates/restarts from inheriting
	// /proc/self/fd/3 as the launcher path. Both files are checked as trusted
	// image executables; no shell text or caller-supplied option is admitted.
	command := launcherCommand{
		executable: "/usr/bin/bash", packageScript: "/usr/games/steam",
		arguments: []string{"/usr/games/steam", "-gamepadui"}, retainDescendants: true,
	}
	if request.WorkloadID != seatruntime.SteamBigPicture {
		command.arguments = append(command.arguments, "-applaunch", request.WorkloadID)
	}
	return command, nil
}

// Heroic is an Electron application, and Electron's zygote wants a user
// namespace this container does not grant, so it runs with its own sandbox off.
// The deep link is rebuilt here from the validated token: the runner and the
// store's own identifier are the only two pieces that cross, and neither one
// reaches a shell.
func planHeroicLauncher(request seatruntime.Request) (launcherCommand, error) {
	command := launcherCommand{
		executable:        heroicExecutable,
		arguments:         []string{heroicExecutable, "--no-sandbox"},
		retainDescendants: true,
	}
	if request.WorkloadID == seatruntime.LauncherLibrary {
		return command, nil
	}
	runner, name, found := strings.Cut(request.WorkloadID, ".")
	if !found || runner == "" || name == "" {
		return launcherCommand{}, errors.New("heroic target is not a runner and an application")
	}
	command.arguments = append(command.arguments, "heroic://launch/"+runner+"/"+name)
	return command, nil
}

func launcherEnvironment(request seatruntime.Request, session launcherSession) ([]string, error) {
	environment, err := seatruntime.Environment(request)
	if err != nil {
		return nil, err
	}
	if request.WorkloadKind == seatruntime.WorkloadSteam || request.WorkloadKind == seatruntime.WorkloadHeroic {
		// Profile streams currently allocate at most one gamepad. SDL's Linux
		// discovery skips our reserved alias because it is not an eventN name
		// and this namespace has no host udev database. Select only that exact
		// host-admitted node. It stays absent when controller input is denied;
		// this hint neither creates a device nor grants access to another seat.
		environment = append(environment, "SDL_JOYSTICK_DEVICE=/dev/input/polaris-gamepad-0")
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
	command, err := planLauncher(request)
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
	if request.WorkloadKind == seatruntime.WorkloadHeroic {
		// The one exception, and only for a home Heroic has never started in:
		// see seedHeroicSettings. A home it cannot write to the way it expects,
		// a linked .config for one, is the player's arrangement and not a
		// reason to refuse the launch, so the result is deliberately dropped.
		_ = seedHeroicSettings(home)
	}
	inputs, err := seatinput.Open(seatinput.Directory, request.InputSeat)
	if err != nil {
		return err
	}
	defer inputs.Close()
	session, err := readLauncherSession(parent, request, runtime, options)
	if err != nil {
		return err
	}
	defer session.lifetime.close()
	environment, err := launcherEnvironment(request, session)
	if err != nil {
		return err
	}
	var steamBroker *steamInputBroker
	var steamDone <-chan struct{}
	if request.WorkloadKind == seatruntime.WorkloadSteam {
		path, name, err := inputs.SteamOutput()
		if err != nil {
			return err
		}
		if path != "" {
			for _, library := range []string{
				"/usr/lib/x86_64-linux-gnu/libpolaris-steam-input.so",
				"/usr/lib/i386-linux-gnu/libpolaris-steam-input.so",
			} {
				file, err := openTrustedExecutable(library, options.executableOwnerUID)
				if err != nil {
					return err
				}
				_ = file.Close()
			}
			sysname, err := inputs.SteamOutputSysname()
			if err != nil {
				return err
			}
			output, err := inputs.SteamOutputWriter()
			if err != nil {
				return err
			}
			defer output.Close()
			socket := filepath.Join(runtime.path, "polaris-steam-input.sock")
			steamBroker, err = startSteamInputBroker(parent, socket, options.runtimeOwnerUID, func(state steamInputState) error {
				events := steamInputEvents(state)
				n, err := output.Write(events)
				if err != nil || n != len(events) {
					return errors.New("Steam output device write failed")
				}
				return nil
			})
			if err != nil {
				return err
			}
			defer func() { result = errors.Join(result, steamBroker.close()) }()
			steamDone = steamBroker.done
			environment = append(environment, "LD_PRELOAD=libpolaris-steam-input.so",
				"POLARIS_STEAM_INPUT_SOCKET="+socket, "POLARIS_STEAM_INPUT_SYSNAME="+sysname, "POLARIS_STEAM_INPUT_NAME="+name)
		}
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
	if err := session.lifetime.verify(); err != nil {
		return err
	}
	if command.packageScript != "" {
		script, err := openTrustedExecutable(command.packageScript, options.executableOwnerUID)
		if err != nil {
			return err
		}
		defer script.Close()
	}
	child, err := startManagedChildWithUmask(command.executable, options.executableOwnerUID, command.arguments, environment, nil, 0o077)
	if err != nil {
		return err
	}
	defer func() { result = errors.Join(result, stopWorkload(child, 4*time.Second)) }()
	if err := session.lifetime.verify(); err != nil {
		return err
	}
	if child.exited() {
		if alive, err := launcherDescendantsAlive(command.retainDescendants); err != nil || !alive {
			return errors.Join(errors.New("workload exited during startup"), err)
		}
	}
	if err := publishReadiness(ready); err != nil {
		return err
	}
	// This readiness means supervised process startup. Frame production and
	// successful client presentation are separate media/acceptance evidence.
	ticker := time.NewTicker(time.Second)
	defer ticker.Stop()
	primaryDone := child.done
	for {
		select {
		case <-parent.Done():
			return nil
		case <-steamDone:
			if parent.Err() != nil {
				return nil
			}
			return errors.Join(errors.New("Steam input broker stopped"), steamBroker.verify())
		case <-primaryDone:
			primaryDone = nil
			if alive, err := launcherDescendantsAlive(command.retainDescendants); err != nil || !alive {
				return err
			}
		case <-ticker.C:
			if steamBroker != nil {
				if err := steamBroker.verify(); err != nil {
					return err
				}
			}
			if primaryDone == nil {
				if alive, err := launcherDescendantsAlive(command.retainDescendants); err != nil || !alive {
					return err
				}
			}
			if err := session.lifetime.verify(); err != nil {
				return err
			}
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
