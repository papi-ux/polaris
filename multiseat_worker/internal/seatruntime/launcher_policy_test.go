package seatruntime

import "testing"

func TestSteamTargetsAreCanonicalAndCannotCarryCommands(t *testing.T) {
	for _, target := range []string{SteamBigPicture, "1", "570", "4294967295"} {
		if !ValidSteamTarget(target) || !StreamingWorkloadSupported("steam", WorkloadSteam, target) {
			t.Fatalf("valid Steam target rejected: %q", target)
		}
	}
	for _, target := range []string{"", "0", "01", "+1", "-1", "4294967296", "1 --login user", "steam://rungameid/1", "/bin/sh", "$(id)", "1\n", "big-picture-v2"} {
		if ValidSteamTarget(target) || StreamingWorkloadSupported("steam", WorkloadSteam, target) {
			t.Fatalf("invalid Steam target admitted: %q", target)
		}
	}
	if StreamingWorkloadSupported("gamescope", WorkloadSteam, "570") ||
		StreamingWorkloadSupported("steam", WorkloadGamescope, "input-pong-v1") ||
		StreamingWorkloadSupported("heroic", WorkloadHeroic, "570") {
		t.Fatal("unsupported or mismatched image family admitted")
	}
}
