//go:build linux

package seatprovider

import (
	"context"
	"io"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"testing"
	"time"

	"github.com/papi-ux/polaris/multiseat_worker/internal/seatruntime"
)

func withStopNoticeGrace(t *testing.T, grace time.Duration) {
	t.Helper()
	previous := stopNoticeGrace
	stopNoticeGrace = grace
	t.Cleanup(func() { stopNoticeGrace = previous })
}

func TestAStopAlreadyRequestedIsSeenAtOnce(t *testing.T) {
	withStopNoticeGrace(t, time.Hour)
	parent, cancel := context.WithCancel(context.Background())
	cancel()
	if !stopRequested(parent) {
		t.Fatal("a cancelled helper called its child's exit unexpected")
	}
}

func TestAStopThatArrivesJustAfterTheChildExitsStillCounts(t *testing.T) {
	withStopNoticeGrace(t, 5*time.Second)
	parent, cancel := context.WithCancel(context.Background())
	defer cancel()
	// The group signal reached the child first; this process hears of it a moment later.
	time.AfterFunc(20*time.Millisecond, cancel)
	started := time.Now()
	if !stopRequested(parent) {
		t.Fatal("the helper's own stop signal arrived within the grace and was not counted")
	}
	if waited := time.Since(started); waited > 2*time.Second {
		t.Fatalf("waited %v after the stop was already known", waited)
	}
}

func TestAChildThatDiesOnItsOwnIsStillUnexpected(t *testing.T) {
	withStopNoticeGrace(t, 30*time.Millisecond)
	if stopRequested(context.Background()) {
		t.Fatal("nobody asked for a stop, so the exit has to be reported")
	}
}

func sessionBusDaemonPID(t *testing.T, busPath string) int {
	t.Helper()
	want := "--address=unix:path=" + busPath
	entries, err := os.ReadDir("/proc")
	if err != nil {
		t.Fatal(err)
	}
	for _, entry := range entries {
		pid, err := strconv.Atoi(entry.Name())
		if err != nil {
			continue
		}
		raw, err := os.ReadFile(filepath.Join("/proc", entry.Name(), "cmdline"))
		if err != nil {
			continue
		}
		for _, argument := range strings.Split(string(raw), "\x00") {
			if argument == want {
				return pid
			}
		}
	}
	t.Fatal("the session bus daemon is not running")
	return 0
}

// The worker stops a helper by signalling its process group, so the bus daemon
// is told to go in the same instant and is gone before the helper's context is
// cancelled. That is a clean stop, and it used to come back as "runtime session
// bus exited unexpectedly (exit status 0)" on every disconnect.
//
// The name is short on purpose: it is part of the socket path, which has 108 bytes.
func TestRealSessionBusGroupStopIsClean(t *testing.T) {
	withStopNoticeGrace(t, 3*time.Second)
	runtimePath := privateRuntimeDirectoryForTest(t)
	options := realProviderOptions(t, runtimePath, false)
	request := seatruntime.Request{
		Stage:            seatruntime.StageSessionBus,
		RuntimeNamespace: "real-bus-group-stop",
	}
	provider := startRealProvider(t, func(context context.Context, ready io.WriteCloser) error {
		return runSessionBus(context, request, ready, options)
	})
	daemon := sessionBusDaemonPID(t, filepath.Join(runtimePath, "bus"))
	if err := syscall.Kill(daemon, syscall.SIGTERM); err != nil {
		t.Fatal(err)
	}
	// Wait until the daemon has been reaped, so its exit is what the helper sees first.
	deadline := time.Now().Add(2 * time.Second)
	for {
		if _, err := os.Stat(filepath.Join("/proc", strconv.Itoa(daemon))); os.IsNotExist(err) {
			break
		}
		if time.Now().After(deadline) {
			t.Fatal("the session bus daemon ignored SIGTERM")
		}
		time.Sleep(5 * time.Millisecond)
	}
	// Fails the test if the provider comes back with an error.
	stopRealProvider(t, provider)
	requireEmptyRuntime(t, runtimePath)
}
