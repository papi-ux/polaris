//go:build linux

package seatprovider

import (
	"context"
	"io"
	"os"
	"os/exec"
	"os/signal"
	"path/filepath"
	"reflect"
	"syscall"
	"testing"
	"time"

	"github.com/papi-ux/polaris/multiseat_worker/internal/seatruntime"
)

const providerChildModeSetting = "POLARIS_PROVIDER_CHILD_MODE"

func providerChildMain(mode string) int {
	switch mode {
	case "readiness":
		writer, err := openReadinessWriter()
		if err != nil {
			return 81
		}
		if err := publishReadiness(writer); err != nil {
			return 82
		}
		return 0
	case "descriptor":
		var executable syscall.Stat_t
		if err := syscall.Fstat(3, &executable); err != nil ||
			executable.Mode&syscall.S_IFMT != syscall.S_IFREG {
			return 83
		}
		ready := os.NewFile(4, "provider-child-ready")
		if ready == nil {
			return 84
		}
		defer ready.Close()
		if _, err := io.WriteString(ready, "ready\n"); err != nil {
			return 85
		}
		for {
			time.Sleep(time.Hour)
		}
	case "ignore-term":
		signal.Ignore(syscall.SIGTERM)
		ready := os.NewFile(4, "provider-child-ready")
		if ready == nil {
			return 86
		}
		defer ready.Close()
		if _, err := io.WriteString(ready, "ready\n"); err != nil {
			return 87
		}
		for {
			time.Sleep(time.Hour)
		}
	case "exit":
		return 88
	default:
		return 89
	}
}

func TestMain(tests *testing.M) {
	if mode := os.Getenv(providerChildModeSetting); mode != "" {
		os.Exit(providerChildMain(mode))
	}
	if nestedCompositorProviderTestInvocation(os.Args[1:]) {
		os.Exit(nestedCompositorProviderChildMain(os.Args[1:], os.Environ()))
	}
	if displayProviderTestInvocation(os.Args[1:]) {
		os.Exit(displayProviderChildMain(os.Args[1:], os.Environ()))
	}
	os.Exit(tests.Run())
}

func providerInvocationForTest(t *testing.T, request seatruntime.Request) ([]string, []string) {
	t.Helper()
	arguments, err := seatruntime.Arguments(request)
	if err != nil {
		t.Fatal(err)
	}
	arguments[0] = "serve-resource-v1"
	arguments = append(arguments, "--")
	environment, err := seatruntime.Environment(request)
	if err != nil {
		t.Fatal(err)
	}
	environment = append(environment, seatruntime.ReadyFDSetting+"=3")
	return arguments, environment
}

func privateRuntimeDirectoryForTest(t *testing.T) string {
	t.Helper()
	directory := t.TempDir()
	if err := os.Chmod(directory, 0o700); err != nil {
		t.Fatal(err)
	}
	return directory
}

func TestProviderInvocationIsExactAndStageBound(t *testing.T) {
	request := seatruntime.Request{
		Stage:            seatruntime.StageAudio,
		RuntimeNamespace: "seat-epoch-generation",
		AudioSink:        "polaris-seat-audio",
	}
	arguments, environment := providerInvocationForTest(t, request)
	parsed, err := parseProviderInvocation(
		seatruntime.StageAudio,
		arguments,
		environment,
	)
	if err != nil || parsed != request {
		t.Fatalf("provider invocation did not round trip: %#v, %v", parsed, err)
	}
	if !reflect.DeepEqual(environment, []string{
		"XDG_RUNTIME_DIR=/run/polaris",
		"DBUS_SESSION_BUS_ADDRESS=unix:path=/run/polaris/bus",
		"PIPEWIRE_RUNTIME_DIR=/run/polaris",
		"PIPEWIRE_NODE=polaris-seat-audio",
		"PULSE_SERVER=unix:/run/polaris/pulse/native",
		"PULSE_SINK=polaris-seat-audio",
		seatruntime.ReadyFDSetting + "=3",
	}) {
		t.Fatalf("audio environment lost exact routing: %#v", environment)
	}
	for name, mutate := range map[string]func([]string, []string) ([]string, []string){
		"wrong stage": func(argv []string, env []string) ([]string, []string) {
			return argv, env
		},
		"provider argument": func(argv []string, env []string) ([]string, []string) {
			return append(argv, "extra"), env
		},
		"missing separator": func(argv []string, env []string) ([]string, []string) {
			return argv[:len(argv)-1], env
		},
		"ambient environment": func(argv []string, env []string) ([]string, []string) {
			return argv, append(env, "PATH=/tmp")
		},
	} {
		t.Run(name, func(t *testing.T) {
			argv := append([]string(nil), arguments...)
			env := append([]string(nil), environment...)
			argv, env = mutate(argv, env)
			expected := seatruntime.StageAudio
			if name == "wrong stage" {
				expected = seatruntime.StageSessionBus
			}
			if _, err := parseProviderInvocation(expected, argv, env); err == nil {
				t.Fatal("expanded or mismatched provider authority was accepted")
			}
		})
	}
}

func TestReadinessDescriptorIsValidatedSealedAndClosed(t *testing.T) {
	executable, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	reader, writer, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	command := exec.Command(executable, "-test.run=^$")
	command.Env = []string{providerChildModeSetting + "=readiness"}
	command.ExtraFiles = []*os.File{writer}
	command.Stdout = io.Discard
	command.Stderr = io.Discard
	if err := command.Start(); err != nil {
		_ = reader.Close()
		_ = writer.Close()
		t.Fatal(err)
	}
	_ = writer.Close()
	record, err := readBoundedLine(reader, time.Second, 64)
	if err != nil || record != seatruntime.ReadyRecord {
		t.Fatalf("readiness record mismatch: %q, %v", record, err)
	}
	remaining, err := io.ReadAll(reader)
	_ = reader.Close()
	if err != nil || len(remaining) != 0 {
		t.Fatalf("readiness descriptor remained open: %q, %v", remaining, err)
	}
	if err := command.Wait(); err != nil {
		t.Fatal(err)
	}
}

func TestReadinessDescriptorRejectsThePipeReadEnd(t *testing.T) {
	executable, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	reader, writer, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	defer writer.Close()
	command := exec.Command(executable, "-test.run=^$")
	command.Env = []string{providerChildModeSetting + "=readiness"}
	command.ExtraFiles = []*os.File{reader}
	command.Stdout = io.Discard
	command.Stderr = io.Discard
	if err := command.Start(); err != nil {
		_ = reader.Close()
		t.Fatal(err)
	}
	_ = reader.Close()
	if err := command.Wait(); err == nil {
		t.Fatal("read-only readiness descriptor was accepted")
	}
}

func startProviderChildForTest(t *testing.T, mode string) (*managedChild, *os.File) {
	t.Helper()
	executable, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	reader, writer, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	child, err := startManagedChild(
		executable,
		uint32(os.Getuid()),
		[]string{"-test.run=^$"},
		[]string{providerChildModeSetting + "=" + mode},
		[]*os.File{writer},
	)
	_ = writer.Close()
	if err != nil {
		_ = reader.Close()
		t.Fatal(err)
	}
	line, err := readBoundedLine(reader, time.Second, 64)
	if err != nil || line != "ready\n" {
		_ = child.stop(time.Second)
		_ = reader.Close()
		t.Fatalf("provider child did not start: %q, %v", line, err)
	}
	return child, reader
}

func TestTrustedChildUsesPinnedExecutableAndStops(t *testing.T) {
	child, reader := startProviderChildForTest(t, "descriptor")
	defer reader.Close()
	if child.exited() {
		t.Fatal("trusted child exited before stop")
	}
	if err := child.stop(time.Second); err != nil {
		t.Fatal(err)
	}
	if !child.exited() {
		t.Fatal("trusted child remained alive")
	}
}

func TestManagedChildEscalatesAnIgnoredTermination(t *testing.T) {
	child, reader := startProviderChildForTest(t, "ignore-term")
	defer reader.Close()
	started := time.Now()
	if err := child.stop(30 * time.Millisecond); err != nil {
		t.Fatal(err)
	}
	if elapsed := time.Since(started); elapsed < 25*time.Millisecond {
		t.Fatalf("ignored TERM was not given its bounded grace period: %s", elapsed)
	}
}

func TestManagedChildReportsAnUnexpectedExit(t *testing.T) {
	executable, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	child, err := startManagedChild(
		executable,
		uint32(os.Getuid()),
		[]string{"-test.run=^$"},
		[]string{providerChildModeSetting + "=exit"},
		nil,
	)
	if err != nil {
		t.Fatal(err)
	}
	select {
	case <-child.done:
	case <-time.After(time.Second):
		t.Fatal("unexpected child exit was not reported")
	}
	if !child.exited() {
		t.Fatal("exited child still appeared active")
	}
	if err := child.stop(time.Second); err != nil {
		t.Fatal(err)
	}
}

func TestTrustedExecutableRejectsWritableAndSymlinkedFiles(t *testing.T) {
	executable, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	content, err := os.ReadFile(executable)
	if err != nil {
		t.Fatal(err)
	}
	copyPath := filepath.Join(t.TempDir(), "provider")
	if err := os.WriteFile(copyPath, content, 0o775); err != nil {
		t.Fatal(err)
	}
	if err := os.Chmod(copyPath, 0o775); err != nil {
		t.Fatal(err)
	}
	if _, err := openTrustedExecutable(copyPath, uint32(os.Getuid())); err == nil {
		t.Fatal("group-writable executable was accepted")
	}
	if err := os.Chmod(copyPath, 0o755); err != nil {
		t.Fatal(err)
	}
	linkPath := filepath.Join(t.TempDir(), "provider-link")
	if err := os.Symlink(copyPath, linkPath); err != nil {
		t.Fatal(err)
	}
	if _, err := openTrustedExecutable(linkPath, uint32(os.Getuid())); err == nil {
		t.Fatal("executable symlink was followed")
	}
}

func TestRuntimeDirectoryRequiresExactOwnerModeAndIdentity(t *testing.T) {
	directory := privateRuntimeDirectoryForTest(t)
	runtime, err := openRuntimeDirectory(directory, uint32(os.Getuid()))
	if err != nil {
		t.Fatal(err)
	}
	defer runtime.close()
	if err := os.Chmod(directory, 0o750); err != nil {
		t.Fatal(err)
	}
	if err := runtime.verify(); err == nil {
		t.Fatal("runtime directory mode replacement was accepted")
	}
	if _, err := openRuntimeDirectory(directory, uint32(os.Getuid())); err == nil {
		t.Fatal("over-broad runtime directory was accepted")
	}
	link := filepath.Join(t.TempDir(), "runtime-link")
	if err := os.Symlink(directory, link); err != nil {
		t.Fatal(err)
	}
	if _, err := openRuntimeDirectory(link, uint32(os.Getuid())); err == nil {
		t.Fatal("runtime directory symlink was followed")
	}
}

func TestCleanupRefusesReplacementArtifacts(t *testing.T) {
	directory := privateRuntimeDirectoryForTest(t)
	runtime, err := openRuntimeDirectory(directory, uint32(os.Getuid()))
	if err != nil {
		t.Fatal(err)
	}
	defer runtime.close()
	busPath := filepath.Join(directory, "bus")
	listener, err := syscall.Socket(syscall.AF_UNIX, syscall.SOCK_STREAM, 0)
	if err != nil {
		t.Fatal(err)
	}
	defer syscall.Close(listener)
	if err := syscall.Bind(listener, &syscall.SockaddrUnix{Name: busPath}); err != nil {
		t.Fatal(err)
	}
	original, err := lstatIdentity(busPath)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.Remove(busPath); err != nil {
		t.Fatal(err)
	}
	replacement, err := syscall.Socket(syscall.AF_UNIX, syscall.SOCK_STREAM, 0)
	if err != nil {
		t.Fatal(err)
	}
	defer syscall.Close(replacement)
	if err := syscall.Bind(replacement, &syscall.SockaddrUnix{Name: busPath}); err != nil {
		t.Fatal(err)
	}
	if err := cleanupSessionBus(runtime, map[string]artifactIdentity{"bus": original}, false); err == nil {
		t.Fatal("replacement session bus socket was removed")
	}
	if _, err := os.Lstat(busPath); err != nil {
		t.Fatalf("replacement session bus socket was not retained: %v", err)
	}

	pulsePath := filepath.Join(directory, "pulse")
	if err := os.Mkdir(pulsePath, 0o700); err != nil {
		t.Fatal(err)
	}
	pidPath := filepath.Join(pulsePath, "pid")
	if err := os.WriteFile(pidPath, []byte("1\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	pulseIdentity, err := lstatIdentity(pulsePath)
	if err != nil {
		t.Fatal(err)
	}
	pidIdentity, err := lstatIdentity(pidPath)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.Remove(pidPath); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(pidPath, []byte("2\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	known := map[string]artifactIdentity{
		"pulse":     pulseIdentity,
		"pulse/pid": pidIdentity,
	}
	if err := cleanupAudioArtifacts(runtime, known, false); err == nil {
		t.Fatal("replacement audio artifact was removed")
	}
	content, err := os.ReadFile(pidPath)
	if err != nil || string(content) != "2\n" {
		t.Fatalf("replacement audio artifact was not retained: %q, %v", content, err)
	}
}

func TestPreexistingAudioArtifactFailsBeforeProcessStart(t *testing.T) {
	directory := privateRuntimeDirectoryForTest(t)
	path := filepath.Join(directory, "pipewire-0")
	if err := os.WriteFile(path, []byte("preexisting"), 0o600); err != nil {
		t.Fatal(err)
	}
	request := seatruntime.Request{
		Stage:            seatruntime.StageAudio,
		RuntimeNamespace: "seat-preexisting",
		AudioSink:        "polaris-preexisting",
	}
	reader, writer, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	defer reader.Close()
	options := defaultProviderOptions()
	options.runtimeDirectory = directory
	options.runtimeOwnerUID = uint32(os.Getuid())
	if err := runAudio(context.Background(), request, writer, options); err == nil {
		t.Fatal("preexisting audio artifact was accepted")
	}
	content, err := os.ReadFile(path)
	if err != nil || string(content) != "preexisting" {
		t.Fatalf("preexisting audio artifact changed: %q, %v", content, err)
	}
}
