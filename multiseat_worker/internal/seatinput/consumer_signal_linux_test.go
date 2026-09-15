//go:build linux

package seatinput

import (
	"os"
	"os/exec"
	"runtime"
	"sync/atomic"
	"syscall"
	"testing"
	"time"
)

// Go's preemption signal can interrupt even a zero-timeout pidfd poll. Keep
// the real child alive while delivering signals to the polling thread; an
// interrupted check must not retire its input, display, and encoder providers.
func TestConsumerRemainsAliveDuringThreadSignals(t *testing.T) {
	pidFD := -1
	command := exec.Command("/bin/sleep", "30")
	command.SysProcAttr = &syscall.SysProcAttr{PidFD: &pidFD}
	if err := command.Start(); err != nil {
		t.Fatal(err)
	}
	defer func() { _ = command.Process.Kill(); _ = command.Wait() }()
	if pidFD < 0 {
		t.Fatal("pidfd is required for input consumer proof")
	}
	defer syscall.Close(pidFD)
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	pid, tid := os.Getpid(), syscall.Gettid()
	var stopped atomic.Bool
	var signalError atomic.Uint32
	done := make(chan struct{})
	go func() {
		defer close(done)
		for !stopped.Load() {
			_, _, errno := syscall.RawSyscall(syscall.SYS_TGKILL,
				uintptr(pid), uintptr(tid), uintptr(syscall.SIGURG))
			if errno != 0 {
				signalError.Store(uint32(errno))
				return
			}
			time.Sleep(10 * time.Microsecond)
		}
	}()
	defer func() { stopped.Store(true); <-done }()
	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		if !consumerAlive(pidFD) {
			if err := command.Process.Signal(syscall.Signal(0)); err != nil {
				t.Fatalf("test consumer unexpectedly exited: %v", err)
			}
			t.Fatal("signal interruption reported a living input consumer as dead")
		}
	}
	if errno := signalError.Load(); errno != 0 {
		t.Fatalf("could not deliver test signals: %v", syscall.Errno(errno))
	}
}
