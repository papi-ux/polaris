//go:build linux

package main

import (
	"errors"
	"io"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"time"
	"unsafe"
)

// This observes exit without reaping. The original zombie keeps its PID/PGID
// reserved until the one cleanup owner has finished every group signal.
func observeRuntimeProcessExit(pidFD int, done chan error) {
	defer close(done)
	poll := struct {
		fd              int32
		events, revents int16
	}{fd: int32(pidFD), events: 1}
	for {
		_, _, errno := syscall.Syscall6(syscall.SYS_PPOLL, uintptr(unsafe.Pointer(&poll)), 1, 0, 0, 0, 0)
		if errno == syscall.EINTR {
			continue
		}
		return
	}
}

func runtimeProcessGroupStopped(group int) (bool, error) {
	root, err := os.Open("/proc")
	if err != nil {
		return false, err
	}
	defer root.Close()
	names, err := root.Readdirnames(8193)
	if (err != nil && err != io.EOF) || len(names) > 8192 {
		return false, errors.New("runtime process inventory exceeds bound")
	}
	for _, name := range names {
		pid, err := strconv.Atoi(name)
		if err != nil || pid <= 0 || strconv.Itoa(pid) != name {
			continue
		}
		file, err := os.Open(filepath.Join("/proc", name, "stat"))
		if errors.Is(err, os.ErrNotExist) {
			continue
		}
		if err != nil {
			return false, errors.New("runtime process inventory unavailable")
		}
		content, readError := io.ReadAll(io.LimitReader(file, 8193))
		file.Close()
		if errors.Is(readError, syscall.ESRCH) {
			continue
		}
		if readError != nil || len(content) > 8192 {
			return false, errors.New("runtime process record exceeds bound")
		}
		end := strings.LastIndex(string(content), ") ")
		if end < 0 {
			return false, errors.New("runtime process record invalid")
		}
		fields := strings.Fields(string(content[end+2:]))
		if len(fields) < 3 {
			return false, errors.New("runtime process record invalid")
		}
		if fields[2] != strconv.Itoa(group) {
			continue
		}
		if pid == group && (fields[0] == "Z" || fields[0] == "X") {
			continue
		}
		return false, nil
	}
	return true, nil
}

func runtimeExitObserved(done <-chan error) bool {
	select {
	case <-done:
		return true
	default:
		return false
	}
}

func waitRuntimeGroup(lease *osRuntimeProcessLease, deadline time.Time) (bool, error) {
	for {
		stopped, err := runtimeProcessGroupStopped(lease.process.Pid)
		if err != nil {
			return false, err
		}
		if stopped && runtimeExitObserved(lease.done) {
			return true, nil
		}
		if !time.Now().Before(deadline) {
			return false, nil
		}
		time.Sleep(10 * time.Millisecond)
	}
}
