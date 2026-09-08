//go:build linux

package seatprovider

import (
	"os"
	"os/exec"
	"syscall"
	"testing"
)

func TestProcessCookiePinsOriginalLifetimeAndRejectsRetirement(t *testing.T) {
	pidFD := -1
	child := exec.Command("/bin/sleep", "30")
	child.SysProcAttr = &syscall.SysProcAttr{PidFD: &pidFD}
	if err := child.Start(); err != nil {
		t.Fatal(err)
	}
	defer func() { _ = child.Process.Kill(); _ = child.Wait(); _ = syscall.Close(pidFD) }()
	life, err := retainProcessLifetime(child.Process.Pid, pidFD, processCookie{})
	if err != nil {
		t.Fatal(err)
	}
	defer life.close()
	launcher, err := retainProcessLifetime(child.Process.Pid, -1, life.cookie)
	if err != nil {
		t.Fatal(err)
	}
	defer launcher.close()
	wrong := life.cookie
	wrong.inode++
	if other, err := retainProcessLifetime(child.Process.Pid, -1, wrong); err == nil {
		other.close()
		t.Fatal("stale cookie accepted")
	}
	if other, err := retainProcessLifetime(os.Getpid(), -1, life.cookie); err == nil {
		other.close()
		t.Fatal("different live process accepted")
	}
	if err := child.Process.Kill(); err != nil {
		t.Fatal(err)
	}
	_ = child.Wait()
	if life.verify() == nil || launcher.verify() == nil {
		t.Fatal("dead compositor remained usable")
	}
	if other, err := retainProcessLifetime(child.Process.Pid, pidFD, life.cookie); err == nil {
		other.close()
		t.Fatal("dead process was reacquired")
	}
}

func TestProcessLifetimeRetriesInterruptedPollOfSameDescriptor(t *testing.T) {
	life, err := retainProcessLifetime(os.Getpid(), -1, processCookie{})
	if err != nil {
		t.Fatal(err)
	}
	defer life.close()
	descriptor, calls := life.pidFD.Fd(), 0
	err = verifyProcessDescriptor(descriptor, func(fd int32) (uintptr, int16, syscall.Errno) {
		calls++
		if fd != int32(descriptor) {
			t.Fatal("retry changed retained descriptor")
		}
		if calls <= 2 {
			return ^uintptr(0), 0, syscall.EINTR
		}
		return pollProcessDescriptor(fd)
	})
	if err != nil || calls != 3 {
		t.Fatalf("live interrupted descriptor: calls=%d err=%v", calls, err)
	}
}

func TestClosedProcessLifetimeIsUnavailable(t *testing.T) {
	life, err := retainProcessLifetime(os.Getpid(), -1, processCookie{})
	if err != nil {
		t.Fatal(err)
	}
	life.close()
	if err := life.verify(); err == nil {
		t.Fatal("closed pidfd was accepted as live")
	}
}

func TestProcessLifetimeRejectsPollErrorsAndRetirementAfterInterruption(t *testing.T) {
	for _, result := range []struct {
		name   string
		count  uintptr
		events int16
		errno  syscall.Errno
	}{
		{"exited", 1, 1, 0}, {"reaped", 1, 16, 0}, {"invalid", 1, 32, 0}, {"bad fd", 0, 0, syscall.EBADF},
	} {
		t.Run(result.name, func(t *testing.T) {
			calls := 0
			err := verifyProcessDescriptor(42, func(fd int32) (uintptr, int16, syscall.Errno) {
				calls++
				if fd != 42 {
					t.Fatal("retry changed descriptor")
				}
				if calls == 1 {
					return ^uintptr(0), 0, syscall.EINTR
				}
				return result.count, result.events, result.errno
			})
			if err == nil || calls != 2 {
				t.Fatalf("failed poll result: calls=%d err=%v", calls, err)
			}
		})
	}
}
