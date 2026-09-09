//go:build linux

package seatprovider

import (
	"context"
	"encoding/binary"
	"errors"
	"io"
	"net"
	"os"
	"os/signal"
	"path/filepath"
	"reflect"
	"slices"
	"strconv"
	"strings"
	"syscall"
	"testing"
	"time"

	"github.com/papi-ux/polaris/multiseat_worker/internal/seatruntime"
)

const fakeDisplayFrame = "POLARIS-RAW-FRAME/1\n"

func displayProviderTestInvocation(arguments []string) bool {
	return slices.Contains(arguments, "waylanddisplaysrc") ||
		slices.Contains(arguments, "unixfdsrc")
}

func displayTestEnvironment(environment []string, name string) (string, bool) {
	prefix := name + "="
	for _, setting := range environment {
		if strings.HasPrefix(setting, prefix) {
			return strings.TrimPrefix(setting, prefix), true
		}
	}
	return "", false
}

func displayTestArgument(arguments []string, prefix string) (string, bool) {
	for _, argument := range arguments {
		if strings.HasPrefix(argument, prefix) {
			return strings.TrimPrefix(argument, prefix), true
		}
	}
	return "", false
}

func parseDisplayTestCaps(arguments []string) (uint32, uint32, uint32, bool) {
	for _, argument := range arguments {
		if !strings.HasPrefix(argument, "video/x-raw") {
			continue
		}
		var width uint64
		var height uint64
		var refresh uint64
		var foundWidth bool
		var foundHeight bool
		var foundRefresh bool
		for _, field := range strings.Split(argument, ",") {
			name, value, present := strings.Cut(field, "=")
			if !present {
				continue
			}
			switch name {
			case "width":
				width, _ = strconv.ParseUint(value, 10, 32)
				foundWidth = true
			case "height":
				height, _ = strconv.ParseUint(value, 10, 32)
				foundHeight = true
			case "framerate":
				numerator, denominator, fraction := strings.Cut(value, "/")
				parsedNumerator, numeratorError := strconv.ParseUint(numerator, 10, 32)
				parsedDenominator, denominatorError := strconv.ParseUint(denominator, 10, 32)
				if fraction && numeratorError == nil && denominatorError == nil &&
					parsedDenominator != 0 && parsedNumerator*1000%parsedDenominator == 0 {
					refresh = parsedNumerator * 1000 / parsedDenominator
					foundRefresh = true
				}
			}
		}
		if foundWidth && foundHeight && foundRefresh && width > 0 && height > 0 &&
			refresh > 0 {
			return uint32(width), uint32(height), uint32(refresh), true
		}
	}
	return 0, 0, 0, false
}

func fakeWaylandGlobalPayload(name uint32, interfaceName string, version uint32) []byte {
	payload := appendWaylandUint(nil, name)
	payload, err := appendWaylandString(payload, interfaceName)
	if err != nil {
		return nil
	}
	return appendWaylandUint(payload, version)
}

func serveFakeWaylandConnection(
	connection net.Conn,
	width uint32,
	height uint32,
	refresh uint32,
	mode string,
) {
	defer connection.Close()
	registryID := uint32(0)
	for count := 0; count < 64; count++ {
		message, err := readWaylandMessage(connection)
		if err != nil {
			return
		}
		switch {
		case message.objectID == 1 && message.opcode == 1 && len(message.payload) == 4:
			registryID = binary.NativeEndian.Uint32(message.payload)
			globals := []struct {
				name          uint32
				interfaceName string
				version       uint32
			}{
				{10, "wl_compositor", 6},
				{11, "wl_shm", 1},
				{12, "wl_seat", 9},
				{13, "xdg_wm_base", 6},
				{14, "wl_output", 4},
			}
			if mode != "no-dmabuf" {
				globals = append(globals, struct {
					name          uint32
					interfaceName string
					version       uint32
				}{15, "zwp_linux_dmabuf_v1", 5})
			}
			for _, global := range globals {
				payload := fakeWaylandGlobalPayload(
					global.name,
					global.interfaceName,
					global.version,
				)
				if payload == nil || writeWaylandMessage(connection, registryID, 0, payload) != nil {
					return
				}
			}
		case message.objectID == 1 && message.opcode == 0 && len(message.payload) == 4:
			callbackID := binary.NativeEndian.Uint32(message.payload)
			if writeWaylandMessage(
				connection,
				callbackID,
				0,
				appendWaylandUint(nil, 1),
			) != nil {
				return
			}
		case message.objectID == registryID && message.opcode == 0 && len(message.payload) >= 16:
			outputID := binary.NativeEndian.Uint32(message.payload[len(message.payload)-4:])
			advertisedWidth := width
			if mode == "wrong-mode" {
				advertisedWidth++
			}
			payload := appendWaylandUint(nil, waylandOutputCurrent|0x2)
			payload = appendWaylandUint(payload, advertisedWidth)
			payload = appendWaylandUint(payload, height)
			payload = appendWaylandUint(payload, refresh)
			if writeWaylandMessage(connection, outputID, 1, payload) != nil ||
				writeWaylandMessage(connection, outputID, 2, nil) != nil {
				return
			}
		}
	}
}

func serveFakeWayland(
	listener *net.UnixListener,
	width uint32,
	height uint32,
	refresh uint32,
	mode string,
) {
	for {
		connection, err := listener.Accept()
		if err != nil {
			return
		}
		if mode == "socket-only" {
			_ = connection.Close()
			continue
		}
		go serveFakeWaylandConnection(connection, width, height, refresh, mode)
	}
}

func serveFakeFrames(listener *net.UnixListener, mode string) {
	for {
		connection, err := listener.Accept()
		if err != nil {
			return
		}
		if mode == "bad-frame" {
			_, _ = io.WriteString(connection, "INVALID\n")
		} else {
			_, _ = io.WriteString(connection, fakeDisplayFrame)
		}
		_ = connection.Close()
	}
}

func fakeDisplayProducer(arguments []string, environment []string, mode string) int {
	runtimePath, present := displayTestEnvironment(environment, "XDG_RUNTIME_DIR")
	if !present || !validAbsolutePath(runtimePath) {
		return 91
	}
	mediaSocket, present := displayTestArgument(arguments, "socket-path=")
	if !present || !validUnixSocketPath(mediaSocket) ||
		filepath.Dir(mediaSocket) != runtimePath {
		return 92
	}
	width, height, refresh, validCaps := parseDisplayTestCaps(arguments)
	if !validCaps {
		return 93
	}
	lockPath := filepath.Join(runtimePath, "wayland-0.lock")
	if err := os.WriteFile(lockPath, []byte(strconv.Itoa(os.Getpid())+"\n"), 0o600); err != nil {
		return 94
	}
	waylandListener, err := net.ListenUnix(
		"unix",
		&net.UnixAddr{Name: filepath.Join(runtimePath, "wayland-0"), Net: "unix"},
	)
	if err != nil {
		return 95
	}
	defer waylandListener.Close()
	frameListener, err := net.ListenUnix(
		"unix",
		&net.UnixAddr{Name: mediaSocket, Net: "unix"},
	)
	if err != nil {
		return 96
	}
	defer frameListener.Close()
	if mode == "exit-early" {
		return 97
	}
	go serveFakeWayland(waylandListener, width, height, refresh, mode)
	go serveFakeFrames(frameListener, mode)
	terminated := make(chan os.Signal, 1)
	signal.Notify(terminated, syscall.SIGTERM, syscall.SIGINT)
	defer signal.Stop(terminated)
	<-terminated
	return 0
}

func fakeDisplayProbe(arguments []string) int {
	mediaSocket, present := displayTestArgument(arguments, "socket-path=")
	if !present || !slices.Contains(arguments, "num-buffers=1") ||
		!slices.Contains(arguments, "fakesink") {
		return 98
	}
	connection, err := net.DialTimeout("unix", mediaSocket, time.Second)
	if err != nil {
		return 99
	}
	defer connection.Close()
	if err := connection.SetReadDeadline(time.Now().Add(time.Second)); err != nil {
		return 100
	}
	content, err := io.ReadAll(io.LimitReader(connection, 128))
	if err != nil || string(content) != fakeDisplayFrame {
		return 101
	}
	return 0
}

func displayProviderChildMain(arguments []string, environment []string) int {
	mode := filepath.Base(os.Args[0])
	if slices.Contains(arguments, "waylanddisplaysrc") {
		return fakeDisplayProducer(arguments, environment, mode)
	}
	if slices.Contains(arguments, "unixfdsrc") {
		return fakeDisplayProbe(arguments)
	}
	return 102
}

func copyDisplayProviderTestBinary(t *testing.T, mode string) string {
	t.Helper()
	executable, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	content, err := os.ReadFile(executable)
	if err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(t.TempDir(), mode)
	if err := os.WriteFile(path, content, 0o500); err != nil {
		t.Fatal(err)
	}
	return path
}

func privateDisplayRuntimeDirectoryForTest(t *testing.T) string {
	t.Helper()
	directory, err := os.MkdirTemp("/tmp", "polaris-d-")
	if err != nil {
		t.Fatal(err)
	}
	if err := os.Chmod(directory, 0o700); err != nil {
		_ = os.RemoveAll(directory)
		t.Fatal(err)
	}
	t.Cleanup(func() {
		if err := os.RemoveAll(directory); err != nil {
			t.Errorf("display test runtime cleanup failed: %v", err)
		}
	})
	return directory
}

func displayRequest(namespace string, socket string) seatruntime.Request {
	return seatruntime.Request{
		Stage:                    seatruntime.StageDisplayCapture,
		RuntimeNamespace:         namespace,
		CaptureWaylandSocket:     socket,
		RenderNode:               "/dev/dri/renderD128",
		DisplayTopology:          seatruntime.DisplayTopologyCaptureHostNested,
		MediaPipeline:            seatruntime.MediaPipelineWorkerLocal,
		DisplayWidth:             1920,
		DisplayHeight:            1080,
		DisplayRefreshMillihertz: 60000,
	}
}

func fakeDisplayOptions(t *testing.T, runtimePath string, mode string) providerOptions {
	t.Helper()
	options := defaultProviderOptions()
	options.runtimeDirectory = runtimePath
	options.runtimeOwnerUID = uint32(os.Geteuid())
	options.executableOwnerUID = uint32(os.Geteuid())
	options.gstLaunchPath = copyDisplayProviderTestBinary(t, mode)
	options.gstInspectPath = options.gstLaunchPath
	options.softwareDisplay = true
	options.startupTimeout = 5 * time.Second
	options.probeTimeout = time.Second
	options.stopTimeout = 2 * time.Second
	options.probeInterval = 5 * time.Millisecond
	return options
}

func TestDisplayCapturePipelineIsCanonicalAndPreservesFraction(t *testing.T) {
	request := displayRequest("pipeline-shape", "polaris-capture-pipeline")
	request.DisplayRefreshMillihertz = 59940
	mediaName, err := seatruntime.CaptureMediaSocketName(request.RuntimeNamespace)
	if err != nil {
		t.Fatal(err)
	}
	mediaSocket := filepath.Join("/run/polaris", mediaName)
	expectedProducer := []string{
		"-q", "waylanddisplaysrc", "render-node=/dev/dri/renderD128", "!",
		"video/x-raw(memory:DMABuf),width=1920,height=1080,framerate=2997/50", "!",
		"unixfdsink", "socket-path=" + mediaSocket, "sync=false", "async=false",
		"enable-last-sample=false", "wait-for-connection=false",
	}
	if actual := displayProducerArguments(request, mediaSocket, false); !reflect.DeepEqual(actual, expectedProducer) {
		t.Fatalf("hardware display pipeline changed: %#v", actual)
	}
	expectedProbe := []string{
		"-q", "unixfdsrc", "socket-path=" + mediaSocket, "num-buffers=1", "!",
		"video/x-raw,format=BGRx,width=1920,height=1080,framerate=2997/50", "!",
		"fakesink", "sync=false", "async=false", "enable-last-sample=false",
	}
	if actual := displayProbeArguments(request, mediaSocket, true); !reflect.DeepEqual(actual, expectedProbe) {
		t.Fatalf("software display probe changed: %#v", actual)
	}
}

func TestDisplayEnvironmentIsExactAndHasNoPersistentRegistry(t *testing.T) {
	options := defaultProviderOptions()
	options.runtimeDirectory = "/run/polaris-display-test"
	options.gstPluginPath = "/opt/polaris-test/gstreamer"
	expected := []string{
		"LC_ALL=C",
		"HOME=/nonexistent",
		"XDG_CONFIG_HOME=/nonexistent",
		"XDG_CACHE_HOME=/nonexistent",
		"XDG_DATA_HOME=/nonexistent",
		"XDG_RUNTIME_DIR=/run/polaris-display-test",
		"GST_REGISTRY=/dev/null",
		"GST_REGISTRY_1_0=/dev/null",
		"GST_PLUGIN_PATH=",
		"GST_PLUGIN_PATH_1_0=/opt/polaris-test/gstreamer",
	}
	if actual := displayEnvironment(options); !reflect.DeepEqual(actual, expected) {
		t.Fatalf("display environment expanded or changed: %#v", actual)
	}
}

func TestDisplayRenderNodeRejectsGStreamerGrammar(t *testing.T) {
	for _, path := range []string{
		"/dev/dri/renderD128 ! fakesrc",
		"/dev/dri/renderD128,foo=bar",
		"/dev/dri/by-path/pci-0000:01:00.0-render",
		"/dev/dri/renderD",
		"/dev/dri/renderD128x",
	} {
		if validDisplayRenderNodeName(path) {
			t.Fatalf("GStreamer grammar or non-canonical render node was accepted: %q", path)
		}
	}
	if !validDisplayRenderNodeName("/dev/dri/renderD128") {
		t.Fatal("canonical render node was rejected")
	}
}

func TestDisplayCapturePublishesOnlyAfterProtocolsAndCleansUp(t *testing.T) {
	runtimePath := privateDisplayRuntimeDirectoryForTest(t)
	options := fakeDisplayOptions(t, runtimePath, "good")
	request := displayRequest("display-good", "polaris-capture-good")
	provider := startRealProvider(t, func(context context.Context, ready io.WriteCloser) error {
		return runDisplayCapture(context, request, ready, options)
	})
	mediaName, err := seatruntime.CaptureMediaSocketName(request.RuntimeNamespace)
	if err != nil {
		stopRealProvider(t, provider)
		t.Fatal(err)
	}
	for _, path := range []string{
		filepath.Join(runtimePath, request.CaptureWaylandSocket),
		filepath.Join(runtimePath, mediaName),
	} {
		identity, err := lstatIdentity(path)
		if err != nil || identity.mode&syscall.S_IFMT != syscall.S_IFSOCK ||
			identity.mode&0o077 != 0 {
			stopRealProvider(t, provider)
			t.Fatalf("ready display artifact is invalid: %s, %#v, %v", filepath.Base(path), identity, err)
		}
	}
	stopRealProvider(t, provider)
	requireEmptyRuntime(t, runtimePath)
}

func TestDisplayCaptureRejectsSocketOnlyWrongModeAndBadFrame(t *testing.T) {
	for _, mode := range []string{"socket-only", "wrong-mode", "bad-frame"} {
		t.Run(mode, func(t *testing.T) {
			runtimePath := privateDisplayRuntimeDirectoryForTest(t)
			options := fakeDisplayOptions(t, runtimePath, mode)
			request := displayRequest("display-"+mode, "polaris-capture-"+mode)
			reader, writer, err := os.Pipe()
			if err != nil {
				t.Fatal(err)
			}
			context, cancel := context.WithTimeout(context.Background(), time.Second)
			defer cancel()
			err = runDisplayCapture(context, request, writer, options)
			content, readError := io.ReadAll(reader)
			_ = reader.Close()
			if err == nil || readError != nil || len(content) != 0 {
				t.Fatalf("invalid display reached readiness: %v, %q, %v", err, content, readError)
			}
			requireEmptyRuntime(t, runtimePath)
		})
	}
}

func TestDisplayCaptureRejectsHDRAndPreexistingArtifacts(t *testing.T) {
	runtimePath := privateDisplayRuntimeDirectoryForTest(t)
	options := fakeDisplayOptions(t, runtimePath, "good")
	request := displayRequest("display-hdr", "polaris-capture-hdr")
	request.DisplayHDR = true
	reader, writer, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	if err := runDisplayCapture(context.Background(), request, writer, options); err == nil {
		t.Fatal("HDR display was accepted without an upstream HDR contract")
	}
	_ = reader.Close()
	requireEmptyRuntime(t, runtimePath)

	preexisting := filepath.Join(runtimePath, request.CaptureWaylandSocket)
	if err := os.WriteFile(preexisting, []byte("retain"), 0o600); err != nil {
		t.Fatal(err)
	}
	request.DisplayHDR = false
	reader, writer, err = os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	if err := runDisplayCapture(context.Background(), request, writer, options); err == nil {
		t.Fatal("preexisting display artifact was accepted")
	}
	_ = reader.Close()
	content, err := os.ReadFile(preexisting)
	if err != nil || string(content) != "retain" {
		t.Fatalf("preexisting display artifact changed: %q, %v", content, err)
	}
}

func TestDisplayCaptureRejectsMissingDMABufGlobal(t *testing.T) {
	runtimePath := privateDisplayRuntimeDirectoryForTest(t)
	options := fakeDisplayOptions(t, runtimePath, "no-dmabuf")
	request := displayRequest("display-no-dmabuf", "polaris-capture-no-dmabuf")
	mediaName, err := seatruntime.CaptureMediaSocketName(request.RuntimeNamespace)
	if err != nil {
		t.Fatal(err)
	}
	mediaSocket := filepath.Join(runtimePath, mediaName)
	child, err := startManagedChildWithUmask(
		options.gstLaunchPath,
		options.executableOwnerUID,
		displayProducerArguments(request, mediaSocket, true),
		displayEnvironment(options),
		nil,
		0o077,
	)
	if err != nil {
		t.Fatal(err)
	}
	runtime, err := openRuntimeDirectory(runtimePath, options.runtimeOwnerUID)
	if err != nil {
		_ = child.stop(options.stopTimeout)
		t.Fatal(err)
	}
	defer runtime.close()
	deadline := time.Now().Add(options.startupTimeout)
	candidate, err := waitForDisplayArtifactCandidate(
		context.Background(),
		child,
		runtime,
		mediaSocket,
		deadline,
		options.probeInterval,
	)
	if err != nil {
		_ = child.stop(options.stopTimeout)
		t.Fatal(err)
	}
	known, err := prepareDisplayArtifacts(
		runtime,
		candidate,
		filepath.Join(runtimePath, request.CaptureWaylandSocket),
	)
	if err != nil {
		_ = child.stop(options.stopTimeout)
		t.Fatal(err)
	}
	probeError := probeWaylandDisplay(
		context.Background(),
		filepath.Join(runtimePath, request.CaptureWaylandSocket),
		waylandProbeExpectation{
			width:         request.DisplayWidth,
			height:        request.DisplayHeight,
			refresh:       request.DisplayRefreshMillihertz,
			requireDMABuf: true,
			peerPID:       child.command.Process.Pid,
			peerUID:       options.runtimeOwnerUID,
		},
		options.probeTimeout,
	)
	if probeError == nil {
		_ = child.stop(options.stopTimeout)
		t.Fatal("Wayland display without DMA-BUF support was accepted")
	}
	if err := child.stop(options.stopTimeout); err != nil {
		t.Fatal(err)
	}
	if err := cleanupDisplayArtifacts(
		runtime,
		known,
		filepath.Join(runtimePath, request.CaptureWaylandSocket),
		mediaSocket,
		false,
	); err != nil {
		t.Fatal(err)
	}
	requireEmptyRuntime(t, runtimePath)
}

func TestTwoDisplayCaptureProvidersRemainIndependent(t *testing.T) {
	firstPath := privateDisplayRuntimeDirectoryForTest(t)
	secondPath := privateDisplayRuntimeDirectoryForTest(t)
	firstOptions := fakeDisplayOptions(t, firstPath, "good")
	secondOptions := fakeDisplayOptions(t, secondPath, "good")
	firstRequest := displayRequest("display-first", "polaris-capture-first")
	secondRequest := displayRequest("display-second", "polaris-capture-second")
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
		secondOptions.startupTimeout,
	); err != nil {
		stopRealProvider(t, second)
		t.Fatalf("stopping one display harmed the other: %v", err)
	}
	stopRealProvider(t, second)
	requireEmptyRuntime(t, secondPath)
}

func TestDisplayCleanupRefusesReplacementAlias(t *testing.T) {
	runtimePath := privateDisplayRuntimeDirectoryForTest(t)
	options := fakeDisplayOptions(t, runtimePath, "good")
	request := displayRequest("display-replacement", "polaris-capture-replacement")
	reader, writer, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	context, cancel := context.WithCancel(context.Background())
	done := make(chan error, 1)
	go func() { done <- runDisplayCapture(context, request, writer, options) }()
	record, err := readBoundedLine(reader, 8*time.Second, 64)
	if err != nil || record != seatruntime.ReadyRecord {
		cancel()
		t.Fatalf("display did not become ready: %q, %v", record, err)
	}
	target := filepath.Join(runtimePath, request.CaptureWaylandSocket)
	if err := os.Remove(target); err != nil {
		cancel()
		t.Fatal(err)
	}
	replacement, err := net.ListenUnix("unix", &net.UnixAddr{Name: target, Net: "unix"})
	if err != nil {
		cancel()
		t.Fatal(err)
	}
	replacement.SetUnlinkOnClose(false)
	cancel()
	select {
	case providerError := <-done:
		if providerError == nil || !strings.Contains(providerError.Error(), "replacement") {
			_ = replacement.Close()
			t.Fatalf("replacement alias was not rejected: %v", providerError)
		}
	case <-time.After(4 * time.Second):
		_ = replacement.Close()
		t.Fatal("display replacement cleanup hung")
	}
	if _, err := os.Lstat(target); err != nil {
		_ = replacement.Close()
		t.Fatalf("replacement alias was removed: %v", err)
	}
	entries, err := os.ReadDir(runtimePath)
	if err != nil || len(entries) != 1 || entries[0].Name() != request.CaptureWaylandSocket {
		_ = replacement.Close()
		t.Fatalf("owned artifacts remained beside replacement: %v, %v", entries, err)
	}
	if err := replacement.Close(); err != nil {
		t.Fatal(err)
	}
	if err := os.Remove(target); err != nil && !errors.Is(err, os.ErrNotExist) {
		t.Fatal(err)
	}
	_ = reader.Close()
}

func TestDisplayPartialCleanupRetainsReplacementAndRemovesKnownArtifacts(t *testing.T) {
	runtimePath := privateDisplayRuntimeDirectoryForTest(t)
	runtime, err := openRuntimeDirectory(runtimePath, uint32(os.Geteuid()))
	if err != nil {
		t.Fatal(err)
	}
	defer runtime.close()
	request := displayRequest("display-partial-replacement", "polaris-capture-partial")
	mediaName, err := seatruntime.CaptureMediaSocketName(request.RuntimeNamespace)
	if err != nil {
		t.Fatal(err)
	}
	sourceSocket := filepath.Join(runtimePath, "wayland-0")
	sourceLock := sourceSocket + ".lock"
	mediaSocket := filepath.Join(runtimePath, mediaName)
	targetSocket := filepath.Join(runtimePath, request.CaptureWaylandSocket)
	listen := func(path string) *net.UnixListener {
		t.Helper()
		listener, err := net.ListenUnix(
			"unix",
			&net.UnixAddr{Name: path, Net: "unix"},
		)
		if err != nil {
			t.Fatal(err)
		}
		listener.SetUnlinkOnClose(false)
		if err := os.Chmod(path, 0o600); err != nil {
			_ = listener.Close()
			t.Fatal(err)
		}
		return listener
	}
	source := listen(sourceSocket)
	defer source.Close()
	media := listen(mediaSocket)
	defer media.Close()
	replacement := listen(targetSocket)
	defer replacement.Close()
	if err := os.WriteFile(sourceLock, []byte("owned\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	known := make(map[string]artifactIdentity, 3)
	for _, path := range []string{sourceSocket, sourceLock, mediaSocket} {
		identity, err := lstatIdentity(path)
		if err != nil {
			t.Fatal(err)
		}
		known[path] = identity
	}
	cleanupError := cleanupDisplayArtifacts(
		runtime,
		known,
		targetSocket,
		mediaSocket,
		true,
	)
	if cleanupError == nil || !strings.Contains(cleanupError.Error(), "unowned") {
		t.Fatalf("partial replacement was not reported: %v", cleanupError)
	}
	entries, err := os.ReadDir(runtimePath)
	if err != nil || len(entries) != 1 || entries[0].Name() != request.CaptureWaylandSocket {
		t.Fatalf("known artifacts remained beside partial replacement: %v, %v", entries, err)
	}
	if err := os.Remove(targetSocket); err != nil {
		t.Fatal(err)
	}
}
