//go:build linux

package seatprovider

import (
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"time"
)

const maximumWorkloadChildren = 256

// Called only by the dedicated launcher process. Adopted game descendants,
// including helpers that call setsid, remain owned by this provider on exit.
func enableWorkloadSubreaper() error {
	_, _, errno := syscall.Syscall6(syscall.SYS_PRCTL, 36, 1, 0, 0, 0, 0)
	if errno != 0 {
		return errors.New("workload descendant ownership unavailable")
	}
	return nil
}

func boundedProcessText(path string) (string, error) {
	file, err := os.Open(path)
	if err != nil {
		return "", err
	}
	defer file.Close()
	content, err := io.ReadAll(io.LimitReader(file, 16385))
	if err != nil || len(content) > 16384 {
		return "", errors.New("workload process record exceeds bound")
	}
	return string(content), nil
}

func ownWorkloadChildren() ([]int, error) {
	root, err := os.Open("/proc/self/task")
	if err != nil {
		return nil, err
	}
	defer root.Close()
	threads, err := root.Readdirnames(1025)
	if (err != nil && err != io.EOF) || len(threads) > 1024 {
		return nil, errors.New("workload thread inventory exceeds bound")
	}
	seen := map[int]bool{}
	for _, thread := range threads {
		content, err := boundedProcessText(filepath.Join("/proc/self/task", thread, "children"))
		if errors.Is(err, os.ErrNotExist) {
			continue
		}
		if err != nil {
			return nil, err
		}
		for _, word := range strings.Fields(content) {
			pid, err := strconv.Atoi(word)
			if err != nil || pid <= 1 || strconv.Itoa(pid) != word {
				return nil, errors.New("invalid workload child")
			}
			seen[pid] = true
			if len(seen) > maximumWorkloadChildren {
				return nil, errors.New("workload descendant inventory exceeds bound")
			}
		}
	}
	children := make([]int, 0, len(seen))
	for pid := range seen {
		children = append(children, pid)
	}
	return children, nil
}

func isOwnWorkloadChild(pid int) bool {
	content, err := boundedProcessText(fmt.Sprintf("/proc/%d/stat", pid))
	if err != nil {
		return false
	}
	end := strings.LastIndex(content, ") ")
	if end < 0 {
		return false
	}
	fields := strings.Fields(content[end+2:])
	return len(fields) > 1 && fields[1] == strconv.Itoa(os.Getpid())
}

func signalOwnedWorkloadChild(pid int, signal syscall.Signal) error {
	if !isOwnWorkloadChild(pid) {
		return nil
	} // exited children are reaped below
	fd, _, errno := syscall.Syscall(434 /* pidfd_open */, uintptr(pid), 0, 0)
	if errno == syscall.ESRCH {
		return nil
	}
	if errno != 0 {
		return errors.New("workload child lifetime unavailable")
	}
	defer syscall.Close(int(fd))
	if !isOwnWorkloadChild(pid) {
		return errors.New("workload child ownership changed")
	}
	_, _, errno = syscall.Syscall6(424 /* pidfd_send_signal */, fd, uintptr(signal), 0, 0, 0, 0)
	if errno != 0 && errno != syscall.ESRCH {
		return errors.New("workload child signal failed")
	}
	return nil
}

// The primary command's Wait must finish before this function runs. There are
// no concurrent probe children in the dedicated launcher at this point.
func stopWorkloadChildren(timeout time.Duration) error {
	deadline := time.Now().Add(timeout)
	killAfter := time.Now().Add(250 * time.Millisecond)
	for time.Now().Before(deadline) {
		children, err := ownWorkloadChildren()
		if err != nil {
			return err
		}
		signal := syscall.SIGTERM
		if !time.Now().Before(killAfter) {
			signal = syscall.SIGKILL
		}
		for _, pid := range children {
			if err := signalOwnedWorkloadChild(pid, signal); err != nil {
				return err
			}
		}
		for {
			var status syscall.WaitStatus
			pid, err := syscall.Wait4(-1, &status, syscall.WNOHANG, nil)
			if err == syscall.EINTR {
				continue
			}
			if err == syscall.ECHILD {
				return nil
			}
			if err != nil {
				return errors.New("workload descendants could not be reaped")
			}
			if pid == 0 {
				break
			}
		}
		time.Sleep(10 * time.Millisecond)
	}
	return errors.New("workload descendants did not stop before deadline")
}

func stopWorkload(primary *managedChild, timeout time.Duration) error {
	deadline := time.Now().Add(timeout)
	if err := primary.stop(500 * time.Millisecond); err != nil {
		return err
	}
	remaining := time.Until(deadline)
	if remaining <= 0 {
		return errors.New("workload stop deadline expired")
	}
	return stopWorkloadChildren(remaining)
}
