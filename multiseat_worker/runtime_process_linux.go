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
	"time"
)

const (
	maximumRuntimeProcessArguments   = 64
	maximumRuntimeProcessEnvironment = 64
	maximumRuntimeProcessValueBytes  = 4096
	maximumRuntimeReadyRecordBytes   = 64
)

type osRuntimeProcessHost struct{ diagnostics *os.File }

type osRuntimeProcessLease struct {
	process      *os.Process
	command      *exec.Cmd
	pidFD        *os.File
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

func (host osRuntimeProcessHost) Start(
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
	null, err := os.OpenFile("/dev/null", os.O_WRONLY|syscall.O_NOFOLLOW|syscall.O_CLOEXEC, 0)
	if err != nil {
		readyReader.Close()
		readyWriter.Close()
		return nil, err
	}
	defer null.Close()
	command.Stdout = null
	command.Stderr = null
	if host.diagnostics != nil {
		command.Stdout = host.diagnostics
		command.Stderr = host.diagnostics
	}
	pidFD := -1
	command.SysProcAttr = &syscall.SysProcAttr{
		Setpgid:   true,
		PidFD:     &pidFD,
		Pdeathsig: syscall.SIGKILL,
	}
	if err := command.Start(); err != nil {
		if pidFD >= 0 {
			syscall.Close(pidFD)
		}
		_ = readyReader.Close()
		_ = readyWriter.Close()
		return nil, errors.New("worker runtime helper could not be started")
	}
	_ = readyWriter.Close()
	defer readyReader.Close()
	if pidFD < 0 {
		_ = syscall.Kill(-command.Process.Pid, syscall.SIGKILL)
		_ = command.Wait()
		return nil, errors.New("runtime process lifetime unavailable")
	}
	lease := &osRuntimeProcessLease{
		process:      command.Process,
		command:      command,
		pidFD:        os.NewFile(uintptr(pidFD), "runtime-process-lifetime"),
		done:         make(chan error),
		stopComplete: make(chan struct{}),
	}
	go observeRuntimeProcessExit(pidFD, lease.done)
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
	// Caller cancellation does not abandon cleanup. No code reaps this leader
	// before this owner's final group signal, so a recycled PGID is impossible.
	var result error
	if err := signalRuntimeProcessGroup(lease.process, syscall.SIGTERM); err != nil {
		result = errors.New("worker runtime helper termination failed")
	}
	grace := time.Now().Add(2 * time.Second)
	if deadline, ok := parent.Deadline(); ok && deadline.Before(grace) {
		grace = deadline
	}
	for parent.Err() == nil && time.Now().Before(grace) {
		stopped, err := runtimeProcessGroupStopped(lease.process.Pid)
		if err != nil {
			result = errors.Join(result, err)
			break
		}
		if stopped && runtimeExitObserved(lease.done) {
			break
		}
		time.Sleep(10 * time.Millisecond)
	}
	if parent.Err() != nil {
		result = errors.Join(result, errors.New("worker runtime helper stop deadline exceeded"))
	}
	if err := signalRuntimeProcessGroup(lease.process, syscall.SIGKILL); err != nil {
		return errors.Join(result, errors.New("runtime group kill failed; outer worker teardown required"))
	}
	stopped, err := waitRuntimeGroup(lease, time.Now().Add(2*time.Second))
	if err != nil || !stopped {
		return errors.Join(result, err, errors.New("runtime group cleanup unproven; outer worker teardown required"))
	}
	// No numeric signal may occur after this point. Cmd owns only explicit
	// /dev/null output, so descendants cannot hold copier pipes across Wait.
	waitError := lease.command.Wait()
	_ = lease.pidFD.Close()
	var exitError *exec.ExitError
	if waitError != nil && !errors.As(waitError, &exitError) {
		result = errors.Join(result, errors.New("runtime leader could not be reaped"))
	}
	return result
}

func (lease *osRuntimeProcessLease) Stop(parent context.Context) error {
	if lease == nil || lease.process == nil || lease.command == nil || lease.pidFD == nil || lease.done == nil ||
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
