//go:build linux

package seatprovider

import (
	"encoding/json"
	"os"
	"path/filepath"
	"testing"
)

func heroicHome(t *testing.T) *runtimeDirectory {
	t.Helper()
	path := t.TempDir()
	if err := os.Chmod(path, 0o700); err != nil {
		t.Fatal(err)
	}
	home, err := openRuntimeDirectory(path, uint32(os.Getuid()))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { home.close() })
	return home
}

func TestANewHeroicHomeStartsWithTheUpdateCheckOff(t *testing.T) {
	home := heroicHome(t)
	if err := seedHeroicSettings(home); err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(home.path, ".config", "heroic", "config.json")
	content, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	var document struct {
		Version         string         `json:"version"`
		DefaultSettings map[string]any `json:"defaultSettings"`
	}
	if err := json.Unmarshal(content, &document); err != nil {
		t.Fatalf("settings are not JSON: %v", err)
	}
	if document.Version != "v0" {
		t.Errorf("version = %q, and the pinned Heroic reads only v0", document.Version)
	}
	if value, present := document.DefaultSettings["checkForUpdatesOnStartup"]; !present || value != false {
		t.Errorf("checkForUpdatesOnStartup = %v, want false", value)
	}
	// Heroic blanks these three when a settings file leaves them out, so a
	// seed without them would cost the player their default Wine prefix.
	prefixes := home.path + "/Games/Heroic/Prefixes"
	for key, want := range map[string]string{
		"defaultWinePrefix": prefixes, "defaultWinePrefixDir": prefixes, "winePrefix": prefixes + "/shared",
	} {
		if document.DefaultSettings[key] != want {
			t.Errorf("%s = %v, want %q", key, document.DefaultSettings[key], want)
		}
	}
	// Anything more would be this worker choosing the player's settings.
	if len(document.DefaultSettings) != 4 {
		t.Errorf("seed carries %d settings, want exactly 4: %v", len(document.DefaultSettings), document.DefaultSettings)
	}
	for name, want := range map[string]os.FileMode{
		path: 0o600, filepath.Dir(path): 0o700, filepath.Dir(filepath.Dir(path)): 0o700,
	} {
		status, err := os.Lstat(name)
		if err != nil {
			t.Fatal(err)
		}
		if status.Mode().Perm() != want {
			t.Errorf("%s mode = %o, want %o", name, status.Mode().Perm(), want)
		}
	}
}

func TestHeroicSettingsThePlayerAlreadyHasAreNeverOpened(t *testing.T) {
	for name, existing := range map[string]string{
		"the player turned the check back on": `{"defaultSettings":{"checkForUpdatesOnStartup":true},"version":"v0"}`,
		"an empty file":                       "",
		"not JSON at all":                     "the player's own business",
	} {
		t.Run(name, func(t *testing.T) {
			home := heroicHome(t)
			directory := filepath.Join(home.path, ".config", "heroic")
			if err := os.MkdirAll(directory, 0o700); err != nil {
				t.Fatal(err)
			}
			path := filepath.Join(directory, "config.json")
			if err := os.WriteFile(path, []byte(existing), 0o644); err != nil {
				t.Fatal(err)
			}
			if err := seedHeroicSettings(home); err != nil {
				t.Fatalf("an existing settings file is not a failure: %v", err)
			}
			content, err := os.ReadFile(path)
			if err != nil {
				t.Fatal(err)
			}
			if string(content) != existing {
				t.Errorf("settings became %q", content)
			}
			if status, _ := os.Lstat(path); status.Mode().Perm() != 0o644 {
				t.Errorf("mode became %o", status.Mode().Perm())
			}
		})
	}
}

func TestTheHeroicSeedFollowsNoLink(t *testing.T) {
	t.Run("a linked settings file", func(t *testing.T) {
		home := heroicHome(t)
		directory := filepath.Join(home.path, ".config", "heroic")
		if err := os.MkdirAll(directory, 0o700); err != nil {
			t.Fatal(err)
		}
		elsewhere := filepath.Join(t.TempDir(), "elsewhere.json")
		if err := os.Symlink(elsewhere, filepath.Join(directory, "config.json")); err != nil {
			t.Fatal(err)
		}
		if err := seedHeroicSettings(home); err != nil {
			t.Fatalf("a link the player made is theirs to keep: %v", err)
		}
		if _, err := os.Lstat(elsewhere); !os.IsNotExist(err) {
			t.Error("the seed was written through the link")
		}
	})
	t.Run("a linked settings directory", func(t *testing.T) {
		home := heroicHome(t)
		elsewhere := t.TempDir()
		if err := os.Symlink(elsewhere, filepath.Join(home.path, ".config")); err != nil {
			t.Fatal(err)
		}
		if err := seedHeroicSettings(home); err == nil {
			t.Error("a linked .config was followed")
		}
		entries, err := os.ReadDir(elsewhere)
		if err != nil {
			t.Fatal(err)
		}
		if len(entries) != 0 {
			t.Errorf("the seed reached %v through the link", entries)
		}
	})
}

func TestTheHeroicSeedNeedsAHome(t *testing.T) {
	if err := seedHeroicSettings(nil); err == nil {
		t.Error("no home was accepted")
	}
}
