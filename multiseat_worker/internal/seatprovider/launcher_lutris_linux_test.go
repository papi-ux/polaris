//go:build linux

package seatprovider

import (
	"slices"
	"strings"
	"testing"

	"github.com/papi-ux/polaris/multiseat_worker/internal/seatruntime"
)

func lutrisRequest(target string) seatruntime.Request {
	request := heroicRequest(target)
	request.RuntimeProfile = "lutris"
	request.WorkloadKind = seatruntime.WorkloadLutris
	return request
}

func TestLutrisLibraryRunsThePackageScriptThroughItsInterpreter(t *testing.T) {
	command, err := planLauncher(lutrisRequest(seatruntime.LauncherLibrary))
	if err != nil {
		t.Fatal(err)
	}
	// A link is never followed to find an executable, and python3 is one.
	if command.executable != lutrisInterpreter || strings.HasSuffix(command.executable, "/python3") {
		t.Errorf("executable = %q, want the interpreter file itself", command.executable)
	}
	if command.packageScript != lutrisScript {
		t.Errorf("package script = %q, so the script would start unchecked", command.packageScript)
	}
	if !slices.Equal(command.arguments, []string{lutrisScript}) {
		t.Errorf("arguments = %v, want only the script", command.arguments)
	}
	if !command.retainDescendants {
		t.Error("Lutris hands the game to Wine and the session must outlive that")
	}
}

func TestLutrisTitleRebuildsItsAddressFromTheValidatedToken(t *testing.T) {
	command, err := planLauncher(lutrisRequest("id.42"))
	if err != nil {
		t.Fatal(err)
	}
	if !slices.Equal(command.arguments, []string{lutrisScript, "lutris:rungameid/42"}) {
		t.Errorf("arguments = %v", command.arguments)
	}
}

func TestLutrisRefusesATargetThatIsNotAGameNumber(t *testing.T) {
	for _, target := range []string{"42", "id.", "id.0", "id.04", "id.-1", "id.4 2", "slug.quake", "lutris:rungameid/42",
		"epic.Fortnite", "big-picture-v1"} {
		if _, err := planLauncher(lutrisRequest(target)); err == nil {
			t.Errorf("%q must not plan a launch", target)
		}
	}
}

func TestALauncherIsNeverStartedAsAnotherFamilys(t *testing.T) {
	// Before Lutris had a case of its own it fell through to Steam's command,
	// in an image that has no Steam.
	for _, request := range []seatruntime.Request{lutrisRequest(seatruntime.LauncherLibrary), heroicRequest(seatruntime.LauncherLibrary)} {
		command, err := planLauncher(request)
		if err != nil {
			t.Fatal(err)
		}
		if command.executable == "/usr/bin/bash" || command.packageScript == "/usr/games/steam" {
			t.Errorf("%s was planned as Steam: %+v", request.WorkloadKind, command)
		}
	}
}

func TestEveryWineFamilySeesTheSeatsGamepad(t *testing.T) {
	for _, request := range []seatruntime.Request{lutrisRequest(seatruntime.LauncherLibrary), heroicRequest(seatruntime.LauncherLibrary)} {
		environment, err := launcherEnvironment(request, launcherSession{display: ":0", width: 1920, height: 1080, refresh: 60000})
		if err != nil {
			t.Fatal(err)
		}
		if !slices.Contains(environment, "SDL_JOYSTICK_DEVICE=/dev/input/polaris-gamepad-0") {
			t.Errorf("%s was not pointed at the seat's gamepad", request.WorkloadKind)
		}
	}
}
