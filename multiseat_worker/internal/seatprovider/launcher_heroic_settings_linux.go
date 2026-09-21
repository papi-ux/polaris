//go:build linux

package seatprovider

import (
	"encoding/json"
	"errors"
	"os"
	"syscall"
)

// What a new Heroic home starts with. Heroic asks GitHub for a newer release at
// every start and, finding one, opens a dialog that tells the player to use
// their package manager. In a Space there is none: the launcher is part of the
// runtime and arrives with a runtime update. The pinned Heroic has one switch
// for this and it lives in its settings file; it reads no environment variable
// or argument, and removing its update feed from the image raises an error
// dialog instead.
//
// The three prefix paths are here because of how that Heroic merges a partial
// file over its defaults: it copies defaultWinePrefix into a missing
// defaultWinePrefixDir and spreads winePrefix last, so leaving them out would
// blank all three. They are the values Heroic chooses for this home itself.
func heroicInitialSettings(home string) ([]byte, error) {
	prefixes := home + "/Games/Heroic/Prefixes"
	return json.MarshalIndent(map[string]any{
		"version": "v0",
		"defaultSettings": map[string]any{
			"checkForUpdatesOnStartup": false,
			"defaultWinePrefix":        prefixes,
			"defaultWinePrefixDir":     prefixes,
			"winePrefix":               prefixes + "/shared",
		},
	}, "", "  ")
}

// Writes those settings into a home that has none. A settings file that exists
// is the player's, whatever it holds, and is never opened: the exclusive create
// is the whole check, and it refuses a symlink as readily as a file. Every step
// is taken from the verified home descriptor and follows no link.
func seedHeroicSettings(home *runtimeDirectory) error {
	if home == nil || home.file == nil {
		return errors.New("launcher profile is unavailable")
	}
	directory := int(home.file.Fd())
	owned := []int{}
	defer func() {
		for _, descriptor := range owned {
			_ = syscall.Close(descriptor)
		}
	}()
	for _, name := range []string{".config", "heroic"} {
		if err := syscall.Mkdirat(directory, name, 0o700); err != nil && !errors.Is(err, syscall.EEXIST) {
			return errors.New("launcher settings directory could not be created")
		}
		next, err := syscall.Openat(directory, name,
			syscall.O_RDONLY|syscall.O_DIRECTORY|syscall.O_NOFOLLOW|syscall.O_CLOEXEC, 0)
		if err != nil {
			return errors.New("launcher settings directory is unavailable")
		}
		owned = append(owned, next)
		directory = next
	}
	descriptor, err := syscall.Openat(directory, "config.json",
		syscall.O_WRONLY|syscall.O_CREAT|syscall.O_EXCL|syscall.O_NOFOLLOW|syscall.O_CLOEXEC, 0o600)
	if errors.Is(err, syscall.EEXIST) {
		return nil
	}
	if err != nil {
		return errors.New("launcher settings could not be created")
	}
	file := os.NewFile(uintptr(descriptor), "heroic-settings")
	settings, err := heroicInitialSettings(home.path)
	if err == nil {
		_, err = file.Write(append(settings, '\n'))
	}
	if closeErr := file.Close(); err == nil {
		err = closeErr
	}
	if err != nil {
		// Half a settings file would stand as the player's own from then on,
		// and nothing here would ever replace it.
		_ = syscall.Unlinkat(directory, "config.json")
		return errors.New("launcher settings could not be written")
	}
	return nil
}
