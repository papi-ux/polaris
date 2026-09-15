//go:build linux

package seatprovider

import (
	"errors"
	"fmt"
	"os"
	"syscall"
)

// Only the numeric termination status accompanies the fixed provider message.
// Child output, commands, paths and environment are not diagnostic context.
func childExitError(message string, state *os.ProcessState) error {
	if state != nil {
		if status, ok := state.Sys().(syscall.WaitStatus); ok && status.Signaled() {
			return fmt.Errorf("%s (signal %d)", message, status.Signal())
		}
		if code := state.ExitCode(); code >= 0 {
			return fmt.Errorf("%s (exit status %d)", message, code)
		}
	}
	return errors.New(message)
}
