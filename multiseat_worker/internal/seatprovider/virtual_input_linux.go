//go:build linux

package seatprovider

import (
	"context"
	"errors"
	"io"
	"time"

	"github.com/papi-ux/polaris/multiseat_worker/internal/seatinput"
	"github.com/papi-ux/polaris/multiseat_worker/internal/seatruntime"
)

// RunVirtualInput retains and monitors the allocated evdev aliases. The display
// provider consumes keyboard/pointer devices; workloads consume gamepad devices.
// Neither provider receives the host's injection or device-creation authority.
func RunVirtualInput(arguments, environment []string) error {
	request, err := parseProviderInvocation(seatruntime.StageVirtualInput, arguments, environment)
	if err != nil {
		return err
	}
	ready, err := openReadinessWriter()
	if err != nil {
		return err
	}
	parent, cancel := signalContext()
	defer cancel()
	return runVirtualInput(parent, request, ready, seatinput.Directory)
}

func runVirtualInput(parent context.Context, request seatruntime.Request, ready io.WriteCloser, directory string) error {
	if ready == nil || parent == nil || request.Stage != seatruntime.StageVirtualInput {
		if ready != nil {
			_ = ready.Close()
		}
		return errors.New("invalid virtual input provider")
	}
	defer ready.Close()
	if _, err := seatruntime.Arguments(request); err != nil {
		return err
	}
	inputs, err := seatinput.Open(directory, request.InputSeat)
	if err != nil {
		return err
	}
	defer inputs.Close()
	if err := parent.Err(); err != nil {
		return err
	}
	if err := publishReadiness(ready); err != nil {
		return err
	}
	ticker := time.NewTicker(time.Second)
	defer ticker.Stop()
	for {
		select {
		case <-parent.Done():
			return nil
		case <-ticker.C:
			if err := inputs.Verify(); err != nil {
				return err
			}
		}
	}
}
