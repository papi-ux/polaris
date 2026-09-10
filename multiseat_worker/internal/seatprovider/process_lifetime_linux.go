//go:build linux

package seatprovider

import (
	"errors"
	"fmt"
	"os"
	"syscall"
	"unsafe"
)

type processCookie struct{ device, inode uint64 }
type processLifetime struct {
	directory, pidFD *os.File
	cookie           processCookie
}

func (life *processLifetime) close() {
	if life != nil {
		_ = life.directory.Close()
		_ = life.pidFD.Close()
	}
}

func (life *processLifetime) verify() error {
	if life == nil || life.pidFD == nil {
		return errors.New("process lifetime is unavailable")
	}
	return verifyProcessDescriptor(life.pidFD.Fd(), pollProcessDescriptor)
}

func pollProcessDescriptor(fd int32) (uintptr, int16, syscall.Errno) {
	request := struct {
		fd              int32
		events, revents int16
	}{fd: fd, events: 1}
	timeout := syscall.Timespec{}
	count, _, errno := syscall.Syscall6(syscall.SYS_PPOLL, uintptr(unsafe.Pointer(&request)), 1, uintptr(unsafe.Pointer(&timeout)), 0, 0, 0)
	return count, request.revents, errno
}

func verifyProcessDescriptor(fd uintptr, poll func(int32) (uintptr, int16, syscall.Errno)) error {
	// os.File.Fd returns an invalid sentinel after Close. A negative pollfd is
	// ignored by poll, so truncating that sentinel could falsely report life.
	if fd > 0x7fffffff {
		return errors.New("process lifetime descriptor is unavailable")
	}
	for {
		// Even a zero-time ppoll can be interrupted by a signal (including Go
		// async preemption). Retry the same retained identity with a fresh request.
		count, revents, errno := poll(int32(fd))
		if errno == syscall.EINTR {
			continue
		}
		if errno != 0 {
			return fmt.Errorf("process lifetime query failed: %w", errno)
		}
		if count != 0 || revents != 0 {
			return errors.New("process lifetime has retired")
		}
		return nil
	}
}

// The producer retains the original /proc inode until its published metadata
// has been removed. A replacement process, even with the same numeric PID,
// cannot have that still-pinned inode. The pidfd brackets capture and use.
func retainProcessLifetime(pid, originalFD int, expected processCookie) (_ *processLifetime, result error) {
	if pid <= 1 {
		return nil, errors.New("invalid process identity")
	}
	var fd uintptr
	var errno syscall.Errno
	if originalFD >= 0 {
		fd, _, errno = syscall.Syscall(syscall.SYS_FCNTL, uintptr(originalFD), syscall.F_DUPFD_CLOEXEC, 0)
	} else {
		fd, _, errno = syscall.Syscall(434 /* pidfd_open */, uintptr(pid), 0, 0)
	}
	if errno != 0 {
		return nil, errors.New("process lifetime cannot be retained")
	}
	life := &processLifetime{pidFD: os.NewFile(fd, "process-lifetime")}
	defer func() {
		if result != nil {
			life.close()
		}
	}()
	if err := life.verify(); err != nil {
		return nil, err
	}
	directory, err := syscall.Open(fmt.Sprintf("/proc/%d", pid), syscall.O_RDONLY|syscall.O_DIRECTORY|syscall.O_NOFOLLOW|syscall.O_CLOEXEC, 0)
	if err != nil {
		return nil, errors.New("process identity cannot be retained")
	}
	life.directory = os.NewFile(uintptr(directory), "process-identity")
	var status syscall.Stat_t
	if err := syscall.Fstat(directory, &status); err != nil {
		return nil, err
	}
	life.cookie = processCookie{device: uint64(status.Dev), inode: status.Ino}
	if life.cookie.inode == 0 || (expected != (processCookie{}) && life.cookie != expected) {
		return nil, errors.New("process lifetime identity changed")
	}
	if err := life.verify(); err != nil {
		return nil, err
	}
	return life, nil
}
