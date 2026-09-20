package seatruntime

import (
	"encoding/json"
	"os"
	"path/filepath"
	"testing"
)

// The same grammar is written twice, here and in C++. A shared vector keeps the
// two from drifting: a target either side accepts alone would be a target the
// host admits and the worker refuses, or worse.
const sharedTargetVectorPath = "../../../tests/fixtures/launcher-targets.json"

type targetCase struct {
	Profile  string `json:"profile"`
	Kind     string `json:"kind"`
	Target   string `json:"target"`
	Accepted bool   `json:"accepted"`
	Why      string `json:"why"`
}

func TestLauncherTargetsMatchTheSharedVector(t *testing.T) {
	payload, err := os.ReadFile(filepath.Clean(sharedTargetVectorPath))
	if err != nil {
		t.Fatalf("the shared vector must be readable from both languages: %v", err)
	}
	var cases []targetCase
	if err := json.Unmarshal(payload, &cases); err != nil {
		t.Fatal(err)
	}
	if len(cases) < 20 {
		t.Fatalf("the shared vector is too small to be meaningful: %d", len(cases))
	}
	for _, entry := range cases {
		accepted := StreamingWorkloadSupported(entry.Profile, WorkloadKind(entry.Kind), entry.Target)
		if accepted != entry.Accepted {
			t.Errorf("%s/%s %q: accepted=%v, want %v (%s)",
				entry.Profile, entry.Kind, entry.Target, accepted, entry.Accepted, entry.Why)
		}
	}
}

func TestHeroicTargetsRebuildFromTwoValidatedPieces(t *testing.T) {
	for _, target := range []string{"epic.Fortnite", "gog.1207658924", "amazon.amzn1_ADG_PRODUCT", "sideload.my-game", LauncherLibrary} {
		if !ValidHeroicTarget(target) {
			t.Errorf("%q should be a Heroic target", target)
		}
	}
	for _, target := range []string{
		"", ".", "epic.", ".Fortnite", "steam.440", "epic..Fortnite", "epic./etc/passwd",
		"epic.heroic://launch", "epic.-leading", "epic.a b", "library-v2",
	} {
		if ValidHeroicTarget(target) {
			t.Errorf("%q must not be a Heroic target", target)
		}
	}
}

func TestLutrisTargetsAreNumbered(t *testing.T) {
	for _, target := range []string{"id.1", "id.4294967295", LauncherLibrary} {
		if !ValidLutrisTarget(target) {
			t.Errorf("%q should be a Lutris target", target)
		}
	}
	for _, target := range []string{"1", "id.", "id.0", "id.01", "id.-1", "id.big-picture-v1", "slug.hades"} {
		if ValidLutrisTarget(target) {
			t.Errorf("%q must not be a Lutris target", target)
		}
	}
}

func TestAFamilyNeverAcceptsAnotherFamilysTarget(t *testing.T) {
	crossed := []struct {
		profile string
		kind    WorkloadKind
		target  string
	}{
		{"steam", WorkloadSteam, "epic.Fortnite"},
		{"steam", WorkloadSteam, LauncherLibrary},
		{"heroic", WorkloadHeroic, "440"},
		{"heroic", WorkloadHeroic, SteamBigPicture},
		{"lutris", WorkloadLutris, "epic.Fortnite"},
		{"heroic", WorkloadSteam, "epic.Fortnite"},
		{"steam", WorkloadHeroic, "440"},
	}
	for _, entry := range crossed {
		if StreamingWorkloadSupported(entry.profile, entry.kind, entry.target) {
			t.Errorf("%s/%s must refuse %q", entry.profile, entry.kind, entry.target)
		}
	}
}
