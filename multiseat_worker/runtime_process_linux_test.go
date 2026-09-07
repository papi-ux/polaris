//go:build linux

package main

import (
	"context"
	"errors"
	"os"
	"os/exec"
	"os/signal"
	"path/filepath"
	"reflect"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"testing"
	"time"
)

const runtimeProcessTestMode = "POLARIS_RUNTIME_PROCESS_TEST_MODE"

func blockRuntimeProcessTestHelper() {
	for {
		time.Sleep(time.Hour)
	}
}

func appendRuntimeProcessTestEvent(path string, event string) error {
	if path == "" {
		return nil
	}
	file, err := os.OpenFile(path, os.O_APPEND|os.O_CREATE|os.O_WRONLY, 0o600)
	if err != nil {
		return err
	}
	defer file.Close()
	_, err = file.WriteString(event + "\n")
	return err
}

func runtimeProcessTestStage() string {
	for _, argument := range os.Args[1:] {
		if strings.HasPrefix(argument, "--stage=") {
			return strings.TrimPrefix(argument, "--stage=")
		}
	}
	return "unknown"
}

func runtimeProcessTestHelper(mode string) int {
	if os.Getenv("POLARIS_PARENT_SECRET") != "" {
		return 91
	}
	if mode == "child" {
		blockRuntimeProcessTestHelper()
	}
	eventFile := os.Getenv("POLARIS_RUNTIME_PROCESS_TEST_EVENT_FILE")
	stage := runtimeProcessTestStage()
	readyDescriptor, err := strconv.Atoi(os.Getenv(runtimeReadyFDSetting))
	if err != nil || readyDescriptor < 3 {
		return 92
	}
	ready := os.NewFile(uintptr(readyDescriptor), "runtime-ready")
	if ready == nil {
		return 93
	}
	defer ready.Close()
	if argumentFile := os.Getenv("POLARIS_RUNTIME_PROCESS_TEST_ARGUMENT_FILE"); argumentFile != "" {
		if err := os.WriteFile(argumentFile, []byte(strings.Join(os.Args[1:], "\x00")), 0o600); err != nil {
			return 94
		}
	}
	var child *exec.Cmd
	if childPIDFile := os.Getenv("POLARIS_RUNTIME_PROCESS_TEST_CHILD_PID_FILE"); childPIDFile != "" {
		executable, err := os.Executable()
		if err != nil {
			return 95
		}
		child = exec.Command(executable)
		child.Env = []string{runtimeProcessTestMode + "=child"}
		if err := child.Start(); err != nil {
			return 96
		}
		if err := os.WriteFile(
			childPIDFile,
			[]byte(strconv.Itoa(child.Process.Pid)),
			0o600,
		); err != nil {
			_ = child.Process.Kill()
			return 97
		}
	}
	signals := make(chan os.Signal, 1)
	if mode == "ignore-term" {
		signal.Ignore(syscall.SIGTERM)
	} else {
		signal.Notify(signals, syscall.SIGTERM, syscall.SIGINT)
	}
	switch mode {
	case "ready", "ignore-term":
		if err := appendRuntimeProcessTestEvent(eventFile, "start:"+stage); err != nil {
			return 101
		}
		if _, err := ready.WriteString(runtimeReadyRecord); err != nil {
			return 98
		}
	case "invalid-ready":
		_, _ = ready.WriteString("NOT-READY\n")
	case "exit-before-ready":
		return 99
	case "silent":
	default:
		return 100
	}
	if mode == "ignore-term" {
		blockRuntimeProcessTestHelper()
	}
	<-signals
	if err := appendRuntimeProcessTestEvent(eventFile, "stop:"+stage); err != nil {
		return 102
	}
	if child != nil {
		_ = child.Wait()
	}
	return 0
}

type runtimeProcessTestHost struct {
	eventFile string
}

func (host runtimeProcessTestHost) Start(
	parent context.Context,
	spec runtimeProcessSpec,
) (runtimeLease, error) {
	spec.Environment = append(spec.Environment,
		runtimeProcessTestMode+"=ready",
		"POLARIS_RUNTIME_PROCESS_TEST_EVENT_FILE="+host.eventFile,
		"GORACE=atexit_sleep_ms=0",
	)
	return (osRuntimeProcessHost{}).Start(parent, spec)
}

func TestMain(main *testing.M) {
	if mode := os.Getenv(runtimeProcessTestMode); mode != "" {
		os.Exit(runtimeProcessTestHelper(mode))
	}
	os.Exit(main.Run())
}

func runtimeProcessTestSpec(
	t *testing.T,
	mode string,
	arguments ...string,
) runtimeProcessSpec {
	t.Helper()
	executable, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	return runtimeProcessSpec{
		Stage:      runtimeStageSessionBus,
		Executable: executable,
		Arguments:  append([]string{"serve", "--stage=session-bus"}, arguments...),
		Environment: []string{
			runtimeProcessTestMode + "=" + mode,
			"GORACE=atexit_sleep_ms=0",
		},
	}
}

func waitForProcessAbsent(t *testing.T, pid int) {
	t.Helper()
	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		err := syscall.Kill(pid, 0)
		if errors.Is(err, syscall.ESRCH) {
			return
		}
		time.Sleep(time.Millisecond)
	}
	t.Fatalf("runtime helper descendant %d remained alive", pid)
}

func TestProcessRuntimeAdaptersDriveRealHelpersInExactOrder(t *testing.T) {
	executable, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	eventFile := filepath.Join(t.TempDir(), "events")
	adapters, err := newProcessRuntimeAdapters(
		runtimeProcessTestHost{eventFile: eventFile},
		processRuntimeAdapterOptions{
			HelperExecutable: executable,
		},
	)
	if err != nil {
		t.Fatal(err)
	}
	runtime, err := startWorkerRuntime(
		context.Background(),
		runtimeTestConfig("worker-real-processes", 74, 2),
		adapters,
		runtimeOptions{
			StartupTimeout:       10 * time.Second,
			ComponentStopTimeout: 2 * time.Second,
		},
	)
	if err != nil {
		t.Fatal(err)
	}
	if err := runtime.Stop(); err != nil {
		t.Fatal(err)
	}
	content, err := os.ReadFile(eventFile)
	if err != nil {
		t.Fatal(err)
	}
	got := strings.Fields(string(content))
	if want := completeRuntimeEvents(); !reflect.DeepEqual(got, want) {
		t.Fatalf("real process lifecycle order mismatch: %#v", got)
	}
}

func TestOSRuntimeProcessHostUsesLiteralArgvSanitizedEnvironmentAndGroupStop(t *testing.T) {
	t.Setenv("POLARIS_PARENT_SECRET", "must-not-be-inherited")
	temporary := t.TempDir()
	argumentFile := filepath.Join(temporary, "arguments")
	childPIDFile := filepath.Join(temporary, "child-pid")
	marker := filepath.Join(temporary, "shell-was-used")
	spec := runtimeProcessTestSpec(
		t,
		"ready",
		"--literal=$(touch "+marker+")",
	)
	spec.Environment = append(spec.Environment,
		"POLARIS_RUNTIME_PROCESS_TEST_ARGUMENT_FILE="+argumentFile,
		"POLARIS_RUNTIME_PROCESS_TEST_CHILD_PID_FILE="+childPIDFile,
	)
	startContext, cancelStart := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancelStart()
	lease, err := (osRuntimeProcessHost{}).Start(startContext, spec)
	if err != nil {
		t.Fatal(err)
	}
	arguments, err := os.ReadFile(argumentFile)
	if err != nil {
		t.Fatal(err)
	}
	wantArguments := strings.Join(spec.Arguments, "\x00")
	if string(arguments) != wantArguments {
		t.Fatalf("helper argv was not literal: %q", arguments)
	}
	if _, err := os.Stat(marker); !errors.Is(err, os.ErrNotExist) {
		t.Fatalf("literal helper argument was executed: %v", err)
	}
	childText, err := os.ReadFile(childPIDFile)
	if err != nil {
		t.Fatal(err)
	}
	childPID, err := strconv.Atoi(string(childText))
	if err != nil {
		t.Fatal(err)
	}
	if err := syscall.Kill(childPID, 0); err != nil {
		t.Fatalf("runtime helper descendant was not alive before stop: %v", err)
	}
	stopContext, cancelStop := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancelStop()
	if err := lease.Stop(stopContext); err != nil {
		t.Fatal(err)
	}
	select {
	case <-lease.Done():
	case <-time.After(2 * time.Second):
		t.Fatal("runtime helper did not exit after group stop")
	}
	waitForProcessAbsent(t, childPID)
	if err := lease.Stop(stopContext); err != nil {
		t.Fatalf("idempotent helper stop failed: %v", err)
	}
}

func TestOSRuntimeProcessHostReturnsCleanableLeaseForReadinessFailures(t *testing.T) {
	for _, test := range []struct {
		name string
		mode string
	}{
		{name: "invalid record", mode: "invalid-ready"},
		{name: "early exit", mode: "exit-before-ready"},
		{name: "timeout", mode: "silent"},
	} {
		t.Run(test.name, func(t *testing.T) {
			startContext, cancelStart := context.WithTimeout(context.Background(), 75*time.Millisecond)
			defer cancelStart()
			lease, err := (osRuntimeProcessHost{}).Start(
				startContext,
				runtimeProcessTestSpec(t, test.mode),
			)
			if lease == nil || err == nil {
				t.Fatalf("unexpected readiness result: %#v, %v", lease, err)
			}
			stopContext, cancelStop := context.WithTimeout(context.Background(), time.Second)
			defer cancelStop()
			if err := lease.Stop(stopContext); err != nil {
				t.Fatal(err)
			}
		})
	}
}

func TestOSRuntimeProcessHostForceKillsHelperAtStopDeadline(t *testing.T) {
	startContext, cancelStart := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancelStart()
	lease, err := (osRuntimeProcessHost{}).Start(
		startContext,
		runtimeProcessTestSpec(t, "ignore-term"),
	)
	if err != nil || lease == nil {
		t.Fatalf("ignore-term helper failed startup: %#v, %v", lease, err)
	}
	stopContext, cancelStop := context.WithTimeout(context.Background(), 50*time.Millisecond)
	defer cancelStop()
	if err := lease.Stop(stopContext); err == nil ||
		!strings.Contains(err.Error(), "stop deadline exceeded") {
		t.Fatalf("unexpected forced-stop result: %v", err)
	}
	select {
	case <-lease.Done():
	case <-time.After(2 * time.Second):
		t.Fatal("forced runtime helper did not exit")
	}
}

func TestOSRuntimeProcessLeaseConcurrentStopIsIdempotent(t *testing.T) {
	startContext, cancelStart := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancelStart()
	lease, err := (osRuntimeProcessHost{}).Start(
		startContext,
		runtimeProcessTestSpec(t, "ready"),
	)
	if err != nil {
		t.Fatal(err)
	}
	const callers = 8
	errorsSeen := make(chan error, callers)
	var wait sync.WaitGroup
	for index := 0; index < callers; index++ {
		wait.Add(1)
		go func() {
			defer wait.Done()
			stopContext, cancelStop := context.WithTimeout(context.Background(), 2*time.Second)
			defer cancelStop()
			errorsSeen <- lease.Stop(stopContext)
		}()
	}
	wait.Wait()
	close(errorsSeen)
	for stopError := range errorsSeen {
		if stopError != nil {
			t.Fatal(stopError)
		}
	}
	select {
	case <-lease.Done():
	case <-time.After(2 * time.Second):
		t.Fatal("concurrent stop did not terminate the helper")
	}
}

func TestOSRuntimeProcessLeasesStopIndependently(t *testing.T) {
	startContext, cancelStart := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancelStart()
	first, err := (osRuntimeProcessHost{}).Start(
		startContext,
		runtimeProcessTestSpec(t, "ready"),
	)
	if err != nil {
		t.Fatal(err)
	}
	second, err := (osRuntimeProcessHost{}).Start(
		startContext,
		runtimeProcessTestSpec(t, "ready"),
	)
	if err != nil {
		stopContext, cancelStop := context.WithTimeout(context.Background(), time.Second)
		defer cancelStop()
		_ = first.Stop(stopContext)
		t.Fatal(err)
	}
	stopFirst, cancelFirst := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancelFirst()
	if err := first.Stop(stopFirst); err != nil {
		t.Fatal(err)
	}
	select {
	case <-second.Done():
		t.Fatal("stopping one runtime helper terminated another process group")
	case <-time.After(50 * time.Millisecond):
	}
	stopSecond, cancelSecond := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancelSecond()
	if err := second.Stop(stopSecond); err != nil {
		t.Fatal(err)
	}
}

func TestOSRuntimeProcessHostRejectsInvalidSpecificationsBeforeStart(t *testing.T) {
	tests := []struct {
		name   string
		mutate func(*runtimeProcessSpec)
	}{
		{
			name: "unknown stage",
			mutate: func(spec *runtimeProcessSpec) {
				spec.Stage = 0
			},
		},
		{
			name: "relative executable",
			mutate: func(spec *runtimeProcessSpec) {
				spec.Executable = "helper"
			},
		},
		{
			name: "duplicate environment",
			mutate: func(spec *runtimeProcessSpec) {
				spec.Environment = append(spec.Environment, runtimeProcessTestMode+"=again")
			},
		},
		{
			name: "reserved ready descriptor",
			mutate: func(spec *runtimeProcessSpec) {
				spec.Environment = append(spec.Environment, runtimeReadyFDSetting+"=9")
			},
		},
		{
			name: "missing executable",
			mutate: func(spec *runtimeProcessSpec) {
				spec.Executable = "/definitely/missing/polaris-seat-runtime"
			},
		},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			spec := runtimeProcessTestSpec(t, "ready")
			test.mutate(&spec)
			lease, err := (osRuntimeProcessHost{}).Start(context.Background(), spec)
			if lease != nil || err == nil {
				t.Fatalf("invalid process spec was accepted: %#v, %v", lease, err)
			}
		})
	}
}
