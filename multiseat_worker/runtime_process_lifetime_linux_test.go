//go:build linux

package main

import (
	"context"
	"errors"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"testing"
	"time"
)

// Run the adversarial family in its own subreaper. This fixture alone adopts
// and reaps its known orphan; it cannot steal any other runtime's Wait result.
func TestOSRuntimeProcessPinsLeaderUntilSurvivingGroupIsGone(t *testing.T) {
	if os.Getenv("POLARIS_LIFETIME_TEST_OWNER") != "1" {
		executable, err := os.Executable()
		if err != nil {
			t.Fatal(err)
		}
		ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
		defer cancel()
		command := exec.CommandContext(ctx, executable, "-test.run=^TestOSRuntimeProcessPinsLeaderUntilSurvivingGroupIsGone$", "-test.v")
		command.Env = append(os.Environ(), "POLARIS_LIFETIME_TEST_OWNER=1", "GORACE=atexit_sleep_ms=0")
		output, err := command.CombinedOutput()
		if err != nil {
			t.Fatalf("isolated process family: %v\n%s", err, output)
		}
		return
	}
	if _, _, errno := syscall.Syscall6(syscall.SYS_PRCTL, 36, 1, 0, 0, 0, 0); errno != 0 {
		t.Fatal(errno)
	}
	for _, earlyExit := range []bool{true, false} {
		t.Run(strconv.FormatBool(earlyExit), func(t *testing.T) {
			temporary := t.TempDir()
			childFile := filepath.Join(temporary, "child")
			trigger := filepath.Join(temporary, "exit")
			spec := runtimeProcessTestSpec(t, "leave-helper")
			spec.Environment = append(spec.Environment, "POLARIS_RUNTIME_PROCESS_TEST_CHILD_PID_FILE="+childFile, "POLARIS_EXIT_TRIGGER="+trigger)
			start, cancelStart := context.WithTimeout(context.Background(), 2*time.Second)
			defer cancelStart()
			result, err := (osRuntimeProcessHost{}).Start(start, spec)
			if err != nil {
				t.Fatal(err)
			}
			lease := result.(*osRuntimeProcessLease)
			childText, err := os.ReadFile(childFile)
			if err != nil {
				t.Fatal(err)
			}
			childPID, err := strconv.Atoi(string(childText))
			if err != nil {
				t.Fatal(err)
			}
			reaped := make(chan error, 1)
			go func() {
				deadline := time.Now().Add(8 * time.Second)
				for time.Now().Before(deadline) {
					var status syscall.WaitStatus
					pid, err := syscall.Wait4(childPID, &status, syscall.WNOHANG, nil)
					if pid == childPID {
						reaped <- nil
						return
					}
					if err != nil && !errors.Is(err, syscall.ECHILD) && !errors.Is(err, syscall.EINTR) {
						reaped <- err
						return
					}
					time.Sleep(time.Millisecond)
				}
				reaped <- errors.New("fixture orphan was not reaped")
			}()
			t.Cleanup(func() { _ = lease.Stop(context.Background()); <-lease.stopComplete })
			if earlyExit {
				if err := os.WriteFile(trigger, []byte("exit"), 0600); err != nil {
					t.Fatal(err)
				}
				select {
				case <-lease.Done():
				case <-time.After(time.Second):
					t.Fatal("leader did not exit")
				}
				content, err := os.ReadFile(filepath.Join("/proc", strconv.Itoa(lease.process.Pid), "stat"))
				if err != nil || !strings.Contains(string(content), ") Z ") {
					t.Fatalf("leader was reaped before group cleanup: %v %s", err, content)
				}
				if err := syscall.Kill(childPID, 0); err != nil {
					t.Fatalf("test helper vanished: %v", err)
				}
			}
			stop, cancelStop := context.WithTimeout(context.Background(), 4*time.Second)
			defer cancelStop()
			if err := lease.Stop(stop); err != nil {
				t.Fatal(err)
			}
			if err := <-reaped; err != nil {
				t.Fatal(err)
			}
			waitForProcessAbsent(t, childPID)
			waitForProcessAbsent(t, lease.process.Pid)
			if _, err := lease.pidFD.Stat(); !errors.Is(err, os.ErrClosed) {
				t.Fatalf("pidfd retained after proven cleanup: %v", err)
			}
			if err := lease.Stop(context.Background()); err != nil {
				t.Fatalf("repeat stop: %v", err)
			}
		})
	}
}

func TestOSRuntimeProcessCanceledStopStillReapsOwnedLeader(t *testing.T) {
	result, err := (osRuntimeProcessHost{}).Start(context.Background(), runtimeProcessTestSpec(t, "ignore-term"))
	if err != nil {
		t.Fatal(err)
	}
	lease := result.(*osRuntimeProcessLease)
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if err := lease.Stop(ctx); err == nil {
		t.Fatal("canceled caller reported success")
	}
	select {
	case <-lease.stopComplete:
	case <-time.After(3 * time.Second):
		t.Fatal("cleanup owner abandoned canceled stop")
	}
	if lease.stopError == nil {
		t.Fatal("canceled cleanup result lost")
	}
	waitForProcessAbsent(t, lease.process.Pid)
}

func TestOSRuntimeProcessMissingPidFDReturnsCleanablePartialLease(t *testing.T) {
	host := osRuntimeProcessHost{startCommand: func(command *exec.Cmd) error {
		if err := command.Start(); err != nil {
			return err
		}
		if descriptor := *command.SysProcAttr.PidFD; descriptor >= 0 {
			_ = syscall.Close(descriptor)
		}
		*command.SysProcAttr.PidFD = -1
		return nil
	}}
	result, err := host.Start(context.Background(), runtimeProcessTestSpec(t, "ignore-term"))
	if err == nil || result == nil {
		t.Fatalf("missing partial-start lease: %v %v", result, err)
	}
	lease := result.(*osRuntimeProcessLease)
	if lease.pidFD != nil {
		t.Fatal("fixture did not remove pidfd support")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 250*time.Millisecond)
	defer cancel()
	_ = lease.Stop(ctx)
	select {
	case <-lease.stopComplete:
	case <-time.After(3 * time.Second):
		t.Fatal("partial-start cleanup was abandoned")
	}
	waitForProcessAbsent(t, lease.process.Pid)
}
