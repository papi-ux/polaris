//go:build linux

package main

import (
	"bufio"
	"context"
	"errors"
	"io"
	"os"
	"os/exec"
	"strconv"
	"strings"
	"sync"
	"syscall"
)

const (
	maximumRuntimeProcessArguments   = 64
	maximumRuntimeProcessEnvironment = 64
	maximumRuntimeProcessValueBytes  = 4096
	maximumRuntimeReadyRecordBytes   = 64
)

type osRuntimeProcessHost struct{}

type osRuntimeProcessLease struct {
	process      *os.Process
	done         chan error
	stopOnce     sync.Once
	stopComplete chan struct{}
	stopError    error
}

func validRuntimeProcessValue(value string, allowEmpty bool) bool {
	if (!allowEmpty && value == "") || len(value) > maximumRuntimeProcessValueBytes {
		return false
	}
	for _, character := range []byte(value) {
		if character < 0x20 || character > 0x7e {
			return false
		}
	}
	return true
}

func validRuntimeEnvironmentName(name string) bool {
	if name == "" || len(name) > 128 {
		return false
	}
	for index, character := range []byte(name) {
		if (character >= 'A' && character <= 'Z') || character == '_' ||
			(index > 0 && character >= '0' && character <= '9') {
			continue
		}
		return false
	}
	return true
}

func validateRuntimeProcessSpec(spec runtimeProcessSpec) error {
	if !validRuntimeStage(spec.Stage) ||
		!validRuntimeHelperPath(spec.Executable) ||
		len(spec.Arguments) < 2 ||
		len(spec.Arguments) > maximumRuntimeProcessArguments ||
		len(spec.Environment) > maximumRuntimeProcessEnvironment ||
		spec.Arguments[0] != "serve" ||
		spec.Arguments[1] != "--stage="+spec.Stage.String() {
		return errors.New("worker runtime process specification is invalid")
	}
	for _, argument := range spec.Arguments {
		if !validRuntimeProcessValue(argument, true) {
			return errors.New("worker runtime process argument is invalid")
		}
	}
	seen := make(map[string]struct{}, len(spec.Environment))
	for _, setting := range spec.Environment {
		name, _, present := strings.Cut(setting, "=")
		if !present || !validRuntimeEnvironmentName(name) ||
			!validRuntimeProcessValue(setting, false) ||
			name == runtimeReadyFDSetting {
			return errors.New("worker runtime process environment is invalid")
		}
		if _, duplicate := seen[name]; duplicate {
			return errors.New("worker runtime process environment is invalid")
		}
		seen[name] = struct{}{}
	}
	return nil
}

func executableRuntimeHelper(path string) bool {
	info, err := os.Stat(path)
	return err == nil && info.Mode().IsRegular() && info.Mode().Perm()&0o111 != 0
}

func readRuntimeReadyRecord(reader io.Reader) error {
	buffered := bufio.NewReader(io.LimitReader(reader, maximumRuntimeReadyRecordBytes+1))
	record, err := buffered.ReadString('\n')
	if err != nil || record != runtimeReadyRecord {
		return errors.New("worker runtime helper readiness record is invalid")
	}
	return nil
}

func (osRuntimeProcessHost) Start(
	parent context.Context,
	spec runtimeProcessSpec,
) (runtimeLease, error) {
	if parent == nil {
		return nil, errors.New("worker runtime process context is missing")
	}
	if err := validateRuntimeProcessSpec(spec); err != nil {
		return nil, err
	}
	if !executableRuntimeHelper(spec.Executable) {
		return nil, errors.New("worker runtime helper is unavailable")
	}
	readyReader, readyWriter, err := os.Pipe()
	if err != nil {
		return nil, errors.New("worker runtime readiness pipe could not be created")
	}
	command := exec.Command(spec.Executable, spec.Arguments...)
	command.Env = append(
		append([]string(nil), spec.Environment...),
		runtimeReadyFDSetting+"="+strconv.Itoa(3),
	)
	command.ExtraFiles = []*os.File{readyWriter}
	command.Stdin = nil
	command.Stdout = io.Discard
	command.Stderr = io.Discard
	command.SysProcAttr = &syscall.SysProcAttr{
		Setpgid:   true,
		Pdeathsig: syscall.SIGKILL,
	}
	if err := command.Start(); err != nil {
		_ = readyReader.Close()
		_ = readyWriter.Close()
		return nil, errors.New("worker runtime helper could not be started")
	}
	_ = readyWriter.Close()
	lease := &osRuntimeProcessLease{
		process:      command.Process,
		done:         make(chan error),
		stopComplete: make(chan struct{}),
	}
	go func() {
		_ = command.Wait()
		close(lease.done)
	}()
	readiness := make(chan error, 1)
	go func() {
		defer readyReader.Close()
		readiness <- readRuntimeReadyRecord(readyReader)
	}()
	select {
	case err := <-readiness:
		if err != nil {
			return lease, err
		}
		return lease, nil
	case <-lease.done:
		return lease, errors.New("worker runtime helper exited before readiness")
	case <-parent.Done():
		return lease, errors.New("worker runtime helper readiness timed out")
	}
}

func (lease *osRuntimeProcessLease) Done() <-chan error {
	if lease == nil {
		return nil
	}
	return lease.done
}

func signalRuntimeProcessGroup(process *os.Process, signal syscall.Signal) error {
	if process == nil || process.Pid <= 1 {
		return errors.New("worker runtime helper process is invalid")
	}
	err := syscall.Kill(-process.Pid, signal)
	if errors.Is(err, syscall.ESRCH) {
		return nil
	}
	return err
}

func (lease *osRuntimeProcessLease) stop(parent context.Context) error {
	select {
	case <-lease.done:
		return nil
	default:
	}
	if err := signalRuntimeProcessGroup(lease.process, syscall.SIGTERM); err != nil {
		return errors.New("worker runtime helper termination failed")
	}
	select {
	case <-lease.done:
		return nil
	case <-parent.Done():
		_ = signalRuntimeProcessGroup(lease.process, syscall.SIGKILL)
		return errors.New("worker runtime helper stop deadline exceeded")
	}
}

func (lease *osRuntimeProcessLease) Stop(parent context.Context) error {
	if lease == nil || lease.process == nil || lease.done == nil ||
		lease.stopComplete == nil {
		return errors.New("worker runtime helper lease is invalid")
	}
	if parent == nil {
		return errors.New("worker runtime process context is missing")
	}
	lease.stopOnce.Do(func() {
		go func() {
			lease.stopError = lease.stop(parent)
			close(lease.stopComplete)
		}()
	})
	select {
	case <-lease.stopComplete:
		return lease.stopError
	case <-parent.Done():
		return errors.New("worker runtime helper stop deadline exceeded")
	}
}
