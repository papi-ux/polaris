//go:build linux

package main

import (
	"bufio"
	"encoding/json"
	"errors"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"slices"
	"strconv"
	"strings"
	"syscall"
	"testing"
	"time"

	"github.com/papi-ux/polaris/multiseat_worker/internal/seatruntime"
)

const (
	testCatalogSetting = "POLARIS_SEAT_RUNTIME_TEST_CATALOG"
	testOwnerSetting   = "POLARIS_SEAT_RUNTIME_TEST_OWNER"
	testSecretSetting  = "POLARIS_SEAT_RUNTIME_TEST_SECRET"
)

func environmentWithoutTestAuthority(environment []string) []string {
	filtered := make([]string, 0, len(environment))
	for _, setting := range environment {
		name, _, _ := strings.Cut(setting, "=")
		if name == testCatalogSetting || name == testOwnerSetting ||
			name == testSecretSetting {
			continue
		}
		filtered = append(filtered, setting)
	}
	return filtered
}

func appendProviderEvent(path string, event string) error {
	file, err := os.OpenFile(path, os.O_APPEND|os.O_CREATE|os.O_WRONLY, 0o600)
	if err != nil {
		return err
	}
	defer file.Close()
	_, err = file.WriteString(event + "\n")
	return err
}

func providerTestMain(arguments []string, environment []string) int {
	separator := slices.Index(arguments, "--")
	if separator < 0 || len(arguments)-separator-1 < 3 {
		return 81
	}
	fixed := arguments[separator+1:]
	eventFile := fixed[0]
	childPIDFile := fixed[1]
	mode := fixed[2]
	for _, setting := range environment {
		if strings.HasPrefix(setting, testCatalogSetting+"=") ||
			strings.HasPrefix(setting, testOwnerSetting+"=") ||
			strings.HasPrefix(setting, testSecretSetting+"=") {
			return 82
		}
	}
	if err := appendProviderEvent(eventFile, "pid="+strconv.Itoa(os.Getpid())); err != nil {
		return 83
	}
	if err := appendProviderEvent(eventFile, "argv="+strings.Join(arguments, "\x00")); err != nil {
		return 84
	}
	if err := appendProviderEvent(eventFile, "env="+strings.Join(environment, "\x00")); err != nil {
		return 85
	}
	executable, err := os.Executable()
	if err != nil {
		return 86
	}
	child := exec.Command(executable, "provider-test-child")
	child.Env = []string{}
	child.Stdin = nil
	child.Stdout = io.Discard
	child.Stderr = io.Discard
	if err := child.Start(); err != nil {
		return 87
	}
	if err := os.WriteFile(childPIDFile, []byte(strconv.Itoa(child.Process.Pid)), 0o600); err != nil {
		_ = child.Process.Kill()
		return 88
	}
	readyDescriptor, err := strconv.Atoi(os.Getenv(seatruntime.ReadyFDSetting))
	if err != nil || readyDescriptor != 3 {
		_ = child.Process.Kill()
		return 89
	}
	ready := os.NewFile(uintptr(readyDescriptor), "runtime-ready")
	if ready == nil {
		_ = child.Process.Kill()
		return 90
	}
	defer ready.Close()
	switch mode {
	case "ready":
		_, err = ready.WriteString(seatruntime.ReadyRecord)
	case "invalid-ready":
		_, err = ready.WriteString("INVALID\n")
	case "silent":
	default:
		_ = child.Process.Kill()
		return 91
	}
	if err != nil {
		_ = child.Process.Kill()
		return 92
	}
	_ = child.Wait()
	return 0
}

func providerTestChild() int {
	for {
		time.Sleep(time.Hour)
	}
}

func TestMain(tests *testing.M) {
	if len(os.Args) > 1 && os.Args[1] == "provider-test-child" {
		os.Exit(providerTestChild())
	}
	if len(os.Args) > 1 && os.Args[1] == "serve-resource-v1" {
		os.Exit(providerTestMain(os.Args[1:], os.Environ()))
	}
	if len(os.Args) > 1 && os.Args[1] == "serve" &&
		os.Getenv(testCatalogSetting) != "" {
		owner, err := strconv.ParseUint(os.Getenv(testOwnerSetting), 10, 32)
		if err != nil {
			os.Exit(93)
		}
		if err := seatruntime.Run(
			os.Args[1:],
			environmentWithoutTestAuthority(os.Environ()),
			os.Getenv(testCatalogSetting),
			uint32(owner),
			uint32(owner),
		); err != nil {
			os.Exit(94)
		}
		os.Exit(95)
	}
	os.Exit(tests.Run())
}

type dispatchedHelper struct {
	command      *exec.Cmd
	ready        *bufio.Reader
	eventFile    string
	childPIDFile string
}

func writeTestCatalog(
	t *testing.T,
	providers []seatruntime.Provider,
	mode os.FileMode,
) string {
	t.Helper()
	content, err := json.Marshal(seatruntime.Catalog{Schema: 1, Providers: providers})
	if err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(t.TempDir(), "providers.json")
	if err := os.WriteFile(path, content, mode); err != nil {
		t.Fatal(err)
	}
	if err := os.Chmod(path, mode); err != nil {
		t.Fatal(err)
	}
	return path
}

func startDispatchedHelper(
	t *testing.T,
	request seatruntime.Request,
	catalogPath string,
) *dispatchedHelper {
	t.Helper()
	executable, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	arguments, err := seatruntime.Arguments(request)
	if err != nil {
		t.Fatal(err)
	}
	environment, err := seatruntime.Environment(request)
	if err != nil {
		t.Fatal(err)
	}
	readyReader, readyWriter, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	command := exec.Command(executable, arguments...)
	command.Env = append(environment,
		seatruntime.ReadyFDSetting+"=3",
		testCatalogSetting+"="+catalogPath,
		testOwnerSetting+"="+strconv.Itoa(os.Getuid()),
		testSecretSetting+"=must-not-reach-provider",
	)
	command.ExtraFiles = []*os.File{readyWriter}
	command.Stdin = nil
	command.Stdout = io.Discard
	command.Stderr = io.Discard
	command.SysProcAttr = &syscall.SysProcAttr{Setpgid: true}
	if err := command.Start(); err != nil {
		_ = readyReader.Close()
		_ = readyWriter.Close()
		t.Fatal(err)
	}
	_ = readyWriter.Close()
	return &dispatchedHelper{
		command: command,
		ready: bufio.NewReader(io.LimitReader(
			readyReader,
			64,
		)),
	}
}

func readHelperReady(helper *dispatchedHelper, timeout time.Duration) (string, error) {
	result := make(chan struct {
		record string
		err    error
	}, 1)
	go func() {
		record, err := helper.ready.ReadString('\n')
		result <- struct {
			record string
			err    error
		}{record: record, err: err}
	}()
	select {
	case received := <-result:
		return received.record, received.err
	case <-time.After(timeout):
		return "", errors.New("runtime helper readiness timed out")
	}
}

func waitCommand(t *testing.T, command *exec.Cmd) error {
	t.Helper()
	done := make(chan error, 1)
	go func() { done <- command.Wait() }()
	select {
	case err := <-done:
		return err
	case <-time.After(2 * time.Second):
		t.Fatal("runtime helper did not exit")
		return nil
	}
}

func stopHelper(t *testing.T, helper *dispatchedHelper) {
	t.Helper()
	if err := syscall.Kill(-helper.command.Process.Pid, syscall.SIGTERM); err != nil &&
		!errors.Is(err, syscall.ESRCH) {
		t.Fatal(err)
	}
	if err := waitCommand(t, helper.command); err != nil {
		var exit *exec.ExitError
		if !errors.As(err, &exit) || exit.ExitCode() != -1 {
			t.Fatalf("runtime provider did not stop cleanly: %v", err)
		}
	}
}

func waitFile(t *testing.T, path string) []byte {
	t.Helper()
	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		content, err := os.ReadFile(path)
		if err == nil && len(content) != 0 {
			return content
		}
		time.Sleep(time.Millisecond)
	}
	t.Fatalf("runtime provider did not write %s", filepath.Base(path))
	return nil
}

func waitProcessAbsent(t *testing.T, pid int) {
	t.Helper()
	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		if err := syscall.Kill(pid, 0); errors.Is(err, syscall.ESRCH) {
			return
		}
		time.Sleep(time.Millisecond)
	}
	t.Fatalf("runtime provider descendant %d remained alive", pid)
}

func sessionBusRequest(namespace string) seatruntime.Request {
	return seatruntime.Request{
		Stage: seatruntime.StageSessionBus, RuntimeNamespace: namespace,
	}
}

func TestRuntimeHelperExecsProviderInPlaceAndOwnsDescendants(t *testing.T) {
	temporary := t.TempDir()
	eventFile := filepath.Join(temporary, "provider-events")
	childPIDFile := filepath.Join(temporary, "child-pid")
	marker := filepath.Join(temporary, "shell-executed")
	executable, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	catalogPath := writeTestCatalog(t, []seatruntime.Provider{{
		Stage: seatruntime.StageSessionBus, Executable: executable,
		Arguments: []string{
			eventFile,
			childPIDFile,
			"ready",
			"$(touch " + marker + ")",
		},
	}}, 0o400)
	helper := startDispatchedHelper(t, sessionBusRequest("seat-one"), catalogPath)
	record, err := readHelperReady(helper, 2*time.Second)
	if err != nil || record != seatruntime.ReadyRecord {
		t.Fatalf("provider readiness mismatch: %q, %v", record, err)
	}
	events := string(waitFile(t, eventFile))
	if !strings.Contains(events, "pid="+strconv.Itoa(helper.command.Process.Pid)+"\n") {
		t.Fatalf("dispatcher did not preserve its PID across exec: %q", events)
	}
	if !strings.Contains(events, "$(touch "+marker+")") {
		t.Fatalf("catalog argument was not preserved literally: %q", events)
	}
	if strings.Contains(events, "must-not-reach-provider") ||
		strings.Contains(events, testCatalogSetting) ||
		strings.Contains(events, testOwnerSetting) {
		t.Fatalf("dispatcher leaked test or ambient authority: %q", events)
	}
	if _, err := os.Stat(marker); !errors.Is(err, os.ErrNotExist) {
		t.Fatalf("catalog argument was evaluated by a shell: %v", err)
	}
	childText := string(waitFile(t, childPIDFile))
	childPID, err := strconv.Atoi(childText)
	if err != nil {
		t.Fatal(err)
	}
	if err := syscall.Kill(childPID, 0); err != nil {
		t.Fatalf("provider descendant was not alive before cleanup: %v", err)
	}
	stopHelper(t, helper)
	waitProcessAbsent(t, childPID)
}

func TestRuntimeHelperReadinessFailureLeavesOneCleanableGroup(t *testing.T) {
	temporary := t.TempDir()
	eventFile := filepath.Join(temporary, "provider-events")
	childPIDFile := filepath.Join(temporary, "child-pid")
	executable, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	catalogPath := writeTestCatalog(t, []seatruntime.Provider{{
		Stage: seatruntime.StageSessionBus, Executable: executable,
		Arguments: []string{eventFile, childPIDFile, "invalid-ready"},
	}}, 0o400)
	helper := startDispatchedHelper(t, sessionBusRequest("seat-partial"), catalogPath)
	record, err := readHelperReady(helper, 2*time.Second)
	if err != nil || record == seatruntime.ReadyRecord {
		t.Fatalf("invalid provider readiness was not observable: %q, %v", record, err)
	}
	childPID, err := strconv.Atoi(string(waitFile(t, childPIDFile)))
	if err != nil {
		t.Fatal(err)
	}
	stopHelper(t, helper)
	waitProcessAbsent(t, childPID)
}

func TestRuntimeHelpersKeepSeatProcessGroupsIndependent(t *testing.T) {
	executable, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	start := func(namespace string) (*dispatchedHelper, int) {
		temporary := t.TempDir()
		eventFile := filepath.Join(temporary, "events")
		childPIDFile := filepath.Join(temporary, "child")
		catalog := writeTestCatalog(t, []seatruntime.Provider{{
			Stage: seatruntime.StageSessionBus, Executable: executable,
			Arguments: []string{eventFile, childPIDFile, "ready"},
		}}, 0o400)
		helper := startDispatchedHelper(t, sessionBusRequest(namespace), catalog)
		record, err := readHelperReady(helper, 2*time.Second)
		if err != nil || record != seatruntime.ReadyRecord {
			t.Fatalf("provider readiness mismatch: %q, %v", record, err)
		}
		pid, err := strconv.Atoi(string(waitFile(t, childPIDFile)))
		if err != nil {
			t.Fatal(err)
		}
		return helper, pid
	}
	first, firstChild := start("seat-first")
	second, secondChild := start("seat-second")
	stopHelper(t, first)
	waitProcessAbsent(t, firstChild)
	if err := syscall.Kill(second.command.Process.Pid, 0); err != nil {
		t.Fatalf("stopping one seat terminated another helper: %v", err)
	}
	if err := syscall.Kill(secondChild, 0); err != nil {
		t.Fatalf("stopping one seat terminated another provider tree: %v", err)
	}
	stopHelper(t, second)
	waitProcessAbsent(t, secondChild)
}

func TestRuntimeHelperFailsClosedBeforeProviderExec(t *testing.T) {
	executable, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	for name, setup := range map[string]func(*testing.T) string{
		"missing selection": func(t *testing.T) string {
			return writeTestCatalog(t, []seatruntime.Provider{{
				Stage: seatruntime.StageAudio, Executable: executable,
			}}, 0o400)
		},
		"writable catalog": func(t *testing.T) string {
			return writeTestCatalog(t, []seatruntime.Provider{{
				Stage: seatruntime.StageSessionBus, Executable: executable,
			}}, 0o600)
		},
	} {
		t.Run(name, func(t *testing.T) {
			helper := startDispatchedHelper(t, sessionBusRequest("seat-fail"), setup(t))
			if record, err := readHelperReady(helper, 2*time.Second); err == nil || record != "" {
				t.Fatalf("failed helper produced readiness data: %q, %v", record, err)
			}
			if err := waitCommand(t, helper.command); err == nil {
				t.Fatal("failed helper exited successfully")
			}
		})
	}
}

func TestRuntimeHelperRejectsReadEndAsReadyDescriptor(t *testing.T) {
	executable, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	temporary := t.TempDir()
	eventFile := filepath.Join(temporary, "events")
	childPIDFile := filepath.Join(temporary, "child")
	catalog := writeTestCatalog(t, []seatruntime.Provider{{
		Stage: seatruntime.StageSessionBus, Executable: executable,
		Arguments: []string{eventFile, childPIDFile, "ready"},
	}}, 0o400)
	request := sessionBusRequest("seat-wrong-ready-end")
	arguments, err := seatruntime.Arguments(request)
	if err != nil {
		t.Fatal(err)
	}
	environment, err := seatruntime.Environment(request)
	if err != nil {
		t.Fatal(err)
	}
	readEnd, writeEnd, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	defer writeEnd.Close()
	command := exec.Command(executable, arguments...)
	command.Env = append(environment,
		seatruntime.ReadyFDSetting+"=3",
		testCatalogSetting+"="+catalog,
		testOwnerSetting+"="+strconv.Itoa(os.Getuid()),
	)
	command.ExtraFiles = []*os.File{readEnd}
	command.Stdin = nil
	command.Stdout = io.Discard
	command.Stderr = io.Discard
	if err := command.Start(); err != nil {
		_ = readEnd.Close()
		t.Fatal(err)
	}
	_ = readEnd.Close()
	if err := waitCommand(t, command); err == nil {
		t.Fatal("read-only readiness descriptor was accepted")
	}
	if _, err := os.Stat(eventFile); !errors.Is(err, os.ErrNotExist) {
		t.Fatalf("provider ran with an invalid readiness descriptor: %v", err)
	}
}
