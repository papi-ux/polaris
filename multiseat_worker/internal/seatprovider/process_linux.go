//go:build linux

package seatprovider

import (
	"bytes"
	"errors"
	"io"
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
	command.Stdout = io.Discard
	command.Stderr = io.Discard
	command.SysProcAttr = &syscall.SysProcAttr{Pdeathsig: syscall.SIGKILL}
	return command, executable, nil
}

type managedChild struct {
	command *exec.Cmd
	done    chan struct{}
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
	if err := command.Start(); err != nil {
		_ = executable.Close()
		return nil, errors.New("runtime provider child could not be started")
	}
	_ = executable.Close()
	child := &managedChild{command: command, done: make(chan struct{})}
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
	childUmaskLock.Lock()
	previousUmask := syscall.Umask(umask)
	startError := command.Start()
	syscall.Umask(previousUmask)
	childUmaskLock.Unlock()
	if startError != nil {
		_ = executable.Close()
		return nil, errors.New("runtime provider child could not be started")
	}
	_ = executable.Close()
	child := &managedChild{command: command, done: make(chan struct{})}
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

func (child *managedChild) stop(timeout time.Duration) error {
	if child == nil || child.command == nil || child.command.Process == nil ||
		child.done == nil || timeout <= 0 {
		return errors.New("runtime provider child is invalid")
	}
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
