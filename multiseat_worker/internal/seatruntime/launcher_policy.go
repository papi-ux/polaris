package seatruntime

import "strconv"

const (
	SteamBigPicture = "big-picture-v1"
	// Every launcher family has one sentinel that opens the launcher itself
	// rather than a title.
	LauncherLibrary = "library-v1"
)

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

// Heroic names a title by the store that sells it and that store's own
// identifier: `<runner>.<appName>`. The worker rebuilds Heroic's deep link from
// these two pieces and never receives one.
func ValidHeroicTarget(target string) bool {
	if target == LauncherLibrary {
		return true
	}
	runner, name, found := cut(target, '.')
	if !found {
		return false
	}
	switch runner {
	case "epic", "gog", "amazon", "sideload":
	default:
		return false
	}
	if len(name) == 0 || len(name) > 64 || !alphanumeric(name[0]) {
		return false
	}
	for index := 0; index < len(name); index++ {
		character := name[index]
		if !alphanumeric(character) && character != '_' && character != '-' {
			return false
		}
	}
	return true
}

// Lutris numbers a game in its own database, so a target is `id.<decimal>`.
func ValidLutrisTarget(target string) bool {
	if target == LauncherLibrary {
		return true
	}
	number, found := trimPrefix(target, "id.")
	return found && number != SteamBigPicture && ValidSteamTarget(number)
}

func StreamingWorkloadSupported(profile string, kind WorkloadKind, target string) bool {
	switch {
	case profile == "gamescope" && kind == WorkloadGamescope:
		return target == "input-pong-v1"
	case profile == "steam" && kind == WorkloadSteam:
		return ValidSteamTarget(target)
	case profile == "heroic" && kind == WorkloadHeroic:
		return ValidHeroicTarget(target)
	case profile == "lutris" && kind == WorkloadLutris:
		return ValidLutrisTarget(target)
	}
	return false
}

func alphanumeric(character byte) bool {
	return (character >= '0' && character <= '9') ||
		(character >= 'a' && character <= 'z') ||
		(character >= 'A' && character <= 'Z')
}

func cut(value string, separator byte) (string, string, bool) {
	for index := 0; index < len(value); index++ {
		if value[index] == separator {
			return value[:index], value[index+1:], true
		}
	}
	return value, "", false
}

func trimPrefix(value, prefix string) (string, bool) {
	if len(value) < len(prefix) || value[:len(prefix)] != prefix {
		return "", false
	}
	return value[len(prefix):], true
}
