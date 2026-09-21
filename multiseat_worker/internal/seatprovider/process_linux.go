//go:build linux

package seatprovider

import (
	"bytes"
	"context"
	"errors"
	"os"
	"os/exec"
	"runtime"
	"sync"
	"syscall"
	"time"
)

var childUmaskLock sync.Mutex

func openTrustedExecutable(path string, expectedOwnerUID uint32) (*os.File, error) {
	if !validAbsolutePath(path) {
		return nil, errors.New("runtime provider executable path is invalid")
	}
	descriptor, err := syscall.Open(
		path,
		syscall.O_RDONLY|syscall.O_NOFOLLOW|syscall.O_NONBLOCK|syscall.O_CLOEXEC,
		0,
	)
	if err != nil {
		return nil, errors.New("runtime provider executable is unavailable")
	}
	closeDescriptor := true
	defer func() {
		if closeDescriptor {
			_ = syscall.Close(descriptor)
		}
	}()
	var status syscall.Stat_t
	if err := syscall.Fstat(descriptor, &status); err != nil ||
		status.Mode&syscall.S_IFMT != syscall.S_IFREG ||
		status.Uid != expectedOwnerUID || status.Mode&0o022 != 0 ||
		status.Mode&0o111 == 0 ||
		status.Mode&(syscall.S_ISUID|syscall.S_ISGID) != 0 {
		return nil, errors.New("runtime provider executable ownership or mode is invalid")
	}
	file := os.NewFile(uintptr(descriptor), "trusted-runtime-executable")
	if file == nil {
		return nil, errors.New("runtime provider executable is unavailable")
	}
	closeDescriptor = false
	return file, nil
}

func trustedCommand(
	path string,
	expectedOwnerUID uint32,
	arguments []string,
	environment []string,
	extraFiles []*os.File,
) (*exec.Cmd, *os.File, error) {
	executable, err := openTrustedExecutable(path, expectedOwnerUID)
	if err != nil {
		return nil, nil, err
	}
	command := exec.Command("/proc/self/fd/3", arguments...)
	command.Args[0] = path
	command.Env = append([]string(nil), environment...)
	command.ExtraFiles = make([]*os.File, 0, len(extraFiles)+1)
	command.ExtraFiles = append(command.ExtraFiles, executable)
	command.ExtraFiles = append(command.ExtraFiles, extraFiles...)
	command.Stdin = nil
	// Explicit files avoid Cmd.Wait copier pipes being held by descendants.
	// The outer supervisor chooses a private diagnostic sink or /dev/null.
	command.Stdout = os.Stdout
	command.Stderr = os.Stderr
	command.SysProcAttr = &syscall.SysProcAttr{Pdeathsig: syscall.SIGKILL}
	return command, executable, nil
}

type managedChild struct {
	command      *exec.Cmd
	done         chan struct{}
	pidFD        *os.File
	closePIDOnce sync.Once
}

func startManagedChild(
	path string,
	expectedOwnerUID uint32,
	arguments []string,
	environment []string,
	extraFiles []*os.File,
) (*managedChild, error) {
	command, executable, err := trustedCommand(
		path,
		expectedOwnerUID,
		arguments,
		environment,
		extraFiles,
	)
	if err != nil {
		return nil, err
	}
	pidFD := -1
	command.SysProcAttr.PidFD = &pidFD
	if err := command.Start(); err != nil {
		_ = executable.Close()
		if pidFD >= 0 {
			_ = syscall.Close(pidFD)
		}
		return nil, errors.New("runtime provider child could not be started")
	}
	_ = executable.Close()
	child := &managedChild{command: command, done: make(chan struct{}), pidFD: os.NewFile(uintptr(pidFD), "provider-child-lifetime")}
	go func() {
		_ = command.Wait()
		close(child.done)
	}()
	return child, nil
}

func startManagedChildWithUmask(
	path string,
	expectedOwnerUID uint32,
	arguments []string,
	environment []string,
	extraFiles []*os.File,
	umask int,
) (*managedChild, error) {
	if umask < 0 || umask > 0o777 {
		return nil, errors.New("runtime provider child umask is invalid")
	}
	command, executable, err := trustedCommand(
		path,
		expectedOwnerUID,
		arguments,
		environment,
		extraFiles,
	)
	if err != nil {
		return nil, err
	}
	// The display provider is a dedicated process, but tests can exercise two
	// providers concurrently. Serialize the process-global umask only across
	// the fork so every socket the child later creates is owner-only.
	pidFD := -1
	command.SysProcAttr.PidFD = &pidFD
	childUmaskLock.Lock()
	previousUmask := syscall.Umask(umask)
	startError := command.Start()
	syscall.Umask(previousUmask)
	childUmaskLock.Unlock()
	if startError != nil {
		_ = executable.Close()
		if pidFD >= 0 {
			_ = syscall.Close(pidFD)
		}
		return nil, errors.New("runtime provider child could not be started")
	}
	_ = executable.Close()
	child := &managedChild{command: command, done: make(chan struct{}), pidFD: os.NewFile(uintptr(pidFD), "provider-child-lifetime")}
	go func() {
		_ = command.Wait()
		close(child.done)
	}()
	return child, nil
}

func (child *managedChild) exited() bool {
	if child == nil || child.done == nil {
		return true
	}
	select {
	case <-child.done:
		return true
	default:
		return false
	}
}

// stopNoticeGrace is how long a helper waits for its own stop signal before it
// calls a child's exit unexpected. A variable so a test need not sit through it.
var stopNoticeGrace = 250 * time.Millisecond

// stopRequested reports whether a child that has just exited went down with
// its helper rather than on its own.
//
// The worker stops a helper by signalling the helper's whole process group, so
// the child gets the same SIGTERM in the same instant, and a small daemon acts
// on it sooner than this process hears of it: the runtime has to hand the
// signal to a goroutine before the context is cancelled. Without this wait the
// session bus reported "exited unexpectedly (exit status 0)" on every clean
// disconnect, the worker logged a failed stop, and a real failure had nothing
// to tell it apart from the noise around it. A child that dies on its own is
// still reported, a quarter of a second later.
func stopRequested(parent context.Context) bool {
	if parent.Err() != nil {
		return true
	}
	timer := time.NewTimer(stopNoticeGrace)
	defer timer.Stop()
	select {
	case <-parent.Done():
		return true
	case <-timer.C:
		return false
	}
}

func (child *managedChild) exitError(message string) error {
	if child == nil || child.command == nil || child.done == nil || !child.exited() {
		return errors.New(message)
	}
	// Wait publishes ProcessState before closing done. Never read it while
	// the wait goroutine could still be updating the command.
	return childExitError(message, child.command.ProcessState)
}

func (child *managedChild) stop(timeout time.Duration) error {
	if child == nil || child.command == nil || child.command.Process == nil ||
		child.done == nil || timeout <= 0 {
		return errors.New("runtime provider child is invalid")
	}
	defer child.closePIDOnce.Do(func() {
		if child.pidFD != nil {
			_ = child.pidFD.Close()
		}
	})
	select {
	case <-child.done:
		return nil
	default:
	}
	if err := child.command.Process.Signal(syscall.SIGTERM); err != nil &&
		!errors.Is(err, os.ErrProcessDone) && !errors.Is(err, syscall.ESRCH) {
		return errors.New("runtime provider child termination failed")
	}
	timer := time.NewTimer(timeout)
	defer timer.Stop()
	select {
	case <-child.done:
		return nil
	case <-timer.C:
	}
	if err := child.command.Process.Kill(); err != nil &&
		!errors.Is(err, os.ErrProcessDone) && !errors.Is(err, syscall.ESRCH) {
		return errors.New("runtime provider child kill failed")
	}
	timer.Reset(timeout)
	select {
	case <-child.done:
		return nil
	case <-timer.C:
		return errors.New("runtime provider child did not exit")
	}
}

// stopChecked preserves bounded termination but reports a child's failed cleanup.
// A successful exit and the requested TERM/KILL signals are expected; a crash or
// nonzero exit during a requested stop must not become a successful teardown.
func (child *managedChild) stopChecked(timeout time.Duration, message string) error {
	if err := child.stop(timeout); err != nil {
		return err
	}
	state := child.command.ProcessState
	if state != nil && state.Success() {
		return nil
	}
	if state != nil {
		if status, ok := state.Sys().(syscall.WaitStatus); ok && status.Signaled() &&
			(status.Signal() == syscall.SIGTERM || status.Signal() == syscall.SIGKILL) {
			return nil
		}
	}
	return child.exitError(message)
}

// Exhausting a shared wait budget must never skip the termination request for
// a later owned child. Process.Kill retains exec's process identity; the caller
// must preserve artifacts unless the wait goroutine has also proved exit.
func (child *managedChild) killWithoutWaiting() error {
	if child == nil || child.command == nil || child.command.Process == nil || child.done == nil {
		return errors.New("runtime provider child is invalid")
	}
	defer child.closePIDOnce.Do(func() {
		if child.pidFD != nil {
			_ = child.pidFD.Close()
		}
	})
	if child.exited() {
		return nil
	}
	if err := child.command.Process.Kill(); err != nil && !errors.Is(err, os.ErrProcessDone) && !errors.Is(err, syscall.ESRCH) {
		return errors.New("runtime provider child kill failed")
	}
	if child.exited() {
		return nil
	}
	return errors.New("runtime provider child exit is unproven after its wait budget expired")
}

type boundedOutput struct {
	buffer  bytes.Buffer
	maximum int
}

func (output *boundedOutput) Write(content []byte) (int, error) {
	if output == nil || output.maximum <= 0 ||
		len(content) > output.maximum-output.buffer.Len() {
		return 0, errors.New("runtime provider child output exceeded its bound")
	}
	return output.buffer.Write(content)
}

func runTrustedCommand(
	path string,
	expectedOwnerUID uint32,
	arguments []string,
	environment []string,
	timeout time.Duration,
) ([]byte, error) {
	if timeout <= 0 {
		return nil, errors.New("runtime provider child timeout is invalid")
	}
	command, executable, err := trustedCommand(
		path,
		expectedOwnerUID,
		arguments,
		environment,
		nil,
	)
	if err != nil {
		return nil, err
	}
	output := &boundedOutput{maximum: maximumProviderOutput}
	command.Stdout = output
	if err := command.Start(); err != nil {
		_ = executable.Close()
		return nil, errors.New("runtime provider probe could not be started")
	}
	_ = executable.Close()
	done := make(chan error, 1)
	go func() { done <- command.Wait() }()
	timer := time.NewTimer(timeout)
	defer timer.Stop()
	select {
	case err := <-done:
		runtime.KeepAlive(output)
		if err != nil {
			return nil, errors.New("runtime provider probe failed")
		}
		return append([]byte(nil), output.buffer.Bytes()...), nil
	case <-timer.C:
		_ = command.Process.Kill()
		<-done
		return nil, errors.New("runtime provider probe timed out")
	}
}
