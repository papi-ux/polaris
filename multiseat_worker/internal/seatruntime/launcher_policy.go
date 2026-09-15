package seatruntime

import "strconv"

const SteamBigPicture = "big-picture-v1"

// Steam targets are a typed application identity, never a URL, path, login
// credential or command line. Canonical decimal prevents alternate spellings.
func ValidSteamTarget(target string) bool {
	if target == SteamBigPicture {
		return true
	}
	if len(target) == 0 || len(target) > 10 {
		return false
	}
	value, err := strconv.ParseUint(target, 10, 32)
	return err == nil && value != 0 && strconv.FormatUint(value, 10) == target
}

func StreamingWorkloadSupported(profile string, kind WorkloadKind, target string) bool {
	return (profile == "gamescope" && kind == WorkloadGamescope && target == "input-pong-v1") ||
		(profile == "steam" && kind == WorkloadSteam && ValidSteamTarget(target))
}
