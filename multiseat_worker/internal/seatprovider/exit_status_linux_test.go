//go:build linux

package seatprovider

import (
	"os/exec"
	"testing"
)

func TestChildExitErrorRetainsOnlyStatus(t *testing.T) {
	const message = "runtime display exited unexpectedly"
	for _, entry := range []struct {
		name   string
		script string
		want   string
	}{
		{"zero", "exit 0", " (exit status 0)"},
		{"failure", "exit 23", " (exit status 23)"},
		{"signal", "kill -TERM $$", " (signal 15)"},
	} {
		t.Run(entry.name, func(t *testing.T) {
			command := exec.Command("/bin/sh", "-c", entry.script, "private-child-argument")
			command.Env = []string{"PRIVATE_CHILD_VALUE=must-not-appear"}
			_ = command.Run()
			if command.ProcessState == nil {
				t.Fatal("child did not provide an exit status")
			}
			if got := childExitError(message, command.ProcessState).Error(); got != message+entry.want {
				t.Fatalf("unexpected child diagnostic: %q", got)
			}
		})
	}
	if got := childExitError(message, nil).Error(); got != message {
		t.Fatalf("missing process state invented a termination status: %q", got)
	}
}
