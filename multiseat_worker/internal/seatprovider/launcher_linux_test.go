//go:build linux

package seatprovider

import (
	"context"
	"os"
	"os/exec"
	"os/signal"
	"path/filepath"
	"reflect"
	"strconv"
	"strings"
	"syscall"
	"testing"
	"time"

	"github.com/papi-ux/polaris/multiseat_worker/internal/seatruntime"
)

func TestLauncherSessionRejectsWrongAllocationAndMalformedMetadata(t *testing.T) {
	request := nestedRequestFromDisplay(displayRequest("launcher-record", "polaris-capture-launcher"), "polaris-wayland-launcher")
	record := gamescopeLauncherRecord(gamescopeReadyInfo{displayName: ":0"}, request, 123, processCookie{1, 2})
	session, err := parseLauncherSession(record, request.WaylandSocket)
	if err != nil || session.pid != 123 || session.width != request.DisplayWidth || session.refresh != request.DisplayRefreshMillihertz {
		t.Fatal(session, err)
	}
	if _, err := parseLauncherSession(record, "polaris-wayland-other"); err == nil {
		t.Fatal("cross-seat session accepted")
	}
	for _, record := range []string{
		strings.Replace(string(record), "PID=123", "PID=0123", 1),
		strings.Replace(string(record), "PID=123", "PID=0", 1),
		strings.Replace(string(record), "DISPLAY=:0", "DISPLAY=:1", 1),
		strings.Replace(string(record), "SESSION/3", "SESSION/2", 1),
		string(record) + "LD_PRELOAD=/profile/plugin.so\n",
		strings.Replace(string(record), "PID=123", "PID=123\nPID=456", 1),
	} {
		if _, err := parseLauncherSession([]byte(record), request.WaylandSocket); err == nil {
			t.Fatal("malformed session accepted")
		}
	}
}

func TestLauncherRechecksNestedProtocolsBeforeApplicationStart(t *testing.T) {
	runtimePath := privateDisplayRuntimeDirectoryForTest(t)
	display := displayRequest("launcher-ready", "polaris-capture-launcher-ready")
	outer, _ := startNestedOuterDisplay(t, runtimePath, display)
	defer stopRealProvider(t, outer)
	request := nestedRequestFromDisplay(display, "polaris-wayland-launcher-ready")
	options := fakeNestedOptions(t, runtimePath, "good")
	nested := startFakeNestedProvider(t, request, options)
	defer stopRealProvider(t, nested)
	runtime, err := openRuntimeDirectory(runtimePath, uint32(os.Geteuid()))
	if err != nil {
		t.Fatal(err)
	}
	defer runtime.close()
	accepted, err := readLauncherSession(context.Background(), request, runtime, options)
	if err != nil {
		t.Fatal(err)
	}
	defer accepted.lifetime.close()
	other := request
	other.WaylandSocket = "polaris-wayland-other"
	if _, err := readLauncherSession(context.Background(), other, runtime, options); err == nil {
		t.Fatal("launcher accepted another display")
	}
	_, _, name := gamescopeScopedNames(request.RuntimeNamespace)
	path := filepath.Join(runtimePath, name)
	content, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	// Restore bytes on the same owned inode before the nested provider cleans up.
	defer os.WriteFile(path, content, 0600)
	session, err := parseLauncherSession(content, request.WaylandSocket)
	if err != nil {
		t.Fatal(err)
	}
	staleCookie := strings.Replace(string(content), "PROCESS_INODE="+strconv.FormatUint(session.cookie.inode, 10), "PROCESS_INODE="+strconv.FormatUint(session.cookie.inode+1, 10), 1)
	if err := os.WriteFile(path, []byte(staleCookie), 0600); err != nil {
		t.Fatal(err)
	}
	if acquired, err := readLauncherSession(context.Background(), request, runtime, options); err == nil {
		acquired.lifetime.close()
		t.Fatal("launcher accepted stale lifetime cookie with valid live protocol peers")
	}
	forged := strings.Replace(string(content), "PID="+strconv.Itoa(session.pid), "PID="+strconv.Itoa(os.Getpid()), 1)
	if err := os.WriteFile(path, []byte(forged), 0600); err != nil {
		t.Fatal(err)
	}
	if _, err := readLauncherSession(context.Background(), request, runtime, options); err == nil {
		t.Fatal("launcher accepted a stale or substituted compositor PID")
	}
}

func TestLauncherRejectsUnimplementedWorkloads(t *testing.T) {
	request := seatruntime.Request{Stage: seatruntime.StageLauncher, RuntimeNamespace: "launcher-test", RuntimeProfile: "gamescope", WorkloadKind: seatruntime.WorkloadGamescope, WorkloadID: "input-pong-v1", WaylandSocket: "polaris-wayland-test", AudioSink: "audio-test", InputSeat: "input-test"}
	command, err := planLauncher(request)
	if err != nil || command.executable != "/usr/libexec/polaris-seat/workloads/input-pong-v1" || len(command.arguments) != 0 || command.retainDescendants {
		t.Fatal(command, err)
	}
	for _, id := range []string{"../input-pong-v1", "/bin/sh", "steam", "unknown", "input-pong-v1 --other"} {
		request.WorkloadID = id
		if _, err := planLauncher(request); err == nil {
			t.Fatal("unimplemented workload accepted")
		}
	}
}

func TestSteamLauncherUsesTypedTargetsAndCanonicalPackagePath(t *testing.T) {
	t.Setenv("SDL_JOYSTICK_DEVICE", "/dev/input/event0")
	request := seatruntime.Request{Stage: seatruntime.StageLauncher, RuntimeNamespace: "steam-test", RuntimeProfile: "steam", WorkloadKind: seatruntime.WorkloadSteam, WorkloadID: seatruntime.SteamBigPicture, WaylandSocket: "polaris-wayland-test", AudioSink: "audio-test", InputSeat: "input-test"}
	for _, target := range []string{seatruntime.SteamBigPicture, "570"} {
		request.WorkloadID = target
		command, err := planLauncher(request)
		if err != nil || command.executable != "/usr/bin/bash" || command.packageScript != "/usr/games/steam" || !command.retainDescendants {
			t.Fatal(command, err)
		}
		want := []string{"/usr/games/steam", "-gamepadui"}
		if target == "570" {
			want = append(want, "-applaunch", "570")
		}
		if !reflect.DeepEqual(command.arguments, want) {
			t.Fatal(command.arguments)
		}
		environment, err := launcherEnvironment(request, launcherSession{display: ":0", width: 1920, height: 1080, refresh: 60000})
		if err != nil {
			t.Fatal(err)
		}
		for _, required := range []string{"DISPLAY=:0", "HOME=" + launcherHome, "XDG_RUNTIME_DIR=/run/polaris", "PULSE_SINK=audio-test", "SDL_JOYSTICK_DEVICE=/dev/input/polaris-gamepad-0"} {
			if !strings.Contains(strings.Join(environment, "\n"), required+"\n") {
				t.Fatalf("missing private environment %s", required)
			}
		}
	}
}

func TestWorkloadTreeHelper(t *testing.T) {
	mode := os.Getenv("POLARIS_WORKLOAD_TEST_MODE")
	if mode == "" {
		return
	}
	directory := os.Getenv("POLARIS_WORKLOAD_TEST_DIRECTORY")
	executable, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	environment := func(next string) []string {
		return []string{"POLARIS_WORKLOAD_TEST_MODE=" + next, "POLARIS_WORKLOAD_TEST_DIRECTORY=" + directory}
	}
	if mode == "supervise" || mode == "supervise-exit" {
		if err := enableWorkloadSubreaper(); err != nil {
			t.Fatal(err)
		}
		next := "parent"
		if mode == "supervise-exit" {
			next = "parent-exit"
		}
		child, err := startManagedChild(executable, uint32(os.Geteuid()), []string{"-test.run=^TestWorkloadTreeHelper$"}, environment(next), nil)
		if err != nil {
			t.Fatal(err)
		}
		defer stopWorkload(child, 4*time.Second)
		deadline := time.Now().Add(5 * time.Second)
		for {
			if _, err := os.Stat(filepath.Join(directory, "leaf")); err == nil {
				break
			}
			if time.Now().After(deadline) || (mode == "supervise" && child.exited()) {
				t.Fatal("workload descendants did not start")
			}
			time.Sleep(10 * time.Millisecond)
		}
		if mode == "supervise-exit" {
			select {
			case <-child.done:
			case <-time.After(2 * time.Second):
				t.Fatal("primary did not exit")
			}
			if alive, err := launcherDescendantsAlive(true); err != nil || !alive {
				t.Fatal("detached Steam lifetime lost", err)
			}
			if alive, err := launcherDescendantsAlive(false); err != nil || alive {
				t.Fatal("ordinary workload retained descendants", err)
			}
		}
		if err := stopWorkload(child, 4*time.Second); err != nil {
			t.Fatal(err)
		}
		if alive, err := launcherDescendantsAlive(true); err != nil || alive {
			t.Fatal("retired descendants keep session alive", err)
		}
		return
	}
	signal.Ignore(syscall.SIGTERM)
	if mode == "parent" || mode == "middle" || mode == "parent-exit" {
		next := "middle"
		if mode == "middle" || mode == "parent-exit" {
			next = "leaf"
		}
		command := exec.Command(executable, "-test.run=^TestWorkloadTreeHelper$")
		command.Env = environment(next)
		command.SysProcAttr = &syscall.SysProcAttr{Setsid: true}
		if err := command.Start(); err != nil {
			t.Fatal(err)
		}
	} else if mode != "leaf" {
		t.Fatal("unknown helper mode")
	}
	if err := os.WriteFile(filepath.Join(directory, mode), []byte(strconv.Itoa(os.Getpid())), 0600); err != nil {
		t.Fatal(err)
	}
	if mode == "parent-exit" {
		return
	}
	time.Sleep(30 * time.Second)
}

func TestSteamLauncherRetainsDetachedClientAfterInitialScriptExits(t *testing.T) {
	executable, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	directory := t.TempDir()
	parent, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	command := exec.CommandContext(parent, executable, "-test.run=^TestWorkloadTreeHelper$")
	command.Env = []string{"POLARIS_WORKLOAD_TEST_MODE=supervise-exit", "POLARIS_WORKLOAD_TEST_DIRECTORY=" + directory}
	if output, err := command.CombinedOutput(); err != nil {
		t.Fatalf("detached client supervision failed: %v\n%s", err, output)
	}
	content, err := os.ReadFile(filepath.Join(directory, "leaf"))
	if err != nil {
		t.Fatal(err)
	}
	pid, err := strconv.Atoi(string(content))
	if err != nil || syscall.Kill(pid, 0) != syscall.ESRCH {
		t.Fatal("detached client remains", err)
	}
}

func TestWorkloadStopReapsEscapedDescendantsAndPreservesOtherProcesses(t *testing.T) {
	executable, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	directory := t.TempDir()
	unrelated := exec.Command("/bin/sleep", "30")
	if err := unrelated.Start(); err != nil {
		t.Fatal(err)
	}
	defer func() { _ = unrelated.Process.Kill(); _ = unrelated.Wait() }()
	parent, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	command := exec.CommandContext(parent, executable, "-test.run=^TestWorkloadTreeHelper$")
	command.Env = []string{"POLARIS_WORKLOAD_TEST_MODE=supervise", "POLARIS_WORKLOAD_TEST_DIRECTORY=" + directory}
	if output, err := command.CombinedOutput(); err != nil {
		t.Fatalf("workload supervision failed: %v\n%s", err, output)
	}
	for _, name := range []string{"parent", "middle", "leaf"} {
		content, err := os.ReadFile(filepath.Join(directory, name))
		if err != nil {
			t.Fatal(err)
		}
		pid, err := strconv.Atoi(string(content))
		if err != nil {
			t.Fatal(err)
		}
		if syscall.Kill(pid, 0) != syscall.ESRCH {
			t.Fatalf("workload %s remains after teardown", name)
		}
	}
	if err := unrelated.Process.Signal(syscall.Signal(0)); err != nil {
		t.Fatal("unrelated process was affected")
	}
}
