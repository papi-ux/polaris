//go:build linux

package seatprovider

import (
	"strings"
	"testing"

	"github.com/papi-ux/polaris/multiseat_worker/internal/seatruntime"
)

func heroicRequest(target string) seatruntime.Request {
	return seatruntime.Request{
		Stage:            seatruntime.StageLauncher,
		RuntimeNamespace: "launcher-test",
		RuntimeProfile:   "heroic",
		WorkloadKind:     seatruntime.WorkloadHeroic,
		WorkloadID:       target,
		WaylandSocket:    "polaris-wayland-test",
		AudioSink:        "audio-test",
		InputSeat:        "input-test",
	}
}

func TestHeroicLibraryOpensTheLauncherItself(t *testing.T) {
	command, err := planLauncher(heroicRequest(seatruntime.LauncherLibrary))
	if err != nil {
		t.Fatal(err)
	}
	if command.executable != heroicExecutable {
		t.Errorf("executable = %q, want the image's own Heroic", command.executable)
	}
	want := []string{heroicExecutable, "--no-sandbox"}
	if len(command.arguments) != len(want) {
		t.Fatalf("arguments = %v, want exactly %v", command.arguments, want)
	}
	for index := range want {
		if command.arguments[index] != want[index] {
			t.Fatalf("arguments = %v, want %v", command.arguments, want)
		}
	}
	if !command.retainDescendants {
		t.Error("Heroic starts helpers that outlive the first process")
	}
}

func TestHeroicTitleRebuildsItsDeepLinkFromTheValidatedToken(t *testing.T) {
	command, err := planLauncher(heroicRequest("epic.Fortnite"))
	if err != nil {
		t.Fatal(err)
	}
	last := command.arguments[len(command.arguments)-1]
	if last != "heroic://launch/epic/Fortnite" {
		t.Errorf("deep link = %q", last)
	}
	for _, argument := range command.arguments {
		if strings.ContainsAny(argument, " ;|&$`") {
			t.Errorf("argument %q carries shell punctuation", argument)
		}
	}
}

func TestHeroicRefusesATargetThatIsNotARunnerAndAnApplication(t *testing.T) {
	for _, target := range []string{"Fortnite", "epic.", ".Fortnite", "heroic://launch/epic/Fortnite", "440"} {
		if _, err := planLauncher(heroicRequest(target)); err == nil {
			t.Errorf("%q must not plan a launch", target)
		}
	}
}

func TestSteamPlanningIsUnchangedByTheFamilySwitch(t *testing.T) {
	request := heroicRequest("440")
	request.RuntimeProfile = "steam"
	request.WorkloadKind = seatruntime.WorkloadSteam
	command, err := planLauncher(request)
	if err != nil {
		t.Fatal(err)
	}
	if command.executable != "/usr/bin/bash" || command.packageScript != "/usr/games/steam" {
		t.Fatalf("steam command changed: %+v", command)
	}
	last := command.arguments[len(command.arguments)-1]
	if last != "440" {
		t.Errorf("steam arguments = %v", command.arguments)
	}
}
