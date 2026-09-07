//go:build linux

package seatprovider

import (
	"context"
	"errors"
	"io"
	"os"
	"path/filepath"
	"reflect"
	"strconv"
	"strings"
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

// Required image acceptance fails on missing dependencies instead of silently
// turning a provider startup test into a passing job with skipped tests.
func unavailableRealDependency(t *testing.T, format string, args ...any) {
	t.Helper()
	if os.Getenv("POLARIS_TEST_REQUIRE_REAL_PROVIDERS") == "1" {
		t.Fatalf(format, args...)
	}
	t.Skipf(format, args...)
}

func requireTrustedBinary(t *testing.T, path string, expectedOwnerUID uint32) {
	t.Helper()
	if _, err := os.Stat(path); errors.Is(err, os.ErrNotExist) {
		unavailableRealDependency(t, "real provider dependency %s is unavailable", filepath.Base(path))
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
	return startRealProviderWithTimeout(t, 8*time.Second, run)
}

func startRealProviderWithTimeout(
	t *testing.T,
	readinessTimeout time.Duration,
	run func(context.Context, io.WriteCloser) error,
) *runningRealProvider {
	t.Helper()
	if readinessTimeout <= 0 {
		t.Fatal("real provider readiness timeout is invalid")
	}
	reader, writer, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	context, cancel := context.WithCancel(context.Background())
	done := make(chan error, 1)
	go func() { done <- run(context, writer) }()
	record, err := readBoundedLine(reader, readinessTimeout, 64)
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

// Only opt-in real tests consume the explicitly catalogued hardware node.
func realDisplayRequest(namespace, socket string) seatruntime.Request {
	request := displayRequest(namespace, socket)
	if node := os.Getenv("POLARIS_TEST_PROVIDER_RENDER_NODE"); node != "" {
		request.RenderNode = node
	}
	return request
}

func realDisplayProviderOptions(t *testing.T, runtimePath string) providerOptions {
	t.Helper()
	options := defaultProviderOptions()
	options.runtimeDirectory = runtimePath
	options.runtimeOwnerUID = uint32(os.Geteuid())
	options.softwareDisplay = os.Getenv("POLARIS_TEST_PROVIDER_RENDER_NODE") == ""
	if !options.softwareDisplay && os.Getenv("POLARIS_TEST_GAMESCOPE_ALLOW_DRM") != "1" {
		t.Fatal("hardware provider validation needs explicit DRM-device authorization")
	}
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
			unavailableRealDependency(t, "real display dependency %s is unavailable: %v", element, err)
		}
	}
	return options
}

func TestRealDisplayCaptureProducesFrameAndCleansUp(t *testing.T) {
	runtimePath := privateDisplayRuntimeDirectoryForTest(t)
	options := realDisplayProviderOptions(t, runtimePath)
	request := realDisplayRequest("real-display", "polaris-capture-real")
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
	firstRequest := realDisplayRequest("real-display-first", "polaris-capture-real-first")
	secondRequest := realDisplayRequest("real-display-second", "polaris-capture-real-second")
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
			secondOptions.softwareDisplay,
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

func realNestedCompositorOptions(t *testing.T, runtimePath string) providerOptions {
	t.Helper()
	if os.Getenv("POLARIS_TEST_GAMESCOPE_ALLOW_DRM") != "1" {
		unavailableRealDependency(t, "real Gamescope/Xwayland test needs explicit DRM-device authorization")
	}
	options := defaultProviderOptions()
	options.runtimeDirectory = runtimePath
	options.runtimeOwnerUID = uint32(os.Geteuid())
	if node := os.Getenv("POLARIS_TEST_PROVIDER_RENDER_NODE"); node != "" {
		if err := validateDisplayRenderNode(node); err != nil {
			t.Fatal(err)
		}
	} else {
		icdPath := os.Getenv("POLARIS_TEST_GAMESCOPE_SOFTWARE_ICD")
		if icdPath == "" {
			unavailableRealDependency(t, "POLARIS_TEST_GAMESCOPE_SOFTWARE_ICD is not set")
		}
		if !validAbsolutePath(icdPath) {
			t.Fatalf("real Gamescope software ICD path is invalid: %s", icdPath)
		}
		status, err := os.Lstat(icdPath)
		if err != nil || !status.Mode().IsRegular() || status.Mode().Perm()&0o022 != 0 {
			t.Fatalf("real Gamescope software ICD is unavailable or writable: %v", err)
		}
		options.softwareGamescope = true
		options.softwareVulkanICDPath = icdPath
	}
	options.allowSharedX11 = true
	options.startupTimeout = defaultGamescopeStartupTimeout
	options.probeTimeout = 3 * time.Second
	requireTrustedBinary(t, options.gamescopePath, options.executableOwnerUID)
	requireTrustedBinary(t, options.xWaylandPath, options.executableOwnerUID)
	return options
}

func startRealNestedCompositor(
	t *testing.T,
	request seatruntime.Request,
	options providerOptions,
) *runningRealProvider {
	t.Helper()
	return startRealProviderWithTimeout(
		t,
		30*time.Second,
		func(providerContext context.Context, ready io.WriteCloser) error {
			return runNestedCompositor(providerContext, request, ready, options)
		},
	)
}

func readRealGamescopeSession(
	t *testing.T,
	runtimePath string,
	request seatruntime.Request,
) gamescopeReadyInfo {
	t.Helper()
	_, _, sessionName := gamescopeScopedNames(request.RuntimeNamespace)
	sessionPath := filepath.Join(runtimePath, sessionName)
	identity, err := lstatIdentity(sessionPath)
	if err != nil || identity.mode&syscall.S_IFMT != syscall.S_IFREG ||
		identity.uid != uint32(os.Geteuid()) || identity.mode&0o7777 != 0o600 {
		t.Fatalf("real Gamescope session record is invalid: %#v, %v", identity, err)
	}
	content, err := os.ReadFile(sessionPath)
	if err != nil {
		t.Fatal(err)
	}
	lines := strings.Split(string(content), "\n")
	if len(lines) != 6 || lines[0] != strings.TrimSuffix(gamescopeSessionRecordHeader, "\n") ||
		!strings.HasPrefix(lines[1], "DISPLAY=:") ||
		lines[2] != "STEAM_GAME_DISPLAY_0="+strings.TrimPrefix(lines[1], "DISPLAY=") ||
		lines[3] != "WAYLAND_DISPLAY="+request.WaylandSocket ||
		lines[4] != "GAMESCOPE_WAYLAND_DISPLAY="+request.WaylandSocket ||
		lines[5] != "" {
		t.Fatalf("real Gamescope session record is malformed: %q", content)
	}
	display := strings.TrimPrefix(lines[1], "DISPLAY=")
	info, err := parseGamescopeReadyRecord(display+" "+gamescopeWaylandSocket+"\n", true)
	if err != nil {
		t.Fatalf("real Gamescope display record is invalid: %q, %v", display, err)
	}
	return info
}

func realX11Baseline(t *testing.T) map[string]artifactIdentity {
	t.Helper()
	options := defaultProviderOptions()
	sockets, err := openFixedDirectory(
		options.x11SocketDirectory,
		options.x11DirectoryOwnerUID,
		options.x11DirectoryMode,
	)
	if err != nil {
		t.Fatal(err)
	}
	defer sockets.close()
	locks, err := openFixedDirectory(
		options.x11LockDirectory,
		options.x11DirectoryOwnerUID,
		options.x11DirectoryMode,
	)
	if err != nil {
		t.Fatal(err)
	}
	defer locks.close()
	baseline, err := captureX11Baseline(sockets, locks, true)
	if err != nil {
		t.Fatal(err)
	}
	return baseline
}

func runtimeProcessIDs(t *testing.T, runtimePath string) map[int]string {
	t.Helper()
	entries, err := os.ReadDir("/proc")
	if err != nil {
		t.Fatal(err)
	}
	setting := "XDG_RUNTIME_DIR=" + runtimePath
	processes := make(map[int]string)
	for _, entry := range entries {
		pid, err := strconv.Atoi(entry.Name())
		if err != nil || pid <= 0 {
			continue
		}
		environment, err := os.ReadFile(filepath.Join("/proc", entry.Name(), "environ"))
		if err != nil {
			continue
		}
		matched := false
		for _, value := range strings.Split(string(environment), "\x00") {
			if value == setting {
				matched = true
				break
			}
		}
		if !matched {
			continue
		}
		executable, err := os.Readlink(filepath.Join("/proc", entry.Name(), "exe"))
		if err != nil {
			continue
		}
		processes[pid] = filepath.Base(strings.TrimSuffix(executable, " (deleted)"))
	}
	return processes
}

func requireRealGamescopeProcesses(t *testing.T, runtimePath string) map[int]string {
	t.Helper()
	processes := runtimeProcessIDs(t, runtimePath)
	gamescopeSeen := false
	xwaylandSeen := false
	for _, executable := range processes {
		if executable == "gamescope" {
			gamescopeSeen = true
		}
		if executable == "Xwayland" {
			xwaylandSeen = true
		}
	}
	if !gamescopeSeen || !xwaylandSeen {
		t.Fatalf("real Gamescope process tree was not observable for runtime %s: %v", filepath.Base(runtimePath), processes)
	}
	return processes
}

func requireRealX11ArtifactsGone(
	t *testing.T,
	info gamescopeReadyInfo,
	baseline map[string]artifactIdentity,
) {
	t.Helper()
	options := defaultProviderOptions()
	socketPath, lockPath, err := x11DisplayPaths(
		options.x11SocketDirectory,
		options.x11LockDirectory,
		info.displayNumber,
	)
	if err != nil {
		t.Fatal(err)
	}
	for _, path := range []string{socketPath, lockPath} {
		if _, present := baseline[path]; present {
			t.Fatalf("real Gamescope reused a baseline X11 artifact: %s", filepath.Base(path))
		}
		if _, err := os.Lstat(path); !errors.Is(err, os.ErrNotExist) {
			t.Fatalf("real Gamescope retained an X11 artifact: %s, %v", filepath.Base(path), err)
		}
	}
}

func TestRealHeadlessGamescopePublishesBothProtocolsAndCleansUp(t *testing.T) {
	runtimePath := privateDisplayRuntimeDirectoryForTest(t)
	displayOptions := realDisplayProviderOptions(t, runtimePath)
	nestedOptions := realNestedCompositorOptions(t, runtimePath)
	x11Baseline := realX11Baseline(t)
	display := realDisplayRequest("real-nested", "polaris-capture-real-nested")
	display.DisplayWidth = 1280
	display.DisplayHeight = 720
	outer := startRealProvider(t, func(providerContext context.Context, ready io.WriteCloser) error {
		return runDisplayCapture(providerContext, display, ready, displayOptions)
	})
	outerArtifacts := snapshotDirectoryIdentities(t, runtimePath)
	request := nestedRequestFromDisplay(display, "polaris-wayland-real-nested")
	nested := startRealNestedCompositor(t, request, nestedOptions)
	info := readRealGamescopeSession(t, runtimePath, request)
	if err := probeWaylandDisplay(
		context.Background(),
		filepath.Join(runtimePath, request.WaylandSocket),
		waylandProbeExpectation{
			width: request.DisplayWidth, height: request.DisplayHeight,
			refresh: request.DisplayRefreshMillihertz,
			peerPID: 0, peerUID: nestedOptions.runtimeOwnerUID,
		},
		nestedOptions.probeTimeout,
	); err != nil {
		stopRealProvider(t, nested)
		stopRealProvider(t, outer)
		t.Fatalf("real Gamescope Wayland alias is unavailable: %v", err)
	}
	requireRealGamescopeProcesses(t, runtimePath)
	stopRealProvider(t, nested)
	if after := snapshotDirectoryIdentities(t, runtimePath); !reflect.DeepEqual(after, outerArtifacts) {
		stopRealProvider(t, outer)
		t.Fatalf("real Gamescope cleanup changed the outer display: before=%v after=%v", outerArtifacts, after)
	}
	requireRealX11ArtifactsGone(t, info, x11Baseline)
	stopRealProvider(t, outer)
	requireEmptyRuntime(t, runtimePath)
}

func TestTwoRealHeadlessGamescopeStacksRemainIndependent(t *testing.T) {
	firstRuntime := privateDisplayRuntimeDirectoryForTest(t)
	secondRuntime := privateDisplayRuntimeDirectoryForTest(t)
	firstDisplayOptions := realDisplayProviderOptions(t, firstRuntime)
	secondDisplayOptions := realDisplayProviderOptions(t, secondRuntime)
	firstNestedOptions := realNestedCompositorOptions(t, firstRuntime)
	secondNestedOptions := realNestedCompositorOptions(t, secondRuntime)
	x11Baseline := realX11Baseline(t)
	firstDisplay := realDisplayRequest("real-nested-first", "polaris-capture-real-nested-first")
	firstDisplay.DisplayWidth = 1280
	firstDisplay.DisplayHeight = 720
	secondDisplay := realDisplayRequest("real-nested-second", "polaris-capture-real-nested-second")
	secondDisplay.DisplayWidth = 960
	secondDisplay.DisplayHeight = 540
	secondDisplay.DisplayRefreshMillihertz = 97000
	firstOuter := startRealProvider(t, func(providerContext context.Context, ready io.WriteCloser) error {
		return runDisplayCapture(providerContext, firstDisplay, ready, firstDisplayOptions)
	})
	firstRequest := nestedRequestFromDisplay(firstDisplay, "polaris-wayland-real-nested-first")
	first := startRealNestedCompositor(t, firstRequest, firstNestedOptions)
	firstInfo := readRealGamescopeSession(t, firstRuntime, firstRequest)
	secondOuter := startRealProvider(t, func(providerContext context.Context, ready io.WriteCloser) error {
		return runDisplayCapture(providerContext, secondDisplay, ready, secondDisplayOptions)
	})
	secondRequest := nestedRequestFromDisplay(secondDisplay, "polaris-wayland-real-nested-second")
	second := startRealNestedCompositor(t, secondRequest, secondNestedOptions)
	secondInfo := readRealGamescopeSession(t, secondRuntime, secondRequest)
	if firstInfo.displayName == secondInfo.displayName {
		stopRealProvider(t, first)
		stopRealProvider(t, second)
		stopRealProvider(t, firstOuter)
		stopRealProvider(t, secondOuter)
		t.Fatalf("two real Gamescope stacks shared X11 display %s", firstInfo.displayName)
	}
	firstProcesses := requireRealGamescopeProcesses(t, firstRuntime)
	secondProcesses := requireRealGamescopeProcesses(t, secondRuntime)
	for pid := range firstProcesses {
		if _, present := secondProcesses[pid]; present {
			stopRealProvider(t, first)
			stopRealProvider(t, second)
			stopRealProvider(t, firstOuter)
			stopRealProvider(t, secondOuter)
			t.Fatalf("two real Gamescope stacks shared process %d", pid)
		}
	}
	stopRealProvider(t, first)
	stopRealProvider(t, firstOuter)
	requireEmptyRuntime(t, firstRuntime)
	requireRealX11ArtifactsGone(t, firstInfo, x11Baseline)
	if err := probeWaylandDisplay(
		context.Background(),
		filepath.Join(secondRuntime, secondRequest.WaylandSocket),
		waylandProbeExpectation{
			width: secondRequest.DisplayWidth, height: secondRequest.DisplayHeight,
			refresh: secondRequest.DisplayRefreshMillihertz,
			peerPID: 0, peerUID: secondNestedOptions.runtimeOwnerUID,
		},
		secondNestedOptions.probeTimeout,
	); err != nil {
		stopRealProvider(t, second)
		stopRealProvider(t, secondOuter)
		t.Fatalf("stopping one real Gamescope stack harmed the other: %v", err)
	}
	requireRealGamescopeProcesses(t, secondRuntime)
	stopRealProvider(t, second)
	stopRealProvider(t, secondOuter)
	requireEmptyRuntime(t, secondRuntime)
	requireRealX11ArtifactsGone(t, secondInfo, x11Baseline)
}
