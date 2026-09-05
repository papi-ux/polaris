//go:build linux

package seatprovider

import (
	"context"
	"errors"
	"io"
	"os"
	"path/filepath"
	"sync"
	"syscall"
	"testing"
	"time"

	"github.com/papi-ux/polaris/multiseat_worker/internal/seatruntime"
)

type runningRealProvider struct {
	cancel       context.CancelFunc
	done         chan error
	ready        *os.File
	stopOnce     sync.Once
	stopComplete chan struct{}
	stopError    error
}

func requireTrustedBinary(t *testing.T, path string, expectedOwnerUID uint32) {
	t.Helper()
	if _, err := os.Stat(path); errors.Is(err, os.ErrNotExist) {
		t.Skipf("real provider dependency %s is unavailable", filepath.Base(path))
	} else if err != nil {
		t.Fatal(err)
	}
	executable, err := openTrustedExecutable(path, expectedOwnerUID)
	if err != nil {
		t.Fatalf("real provider dependency %s is untrusted: %v", filepath.Base(path), err)
	}
	_ = executable.Close()
}

func realProviderOptions(t *testing.T, runtimePath string, audio bool) providerOptions {
	t.Helper()
	options := defaultProviderOptions()
	options.runtimeDirectory = runtimePath
	options.runtimeOwnerUID = uint32(os.Geteuid())
	requireTrustedBinary(t, options.dbusDaemonPath, options.executableOwnerUID)
	if audio {
		for _, path := range []string{
			options.pipeWirePath,
			options.pipeWirePulsePath,
			options.pwCLIPath,
			options.pactlPath,
		} {
			requireTrustedBinary(t, path, options.executableOwnerUID)
		}
	}
	return options
}

func startRealProvider(
	t *testing.T,
	run func(context.Context, io.WriteCloser) error,
) *runningRealProvider {
	t.Helper()
	reader, writer, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	context, cancel := context.WithCancel(context.Background())
	done := make(chan error, 1)
	go func() { done <- run(context, writer) }()
	record, err := readBoundedLine(reader, 8*time.Second, 64)
	if err != nil || record != seatruntime.ReadyRecord {
		cancel()
		select {
		case providerError := <-done:
			t.Fatalf("real provider did not become ready: %q, %v, %v", record, err, providerError)
		case <-time.After(4 * time.Second):
			t.Fatalf("real provider startup hung: %q, %v", record, err)
		}
	}
	provider := &runningRealProvider{
		cancel:       cancel,
		done:         done,
		ready:        reader,
		stopComplete: make(chan struct{}),
	}
	t.Cleanup(func() { stopRealProvider(t, provider) })
	return provider
}

func stopRealProvider(t *testing.T, provider *runningRealProvider) {
	t.Helper()
	provider.stopOnce.Do(func() {
		provider.cancel()
		go func() {
			provider.stopError = <-provider.done
			_ = provider.ready.Close()
			close(provider.stopComplete)
		}()
	})
	select {
	case <-provider.stopComplete:
		if provider.stopError != nil {
			t.Fatalf("real provider cleanup failed: %v", provider.stopError)
		}
	case <-time.After(4 * time.Second):
		t.Fatal("real provider cleanup timed out")
	}
}

func requireEmptyRuntime(t *testing.T, path string) {
	t.Helper()
	entries, err := os.ReadDir(path)
	if err != nil {
		t.Fatal(err)
	}
	if len(entries) != 0 {
		names := make([]string, 0, len(entries))
		for _, entry := range entries {
			names = append(names, entry.Name())
		}
		t.Fatalf("private runtime retained artifacts: %v", names)
	}
}

func TestRealSessionBusAuthenticatesAndCleansUp(t *testing.T) {
	runtimePath := privateRuntimeDirectoryForTest(t)
	options := realProviderOptions(t, runtimePath, false)
	request := seatruntime.Request{
		Stage:            seatruntime.StageSessionBus,
		RuntimeNamespace: "real-session-bus",
	}
	provider := startRealProvider(t, func(context context.Context, ready io.WriteCloser) error {
		return runSessionBus(context, request, ready, options)
	})
	if identity, err := lstatIdentity(filepath.Join(runtimePath, "bus")); err != nil ||
		identity.mode&syscall.S_IFMT != syscall.S_IFSOCK {
		stopRealProvider(t, provider)
		t.Fatalf("real session bus socket is unavailable: %#v, %v", identity, err)
	}
	stopRealProvider(t, provider)
	requireEmptyRuntime(t, runtimePath)
}

func audioRequest(namespace string, sink string) seatruntime.Request {
	return seatruntime.Request{
		Stage:            seatruntime.StageAudio,
		RuntimeNamespace: namespace,
		AudioSink:        sink,
	}
}

func startRealAudio(
	t *testing.T,
	options providerOptions,
	request seatruntime.Request,
) *runningRealProvider {
	t.Helper()
	return startRealProvider(t, func(context context.Context, ready io.WriteCloser) error {
		return runAudio(context, request, ready, options)
	})
}

func TestRealPrivateAudioGraphRoutesExactlyAndCleansUp(t *testing.T) {
	runtimePath := privateRuntimeDirectoryForTest(t)
	options := realProviderOptions(t, runtimePath, true)
	request := audioRequest("real-audio", "polaris-real-audio")
	provider := startRealAudio(t, options, request)
	if err := probeAudioGraph(
		options,
		audioEnvironment(runtimePath, request.AudioSink),
		request.AudioSink,
		options.probeTimeout,
	); err != nil {
		stopRealProvider(t, provider)
		t.Fatal(err)
	}
	stopRealProvider(t, provider)
	requireEmptyRuntime(t, runtimePath)
}

func TestRealAudioReadinessFailureCleansPartialArtifacts(t *testing.T) {
	runtimePath := privateRuntimeDirectoryForTest(t)
	options := realProviderOptions(t, runtimePath, true)
	options.pactlPath = "/usr/bin/false"
	requireTrustedBinary(t, options.pactlPath, options.executableOwnerUID)
	options.startupTimeout = 150 * time.Millisecond
	options.probeTimeout = 30 * time.Millisecond
	options.probeInterval = 5 * time.Millisecond
	request := audioRequest("real-audio-failure", "polaris-audio-failure")
	reader, writer, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	defer reader.Close()
	if err := runAudio(context.Background(), request, writer, options); err == nil {
		t.Fatal("audio provider published readiness through a failed Pulse probe")
	}
	requireEmptyRuntime(t, runtimePath)
}

func TestRealPrivateAudioGraphsRemainIndependent(t *testing.T) {
	firstPath := privateRuntimeDirectoryForTest(t)
	secondPath := privateRuntimeDirectoryForTest(t)
	firstOptions := realProviderOptions(t, firstPath, true)
	secondOptions := realProviderOptions(t, secondPath, true)
	firstRequest := audioRequest("real-audio-first", "polaris-audio-first")
	secondRequest := audioRequest("real-audio-second", "polaris-audio-second")
	firstBusRequest := seatruntime.Request{
		Stage:            seatruntime.StageSessionBus,
		RuntimeNamespace: "real-bus-first",
	}
	secondBusRequest := seatruntime.Request{
		Stage:            seatruntime.StageSessionBus,
		RuntimeNamespace: "real-bus-second",
	}
	firstBus := startRealProvider(t, func(context context.Context, ready io.WriteCloser) error {
		return runSessionBus(context, firstBusRequest, ready, firstOptions)
	})
	secondBus := startRealProvider(t, func(context context.Context, ready io.WriteCloser) error {
		return runSessionBus(context, secondBusRequest, ready, secondOptions)
	})
	first := startRealAudio(t, firstOptions, firstRequest)
	second := startRealAudio(t, secondOptions, secondRequest)
	if err := probeAudioGraph(
		firstOptions,
		audioEnvironment(firstPath, firstRequest.AudioSink),
		secondRequest.AudioSink,
		firstOptions.probeTimeout,
	); err == nil {
		stopRealProvider(t, first)
		stopRealProvider(t, firstBus)
		stopRealProvider(t, second)
		stopRealProvider(t, secondBus)
		t.Fatal("one private audio graph exposed the other seat sink")
	}
	stopRealProvider(t, first)
	stopRealProvider(t, firstBus)
	requireEmptyRuntime(t, firstPath)
	if _, err := os.Lstat(filepath.Join(secondPath, "bus")); err != nil {
		stopRealProvider(t, second)
		stopRealProvider(t, secondBus)
		t.Fatalf("stopping one base stack harmed the other session bus: %v", err)
	}
	if err := probeAudioGraph(
		secondOptions,
		audioEnvironment(secondPath, secondRequest.AudioSink),
		secondRequest.AudioSink,
		secondOptions.probeTimeout,
	); err != nil {
		stopRealProvider(t, second)
		stopRealProvider(t, secondBus)
		t.Fatalf("stopping one audio graph harmed the other: %v", err)
	}
	stopRealProvider(t, second)
	stopRealProvider(t, secondBus)
	requireEmptyRuntime(t, secondPath)
}

func realDisplayProviderOptions(t *testing.T, runtimePath string) providerOptions {
	t.Helper()
	options := defaultProviderOptions()
	options.runtimeDirectory = runtimePath
	options.runtimeOwnerUID = uint32(os.Geteuid())
	options.softwareDisplay = true
	if pluginPath := os.Getenv("POLARIS_TEST_GST_PLUGIN_PATH"); pluginPath != "" {
		if !validAbsolutePath(pluginPath) {
			t.Fatalf("real display plugin path is invalid: %s", pluginPath)
		}
		options.gstPluginPath = pluginPath
	}
	requireTrustedBinary(t, options.gstLaunchPath, options.executableOwnerUID)
	requireTrustedBinary(t, options.gstInspectPath, options.executableOwnerUID)
	environment := displayEnvironment(options)
	for _, element := range []string{
		"waylanddisplaysrc",
		"unixfdsink",
		"unixfdsrc",
		"fakesink",
	} {
		if _, err := runTrustedCommand(
			options.gstInspectPath,
			options.executableOwnerUID,
			[]string{element},
			environment,
			3*time.Second,
		); err != nil {
			t.Skipf("real display dependency %s is unavailable", element)
		}
	}
	return options
}

func TestRealDisplayCaptureProducesFrameAndCleansUp(t *testing.T) {
	runtimePath := privateDisplayRuntimeDirectoryForTest(t)
	options := realDisplayProviderOptions(t, runtimePath)
	request := displayRequest("real-display", "polaris-capture-real")
	request.DisplayRefreshMillihertz = 59940
	provider := startRealProvider(t, func(context context.Context, ready io.WriteCloser) error {
		return runDisplayCapture(context, request, ready, options)
	})
	if identity, err := lstatIdentity(
		filepath.Join(runtimePath, request.CaptureWaylandSocket),
	); err != nil || identity.mode&syscall.S_IFMT != syscall.S_IFSOCK ||
		identity.mode&0o077 != 0 {
		stopRealProvider(t, provider)
		t.Fatalf("real display socket is unavailable: %#v, %v", identity, err)
	}
	stopRealProvider(t, provider)
	requireEmptyRuntime(t, runtimePath)
}

func TestRealDisplayCapturesRemainIndependent(t *testing.T) {
	firstPath := privateDisplayRuntimeDirectoryForTest(t)
	secondPath := privateDisplayRuntimeDirectoryForTest(t)
	firstOptions := realDisplayProviderOptions(t, firstPath)
	secondOptions := firstOptions
	secondOptions.runtimeDirectory = secondPath
	firstRequest := displayRequest("real-display-first", "polaris-capture-real-first")
	secondRequest := displayRequest("real-display-second", "polaris-capture-real-second")
	secondRequest.DisplayRefreshMillihertz = 97000
	first := startRealProvider(t, func(context context.Context, ready io.WriteCloser) error {
		return runDisplayCapture(context, firstRequest, ready, firstOptions)
	})
	second := startRealProvider(t, func(context context.Context, ready io.WriteCloser) error {
		return runDisplayCapture(context, secondRequest, ready, secondOptions)
	})
	stopRealProvider(t, first)
	requireEmptyRuntime(t, firstPath)
	secondMediaName, err := seatruntime.CaptureMediaSocketName(secondRequest.RuntimeNamespace)
	if err != nil {
		stopRealProvider(t, second)
		t.Fatal(err)
	}
	if _, err := runTrustedCommand(
		secondOptions.gstLaunchPath,
		secondOptions.executableOwnerUID,
		displayProbeArguments(
			secondRequest,
			filepath.Join(secondPath, secondMediaName),
			true,
		),
		displayEnvironment(secondOptions),
		3*time.Second,
	); err != nil {
		stopRealProvider(t, second)
		t.Fatalf("stopping one real display harmed the other: %v", err)
	}
	stopRealProvider(t, second)
	requireEmptyRuntime(t, secondPath)
}
