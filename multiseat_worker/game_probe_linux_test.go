//go:build linux

package main

import (
	"errors"
	"os"
	"path/filepath"
	"strings"
	"syscall"
	"testing"
)

func TestPhysicalGameProbeRejectsUnsafeTokensAndIncompleteObservations(t *testing.T) {
	for _, token := range []string{"", strings.Repeat("a", 31), strings.Repeat("a", 33), "../" + strings.Repeat("a", 29), strings.Repeat("A", 32), strings.Repeat("a", 32) + "\n"} {
		if _, err := gameProbePath(token); err == nil {
			t.Fatalf("accepted token %q", token)
		}
	}
	if _, err := gameProbePath(strings.Repeat("a", 32)); err != nil {
		t.Fatal(err)
	}
	for _, content := range []string{
		`{}`, `{"keyboard":0,"pointer":0,"gamepad":0,"frames":1}`, `{"keyboard":0,"pointer":0,"gamepad":0,"frames":1,"pid":null}`,
		`{"keyboard":0,"pointer":0,"gamepad":0,"frames":0,"pid":42}`, `{"keyboard":0,"pointer":0,"gamepad":0,"frames":1,"pid":1}`,
		`{"keyboard":-1,"pointer":0,"gamepad":0,"frames":1,"pid":42}`, `{"keyboard":4294967296,"pointer":0,"gamepad":0,"frames":1,"pid":42}`,
		`{"keyboard":0,"pointer":0,"gamepad":0,"frames":1,"pid":42,"other":0}`, `{"keyboard":0,"pointer":0,"gamepad":0,"frames":1,"pid":42} {}`,
		strings.Repeat(" ", 1025),
	} {
		if _, err := parseGameObservation([]byte(content)); err == nil {
			t.Fatalf("accepted observation %q", content)
		}
	}
	if got, err := parseGameObservation([]byte(`{"keyboard":0,"pointer":1,"gamepad":2,"frames":3,"pid":42}`)); err != nil || got.Pointer != 1 || got.PID != 42 {
		t.Fatalf("valid observation: %+v %v", got, err)
	}
}

func TestPhysicalProbeSignalsAreExclusiveBoundedAndInodeOwned(t *testing.T) {
	path := filepath.Join(t.TempDir(), "ready")
	file, identity, err := createGameProbeSignal(path)
	if err != nil {
		t.Fatal(err)
	}
	defer file.Close()
	if err := gameProbeSignal(path); err != nil {
		t.Fatal(err)
	}
	if duplicate, _, err := createGameProbeSignal(path); err == nil {
		duplicate.Close()
		t.Fatal("duplicate probe claimed resources")
	}
	if err := os.Rename(path, path+".old"); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, []byte(gameProbeRecord), 0600); err != nil {
		t.Fatal(err)
	}
	if err := removeGameProbeSignal(path, identity); err == nil {
		t.Fatal("replaced marker cleanup reported success")
	}
	if _, err := os.Stat(path); err != nil {
		t.Fatalf("removed replacement: %v", err)
	}
	if err := os.Chmod(path, 0644); err != nil {
		t.Fatal(err)
	}
	if err := gameProbeSignal(path); err == nil {
		t.Fatal("accepted nonprivate signal")
	}
	if err := os.Remove(path); err != nil {
		t.Fatal(err)
	}
	if err := os.Symlink(path+".old", path); err != nil {
		t.Fatal(err)
	}
	if err := gameProbeSignal(path); err == nil {
		t.Fatal("followed signal symlink")
	}
	os.Remove(path)
	if err := syscall.Mkfifo(path, 0600); err != nil {
		t.Fatal(err)
	}
	if err := gameProbeSignal(path); err == nil {
		t.Fatal("accepted or blocked on FIFO")
	}
	os.Remove(path)
	if err := gameProbeSignal(path); !errors.Is(err, os.ErrNotExist) {
		t.Fatalf("absence result: %v", err)
	}
}

func TestPhysicalProbeReportsMarkerRemovalFailure(t *testing.T) {
	path := filepath.Join(t.TempDir(), "ready")
	file, identity, err := createGameProbeSignal(path)
	if err != nil {
		t.Fatal(err)
	}
	defer file.Close()
	if err := removeGameProbeSignalWith(path, identity, func(string) error { return syscall.EACCES }); err == nil {
		t.Fatal("removal failure discarded")
	}
	if err := gameProbeSignal(path); err != nil {
		t.Fatalf("failed removal did not retain original: %v", err)
	}
	if err := removeGameProbeSignal(path, identity); err != nil {
		t.Fatal(err)
	}
}
