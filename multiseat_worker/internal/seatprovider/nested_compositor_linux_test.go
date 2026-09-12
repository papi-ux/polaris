//go:build linux

package seatprovider

import (
	"context"
	"encoding/binary"
	"errors"
	"fmt"
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

func nestedCompositorProviderTestInvocation(arguments []string) bool {
	return len(arguments) >= 2 && arguments[0] == "--backend"
}

func nestedTestArgument(arguments []string, name string) (string, bool) {
	for index := 0; index+1 < len(arguments); index++ {
		if arguments[index] == name {
			return arguments[index+1], true
		}
	}
	return "", false
}

func nestedTestUintEnvironment(environment []string, name string) (uint32, bool) {
	value, present := displayTestEnvironment(environment, name)
	if !present {
		return 0, false
	}
	parsed, err := strconv.ParseUint(value, 10, 32)
	return uint32(parsed), err == nil && parsed > 0
}

func fakeX11SetupPayload(width uint32, height uint32) []byte {
	const vendor = "fake"
	const formatCount = 1
	const screenOffset = 32 + 4 + formatCount*8
	payload := make([]byte, screenOffset+40)
	binary.LittleEndian.PutUint32(payload[0:4], 1)
	binary.LittleEndian.PutUint32(payload[4:8], 0x200000)
	binary.LittleEndian.PutUint32(payload[8:12], 0x1fffff)
	binary.LittleEndian.PutUint16(payload[16:18], uint16(len(vendor)))
	binary.LittleEndian.PutUint16(payload[18:20], 65535)
	payload[20] = 1
	payload[21] = formatCount
	payload[26] = 8
	payload[27] = 255
	copy(payload[32:36], vendor)
	payload[36] = 24
	payload[37] = 32
	payload[38] = 32
	binary.LittleEndian.PutUint32(payload[screenOffset:screenOffset+4], 1)
	binary.LittleEndian.PutUint16(
		payload[screenOffset+20:screenOffset+22],
		uint16(width),
	)
	binary.LittleEndian.PutUint16(
		payload[screenOffset+22:screenOffset+24],
		uint16(height),
	)
	payload[screenOffset+38] = 24
	return payload
}

func serveFakeX11Connection(
	connection net.Conn,
	width uint32,
	height uint32,
	mode string,
) {
	defer connection.Close()
	var request [12]byte
	if _, err := io.ReadFull(connection, request[:]); err != nil ||
		request[0] != 'l' || binary.LittleEndian.Uint16(request[2:4]) != 11 {
		return
	}
	if mode == "socket-only-x" {
		return
	}
	if mode == "wrong-x-mode" {
		width++
	}
	payload := fakeX11SetupPayload(width, height)
	header := make([]byte, 8)
	header[0] = 1
	binary.LittleEndian.PutUint16(header[2:4], 11)
	binary.LittleEndian.PutUint16(header[6:8], uint16(len(payload)/4))
	response := append(header, payload...)
	for len(response) > 0 {
		written, err := connection.Write(response)
		if err != nil || written <= 0 || written > len(response) {
			return
		}
		response = response[written:]
	}
}

func serveFakeX11(
	listener *net.UnixListener,
	width uint32,
	height uint32,
	mode string,
) {
	for {
		connection, err := listener.Accept()
		if err != nil {
			return
		}
		go serveFakeX11Connection(connection, width, height, mode)
	}
}

func fakeGamescopeProducer(arguments []string, environment []string, mode string) int {
	runtimePath, runtimePresent := displayTestEnvironment(environment, "XDG_RUNTIME_DIR")
	parentWayland, parentPresent := displayTestEnvironment(environment, "WAYLAND_DISPLAY")
	readyPath, readyPresent := nestedTestArgument(arguments, "-R")
	limiterPath, limiterPresent := displayTestEnvironment(environment, "GAMESCOPE_LIMITER_FILE")
	x11SocketDirectory, x11SocketPresent := displayTestEnvironment(
		environment,
		"POLARIS_GAMESCOPE_X11_SOCKET_DIRECTORY",
	)
	x11LockDirectory, x11LockPresent := displayTestEnvironment(
		environment,
		"POLARIS_GAMESCOPE_X11_LOCK_DIRECTORY",
	)
	width, widthPresent := nestedTestUintEnvironment(environment, "POLARIS_DISPLAY_WIDTH")
	height, heightPresent := nestedTestUintEnvironment(environment, "POLARIS_DISPLAY_HEIGHT")
	refresh, refreshPresent := nestedTestUintEnvironment(
		environment,
		"POLARIS_DISPLAY_REFRESH_MILLIHZ",
	)
	if !runtimePresent || !parentPresent || !readyPresent || !limiterPresent ||
		!x11SocketPresent || !x11LockPresent || !widthPresent || !heightPresent ||
		!refreshPresent || filepath.Dir(readyPath) != runtimePath ||
		filepath.Dir(limiterPath) != runtimePath || parentWayland == "" {
		return 111
	}
	expectedArguments := gamescopeArguments(
		seatruntime.Request{
			DisplayWidth:             width,
			DisplayHeight:            height,
			DisplayRefreshMillihertz: refresh,
		},
		readyPath,
		true,
	)
	if !slices.Equal(arguments, expectedArguments) {
		return 112
	}
	limiterIdentity, err := lstatIdentity(limiterPath)
	if err != nil || limiterIdentity.mode&syscall.S_IFMT != syscall.S_IFREG ||
		limiterIdentity.mode&0o7777 != 0o600 {
		return 113
	}
	waylandLock := filepath.Join(runtimePath, gamescopeWaylandSocket+".lock")
	if err := os.WriteFile(waylandLock, []byte(strconv.Itoa(os.Getpid())+"\n"), 0o600); err != nil {
		return 114
	}
	waylandPath := filepath.Join(runtimePath, gamescopeWaylandSocket)
	waylandListener, err := net.ListenUnix(
		"unix",
		&net.UnixAddr{Name: waylandPath, Net: "unix"},
	)
	if err != nil {
		return 115
	}
	waylandListener.SetUnlinkOnClose(false)
	x11Path := filepath.Join(x11SocketDirectory, "X0")
	x11Listener, err := net.ListenUnix("unix", &net.UnixAddr{Name: x11Path, Net: "unix"})
	if err != nil {
		return 116
	}
	x11Listener.SetUnlinkOnClose(false)
	x11Lock := filepath.Join(x11LockDirectory, ".X0-lock")
	if err := os.WriteFile(
		x11Lock,
		[]byte(fmt.Sprintf("%10d\x00", os.Getpid())),
		0o400,
	); err != nil {
		return 117
	}
	if mode == "unexpected-artifact" {
		if err := os.WriteFile(
			filepath.Join(runtimePath, "gamescope-surprise"),
			[]byte("retain"),
			0o600,
		); err != nil {
			return 118
		}
	}
	if mode == "exit-early" {
		return 119
	}
	if mode == "replace-limiter" {
		if err := os.WriteFile(limiterPath+".replacement", []byte("retain"), 0o600); err != nil {
			return 122
		}
		if err := os.Rename(limiterPath+".replacement", limiterPath); err != nil {
			return 123
		}
	}
	waylandMode := mode
	if mode == "wrong-wayland-mode" {
		waylandMode = "wrong-mode"
	}
	if mode == "wrong-x-mode" || mode == "socket-only-x" ||
		mode == "bad-ready" || mode == "wrong-x-display" || mode == "replace-limiter" {
		waylandMode = "good"
	}
	go serveFakeWayland(waylandListener, width, height, refresh, waylandMode)
	go serveFakeX11(x11Listener, width, height, mode)
	writer, err := os.OpenFile(readyPath, os.O_WRONLY, 0)
	if err != nil {
		return 120
	}
	record := ":0 " + gamescopeWaylandSocket + "\n"
	if mode == "bad-ready" {
		record = "READY\n"
	} else if mode == "wrong-x-display" {
		record = ":1 " + gamescopeWaylandSocket + "\n"
	}
	if _, err := io.WriteString(writer, record); err != nil || writer.Close() != nil {
		return 121
	}
	terminated := make(chan os.Signal, 1)
	signal.Notify(terminated, syscall.SIGTERM, syscall.SIGINT)
	defer signal.Stop(terminated)
	<-terminated
	return 0
}

func nestedCompositorProviderChildMain(arguments []string, environment []string) int {
	mode := strings.TrimPrefix(filepath.Base(os.Args[0]), "nested-")
	return fakeGamescopeProducer(arguments, environment, mode)
}

func nestedRequestFromDisplay(
	display seatruntime.Request,
	waylandSocket string,
) seatruntime.Request {
	return seatruntime.Request{
		Stage:                    seatruntime.StageNestedCompositor,
		RuntimeNamespace:         display.RuntimeNamespace,
		ParentWaylandSocket:      display.CaptureWaylandSocket,
		WaylandSocket:            waylandSocket,
		RenderNode:               display.RenderNode,
		DisplayWidth:             display.DisplayWidth,
		DisplayHeight:            display.DisplayHeight,
		DisplayRefreshMillihertz: display.DisplayRefreshMillihertz,
		DisplayHDR:               display.DisplayHDR,
		Compositor:               "gamescope",
	}
}

func privateNestedDirectoryForTest(t *testing.T) string {
	t.Helper()
	directory := privateDisplayRuntimeDirectoryForTest(t)
	return directory
}

func fakeNestedOptions(t *testing.T, runtimePath string, mode string) providerOptions {
	t.Helper()
	executable := copyDisplayProviderTestBinary(t, "nested-"+mode)
	options := defaultProviderOptions()
	options.runtimeDirectory = runtimePath
	options.runtimeOwnerUID = uint32(os.Geteuid())
	options.executableOwnerUID = uint32(os.Geteuid())
	options.gamescopePath = executable
	options.xWaylandPath = executable
	options.x11SocketDirectory = privateNestedDirectoryForTest(t)
	options.x11LockDirectory = privateNestedDirectoryForTest(t)
	options.x11DirectoryOwnerUID = uint32(os.Geteuid())
	options.x11DirectoryMode = 0o700
	options.softwareGamescope = true
	options.softwareVulkanICDPath = "/dev/null"
	options.startupTimeout = 2 * time.Second
	options.probeTimeout = 250 * time.Millisecond
	options.stopTimeout = time.Second
	options.probeInterval = 5 * time.Millisecond
	return options
}

func startNestedOuterDisplay(
	t *testing.T,
	runtimePath string,
	display seatruntime.Request,
) (*runningRealProvider, providerOptions) {
	t.Helper()
	options := fakeDisplayOptions(t, runtimePath, "good")
	provider := startRealProvider(t, func(providerContext context.Context, ready io.WriteCloser) error {
		return runDisplayCapture(providerContext, display, ready, options)
	})
	return provider, options
}

func startFakeNestedProvider(
	t *testing.T,
	request seatruntime.Request,
	options providerOptions,
) *runningRealProvider {
	t.Helper()
	return startRealProvider(t, func(providerContext context.Context, ready io.WriteCloser) error {
		return runNestedCompositor(providerContext, request, ready, options)
	})
}

func snapshotDirectoryIdentities(t *testing.T, directory string) map[string]artifactIdentity {
	t.Helper()
	entries, err := os.ReadDir(directory)
	if err != nil {
		t.Fatal(err)
	}
	snapshot := make(map[string]artifactIdentity, len(entries))
	for _, entry := range entries {
		identity, err := lstatIdentity(filepath.Join(directory, entry.Name()))
		if err != nil {
			t.Fatal(err)
		}
		snapshot[entry.Name()] = identity
	}
	return snapshot
}

func requireDirectoryEmpty(t *testing.T, directory string) {
	t.Helper()
	entries, err := os.ReadDir(directory)
	if err != nil || len(entries) != 0 {
		t.Fatalf("directory retained artifacts: %v, %v", entries, err)
	}
}

func TestGamescopeArgumentsPinGeometryBackendAndNoApplication(t *testing.T) {
	request := nestedRequestFromDisplay(
		displayRequest("gamescope-arguments", "polaris-capture-arguments"),
		"polaris-wayland-arguments",
	)
	request.DisplayRefreshMillihertz = 97000
	readyPath := "/run/polaris/polaris-gamescope-ready-test"
	want := []string{
		"--backend", "wayland",
		"--expose-wayland",
		"--xwayland-count", "1",
		"--force-composition",
		"--force-windows-fullscreen",
		"--keep-alive",
		"-W", "1920",
		"-H", "1080",
		"-w", "1920",
		"-h", "1080",
		"-r", "97",
		"-R", readyPath,
	}
	if actual := gamescopeArguments(request, readyPath, false); !reflect.DeepEqual(actual, want) {
		t.Fatalf("Gamescope production argv changed: %#v", actual)
	}
	request.DisplayRefreshMillihertz = 59940
	fractional := gamescopeArguments(request, readyPath, false)
	if slices.Contains(fractional, "-r") || slices.Contains(fractional, "--") {
		t.Fatalf("fractional mode was rounded or an application was injected: %#v", fractional)
	}
	software := gamescopeArguments(request, readyPath, true)
	if len(software) < 2 || software[1] != "headless" {
		t.Fatalf("software-only test backend is not explicit: %#v", software)
	}
}

func TestGamescopeEnvironmentIsScrubbedAndCarriesExactParent(t *testing.T) {
	request := nestedRequestFromDisplay(
		displayRequest("gamescope-environment", "polaris-capture-environment"),
		"polaris-wayland-environment",
	)
	options := defaultProviderOptions()
	readyName, limiterName, sessionName := gamescopeScopedNames(request.RuntimeNamespace)
	paths := gamescopePaths{
		readyFIFO:     filepath.Join(options.runtimeDirectory, readyName),
		limiterFile:   filepath.Join(options.runtimeDirectory, limiterName),
		sessionRecord: filepath.Join(options.runtimeDirectory, sessionName),
	}
	want := []string{
		"PATH=/usr/bin",
		"LC_ALL=C",
		"HOME=/nonexistent",
		"XDG_CONFIG_HOME=/nonexistent",
		"XDG_CACHE_HOME=/nonexistent",
		"XDG_DATA_HOME=/nonexistent",
		"XDG_RUNTIME_DIR=/run/polaris",
		"DBUS_SESSION_BUS_ADDRESS=unix:path=/run/polaris/bus",
		"WAYLAND_DISPLAY=" + request.ParentWaylandSocket,
		"POLARIS_RENDER_NODE=" + request.RenderNode,
		"POLARIS_DISPLAY_WIDTH=1920",
		"POLARIS_DISPLAY_HEIGHT=1080",
		"POLARIS_DISPLAY_REFRESH_MILLIHZ=60000",
		"GAMESCOPE_LIMITER_FILE=" + paths.limiterFile,
		"MESA_SHADER_CACHE_DISABLE=true",
	}
	if actual := gamescopeEnvironment(request, paths, options); !reflect.DeepEqual(actual, want) {
		t.Fatalf("Gamescope production environment changed: %#v", actual)
	}
	options.softwareGamescope = true
	options.softwareVulkanICDPath = "/usr/share/vulkan/icd.d/lvp_icd.x86_64.json"
	software := gamescopeEnvironment(request, paths, options)
	if !slices.Contains(software, "XWAYLAND_NO_GLAMOR=1") {
		t.Fatalf("software Xwayland was allowed to open a GPU: %#v", software)
	}
}

func TestFailedGamescopeCreationIsNeverAdoptedByPartialCleanup(t *testing.T) {
	for _, scenario := range []string{"existing-file", "existing-fifo", "replaced-open-file"} {
		t.Run(scenario, func(t *testing.T) {
			directory := privateNestedDirectoryForTest(t)
			runtime, err := openRuntimeDirectory(directory, uint32(os.Geteuid()))
			if err != nil {
				t.Fatal(err)
			}
			defer runtime.close()
			readyName, limiterName, sessionName := gamescopeScopedNames("failed-creation")
			paths := gamescopePaths{
				readyFIFO: filepath.Join(directory, readyName), limiterFile: filepath.Join(directory, limiterName),
				sessionRecord: filepath.Join(directory, sessionName), targetSocket: filepath.Join(directory, "app-wayland"),
			}
			target := paths.sessionRecord
			var rejected artifactIdentity
			if scenario == "existing-fifo" {
				target = paths.readyFIFO
				if err := syscall.Mkfifo(target, 0o600); err != nil {
					t.Fatal(err)
				}
				_, rejected, err = createGamescopeReadyFIFO(runtime, target)
			} else if scenario == "existing-file" {
				if err := os.WriteFile(target, []byte("replacement"), 0o600); err != nil {
					t.Fatal(err)
				}
				rejected, err = createGamescopeRegularArtifact(runtime, target, nil)
			} else {
				file, openError := os.OpenFile(target, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0o600)
				if openError != nil {
					t.Fatal(openError)
				}
				defer file.Close()
				if err := os.Rename(target, filepath.Join(directory, "original")); err != nil {
					t.Fatal(err)
				}
				if err := os.WriteFile(target, []byte("replacement"), 0o600); err != nil {
					t.Fatal(err)
				}
				rejected, err = captureCreatedArtifact(runtime, target, int(file.Fd()))
			}
			if err == nil {
				t.Fatal("invalid creation was admitted")
			}
			owned, err := createGamescopeRegularArtifact(runtime, paths.limiterFile, nil)
			if err != nil {
				t.Fatal(err)
			}
			known := map[string]artifactIdentity{paths.limiterFile: owned}
			// Match the callers: a zero return would lose the detected rejection.
			if rejected != (artifactIdentity{}) {
				known[target] = rejected
			}
			if err := cleanupGamescopeRuntimeArtifacts(runtime, paths, known, true); err == nil {
				t.Fatal("partial cleanup adopted a rejected creation")
			}
			if _, err := os.Lstat(target); err != nil {
				t.Fatalf("detected replacement was removed: %v", err)
			}
			if _, err := os.Lstat(paths.limiterFile); !errors.Is(err, os.ErrNotExist) {
				t.Fatalf("owned companion artifact was not cleaned: %v", err)
			}
		})
	}
}

func TestGamescopeReadyRecordIsExactAndPrivateX11IsDisplayZero(t *testing.T) {
	valid, err := parseGamescopeReadyRecord(":0 gamescope-0\n", false)
	if err != nil || valid.displayNumber != 0 || valid.displayName != ":0" ||
		valid.waylandName != gamescopeWaylandSocket {
		t.Fatalf("valid Gamescope readiness was rejected: %#v, %v", valid, err)
	}
	if _, err := parseGamescopeReadyRecord(":1 gamescope-0\n", true); err != nil {
		t.Fatalf("shared-X11 lab readiness was rejected: %v", err)
	}
	for _, line := range []string{
		":1 gamescope-0\n",
		":00 gamescope-0\n",
		":0 gamescope-1\n",
		":0 gamescope-0 extra\n",
		":0 gamescope-0",
		"DISPLAY=:0 gamescope-0\n",
	} {
		if _, err := parseGamescopeReadyRecord(line, false); err == nil {
			t.Fatalf("invalid Gamescope readiness was accepted: %q", line)
		}
	}
	if defaultGamescopeStartupTimeout != 120*time.Second {
		t.Fatalf("Gamescope startup timeout regressed: %s", defaultGamescopeStartupTimeout)
	}
}

func TestNestedCompositorPublishesOnlyAfterWaylandX11AndGeometry(t *testing.T) {
	runtimePath := privateDisplayRuntimeDirectoryForTest(t)
	display := displayRequest("nested-good", "polaris-capture-nested-good")
	outer, _ := startNestedOuterDisplay(t, runtimePath, display)
	before := snapshotDirectoryIdentities(t, runtimePath)
	request := nestedRequestFromDisplay(display, "polaris-wayland-nested-good")
	options := fakeNestedOptions(t, runtimePath, "good")
	nested := startFakeNestedProvider(t, request, options)
	_, _, sessionName := gamescopeScopedNames(request.RuntimeNamespace)
	sessionContent, err := os.ReadFile(filepath.Join(runtimePath, sessionName))
	parsedSession, parseErr := parseLauncherSession(sessionContent, request.WaylandSocket)
	if parseErr != nil {
		t.Fatal(parseErr)
	}
	wantSession := gamescopeLauncherRecord(
		gamescopeReadyInfo{displayNumber: 0, displayName: ":0", waylandName: gamescopeWaylandSocket},
		request, parsedSession.pid, parsedSession.cookie,
	)
	if err != nil || !slices.Equal(sessionContent, wantSession) {
		stopRealProvider(t, nested)
		stopRealProvider(t, outer)
		t.Fatalf("Gamescope session record mismatch: %q, %v", sessionContent, err)
	}
	stopRealProvider(t, nested)
	if after := snapshotDirectoryIdentities(t, runtimePath); !reflect.DeepEqual(after, before) {
		stopRealProvider(t, outer)
		t.Fatalf("nested cleanup changed the outer display: before=%v after=%v", before, after)
	}
	requireDirectoryEmpty(t, options.x11SocketDirectory)
	requireDirectoryEmpty(t, options.x11LockDirectory)
	stopRealProvider(t, outer)
	requireEmptyRuntime(t, runtimePath)
}

func TestNestedCompositorPreservesFractionalParentCadence(t *testing.T) {
	runtimePath := privateDisplayRuntimeDirectoryForTest(t)
	display := displayRequest("nested-fractional", "polaris-capture-fractional")
	display.DisplayRefreshMillihertz = 59940
	outer, _ := startNestedOuterDisplay(t, runtimePath, display)
	request := nestedRequestFromDisplay(display, "polaris-wayland-fractional")
	options := fakeNestedOptions(t, runtimePath, "good")
	nested := startFakeNestedProvider(t, request, options)
	stopRealProvider(t, nested)
	stopRealProvider(t, outer)
	requireEmptyRuntime(t, runtimePath)
}

func TestNestedCompositorRejectsProtocolFailuresWithoutHarmingParent(t *testing.T) {
	for _, mode := range []string{
		"bad-ready",
		"wrong-wayland-mode",
		"wrong-x-mode",
		"socket-only",
		"socket-only-x",
		"wrong-x-display",
		"exit-early",
	} {
		t.Run(mode, func(t *testing.T) {
			runtimePath := privateDisplayRuntimeDirectoryForTest(t)
			display := displayRequest("nested-"+mode, "polaris-capture-"+mode)
			outer, _ := startNestedOuterDisplay(t, runtimePath, display)
			before := snapshotDirectoryIdentities(t, runtimePath)
			request := nestedRequestFromDisplay(display, "polaris-wayland-"+mode)
			options := fakeNestedOptions(t, runtimePath, mode)
			reader, writer, err := os.Pipe()
			if err != nil {
				stopRealProvider(t, outer)
				t.Fatal(err)
			}
			providerContext, cancel := context.WithTimeout(context.Background(), 3*time.Second)
			err = runNestedCompositor(providerContext, request, writer, options)
			cancel()
			content, readError := io.ReadAll(reader)
			_ = reader.Close()
			if err == nil || readError != nil || len(content) != 0 {
				stopRealProvider(t, outer)
				t.Fatalf("invalid Gamescope reached readiness: %v, %q, %v", err, content, readError)
			}
			if after := snapshotDirectoryIdentities(t, runtimePath); !reflect.DeepEqual(after, before) {
				stopRealProvider(t, outer)
				t.Fatalf("failed nested cleanup changed the parent: before=%v after=%v", before, after)
			}
			requireDirectoryEmpty(t, options.x11SocketDirectory)
			requireDirectoryEmpty(t, options.x11LockDirectory)
			stopRealProvider(t, outer)
			requireEmptyRuntime(t, runtimePath)
		})
	}
}

func TestNestedCompositorRejectsHDRAndPreexistingArtifacts(t *testing.T) {
	runtimePath := privateDisplayRuntimeDirectoryForTest(t)
	display := displayRequest("nested-preexisting", "polaris-capture-preexisting")
	request := nestedRequestFromDisplay(display, "polaris-wayland-preexisting")
	options := fakeNestedOptions(t, runtimePath, "good")
	request.DisplayHDR = true
	reader, writer, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	if err := runNestedCompositor(context.Background(), request, writer, options); err == nil {
		t.Fatal("nested HDR was accepted without an outer HDR contract")
	}
	_ = reader.Close()
	request.DisplayHDR = false
	preexisting := filepath.Join(runtimePath, request.WaylandSocket)
	if err := os.WriteFile(preexisting, []byte("retain"), 0o600); err != nil {
		t.Fatal(err)
	}
	reader, writer, err = os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	if err := runNestedCompositor(context.Background(), request, writer, options); err == nil {
		t.Fatal("preexisting nested artifact was accepted")
	}
	_ = reader.Close()
	content, err := os.ReadFile(preexisting)
	if err != nil || string(content) != "retain" {
		t.Fatalf("preexisting nested artifact changed: %q, %v", content, err)
	}
}

func TestTwoNestedCompositorStacksRemainIndependent(t *testing.T) {
	firstRuntime := privateDisplayRuntimeDirectoryForTest(t)
	secondRuntime := privateDisplayRuntimeDirectoryForTest(t)
	firstDisplay := displayRequest("nested-first", "polaris-capture-first-nested")
	secondDisplay := displayRequest("nested-second", "polaris-capture-second-nested")
	secondDisplay.DisplayRefreshMillihertz = 97000
	firstOuter, _ := startNestedOuterDisplay(t, firstRuntime, firstDisplay)
	secondOuter, _ := startNestedOuterDisplay(t, secondRuntime, secondDisplay)
	firstRequest := nestedRequestFromDisplay(firstDisplay, "polaris-wayland-first-nested")
	secondRequest := nestedRequestFromDisplay(secondDisplay, "polaris-wayland-second-nested")
	firstOptions := fakeNestedOptions(t, firstRuntime, "good")
	secondOptions := fakeNestedOptions(t, secondRuntime, "good")
	first := startFakeNestedProvider(t, firstRequest, firstOptions)
	second := startFakeNestedProvider(t, secondRequest, secondOptions)
	stopRealProvider(t, first)
	if err := probeWaylandDisplay(
		context.Background(),
		filepath.Join(secondRuntime, secondRequest.WaylandSocket),
		waylandProbeExpectation{
			width: secondRequest.DisplayWidth, height: secondRequest.DisplayHeight,
			refresh: secondRequest.DisplayRefreshMillihertz,
			peerPID: 0,
			peerUID: secondOptions.runtimeOwnerUID,
		},
		secondOptions.probeTimeout,
	); err != nil {
		stopRealProvider(t, second)
		stopRealProvider(t, firstOuter)
		stopRealProvider(t, secondOuter)
		t.Fatalf("stopping one nested compositor harmed the other: %v", err)
	}
	if _, err := os.Lstat(filepath.Join(secondRuntime, secondRequest.WaylandSocket)); err != nil {
		stopRealProvider(t, second)
		t.Fatalf("stopping one nested compositor removed the other: %v", err)
	}
	stopRealProvider(t, second)
	stopRealProvider(t, firstOuter)
	stopRealProvider(t, secondOuter)
	requireEmptyRuntime(t, firstRuntime)
	requireEmptyRuntime(t, secondRuntime)
}

func TestUnexpectedGamescopeArtifactIsRetainedAndReported(t *testing.T) {
	runtimePath := privateDisplayRuntimeDirectoryForTest(t)
	display := displayRequest("nested-unexpected", "polaris-capture-unexpected")
	outer, _ := startNestedOuterDisplay(t, runtimePath, display)
	request := nestedRequestFromDisplay(display, "polaris-wayland-unexpected")
	options := fakeNestedOptions(t, runtimePath, "unexpected-artifact")
	reader, writer, err := os.Pipe()
	if err != nil {
		stopRealProvider(t, outer)
		t.Fatal(err)
	}
	err = runNestedCompositor(context.Background(), request, writer, options)
	_ = reader.Close()
	unexpected := filepath.Join(runtimePath, "gamescope-surprise")
	content, readError := os.ReadFile(unexpected)
	if err == nil || readError != nil || string(content) != "retain" {
		stopRealProvider(t, outer)
		t.Fatalf("unexpected artifact was deleted or ignored: %v, %q, %v", err, content, readError)
	}
	if err := os.Remove(unexpected); err != nil {
		stopRealProvider(t, outer)
		t.Fatal(err)
	}
	stopRealProvider(t, outer)
	requireEmptyRuntime(t, runtimePath)
}

func TestFinalCaptureFailureRetainsReplacementDuringPartialCleanup(t *testing.T) {
	runtimePath := privateDisplayRuntimeDirectoryForTest(t)
	display := displayRequest("final-capture-replacement", "polaris-capture-final-replacement")
	outer, _ := startNestedOuterDisplay(t, runtimePath, display)
	defer stopRealProvider(t, outer)
	before := snapshotDirectoryIdentities(t, runtimePath)
	request := nestedRequestFromDisplay(display, "polaris-wayland-final-replacement")
	options := fakeNestedOptions(t, runtimePath, "replace-limiter")
	reader, writer, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	defer reader.Close()
	providerContext, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	err = runNestedCompositor(providerContext, request, writer, options)
	ready, readError := io.ReadAll(reader)
	_, limiterName, _ := gamescopeScopedNames(request.RuntimeNamespace)
	replacement := filepath.Join(runtimePath, limiterName)
	content, contentError := os.ReadFile(replacement)
	if err == nil || readError != nil || len(ready) != 0 || contentError != nil || string(content) != "retain" {
		t.Fatalf("final capture rejection was lost during cleanup: %v, %q, %v, %q, %v", err, ready, readError, content, contentError)
	}
	if err := os.Remove(replacement); err != nil {
		t.Fatal(err)
	}
	if after := snapshotDirectoryIdentities(t, runtimePath); !reflect.DeepEqual(after, before) {
		t.Fatalf("owned companions were not cleaned independently: before=%v after=%v", before, after)
	}
	requireDirectoryEmpty(t, options.x11SocketDirectory)
	requireDirectoryEmpty(t, options.x11LockDirectory)
}

func TestNestedCleanupRefusesReplacementAlias(t *testing.T) {
	runtimePath := privateDisplayRuntimeDirectoryForTest(t)
	display := displayRequest("nested-replacement", "polaris-capture-nested-replacement")
	outer, _ := startNestedOuterDisplay(t, runtimePath, display)
	before := snapshotDirectoryIdentities(t, runtimePath)
	request := nestedRequestFromDisplay(display, "polaris-wayland-nested-replacement")
	options := fakeNestedOptions(t, runtimePath, "good")
	reader, writer, err := os.Pipe()
	if err != nil {
		stopRealProvider(t, outer)
		t.Fatal(err)
	}
	providerContext, cancel := context.WithCancel(context.Background())
	done := make(chan error, 1)
	go func() {
		done <- runNestedCompositor(providerContext, request, writer, options)
	}()
	record, err := readBoundedLine(reader, 8*time.Second, 64)
	if err != nil || record != seatruntime.ReadyRecord {
		cancel()
		stopRealProvider(t, outer)
		t.Fatalf("nested compositor did not become ready: %q, %v", record, err)
	}
	target := filepath.Join(runtimePath, request.WaylandSocket)
	if err := os.Remove(target); err != nil {
		cancel()
		stopRealProvider(t, outer)
		t.Fatal(err)
	}
	replacement, err := net.ListenUnix("unix", &net.UnixAddr{Name: target, Net: "unix"})
	if err != nil {
		cancel()
		stopRealProvider(t, outer)
		t.Fatal(err)
	}
	replacement.SetUnlinkOnClose(false)
	cancel()
	select {
	case providerError := <-done:
		if providerError == nil || !strings.Contains(providerError.Error(), "replacement") {
			_ = replacement.Close()
			stopRealProvider(t, outer)
			t.Fatalf("replacement nested alias was not rejected: %v", providerError)
		}
	case <-time.After(4 * time.Second):
		_ = replacement.Close()
		stopRealProvider(t, outer)
		t.Fatal("nested replacement cleanup hung")
	}
	after := snapshotDirectoryIdentities(t, runtimePath)
	if _, present := after[request.WaylandSocket]; !present {
		_ = replacement.Close()
		stopRealProvider(t, outer)
		t.Fatal("replacement nested alias was removed")
	}
	delete(after, request.WaylandSocket)
	if !reflect.DeepEqual(after, before) {
		_ = replacement.Close()
		stopRealProvider(t, outer)
		t.Fatalf("owned nested artifacts remained beside replacement: before=%v after=%v", before, after)
	}
	if err := replacement.Close(); err != nil {
		stopRealProvider(t, outer)
		t.Fatal(err)
	}
	if err := os.Remove(target); err != nil && !errors.Is(err, os.ErrNotExist) {
		stopRealProvider(t, outer)
		t.Fatal(err)
	}
	_ = reader.Close()
	requireDirectoryEmpty(t, options.x11SocketDirectory)
	requireDirectoryEmpty(t, options.x11LockDirectory)
	stopRealProvider(t, outer)
	requireEmptyRuntime(t, runtimePath)
}

func TestNestedCompositorRefusesPreoccupiedPrivateX11Namespace(t *testing.T) {
	runtimePath := privateDisplayRuntimeDirectoryForTest(t)
	display := displayRequest("nested-x11-preexisting", "polaris-capture-x11-preexisting")
	outer, _ := startNestedOuterDisplay(t, runtimePath, display)
	beforeRuntime := snapshotDirectoryIdentities(t, runtimePath)
	request := nestedRequestFromDisplay(display, "polaris-wayland-x11-preexisting")
	options := fakeNestedOptions(t, runtimePath, "good")
	socketPath := filepath.Join(options.x11SocketDirectory, "X0")
	lockPath := filepath.Join(options.x11LockDirectory, ".X0-lock")
	listener, err := net.ListenUnix("unix", &net.UnixAddr{Name: socketPath, Net: "unix"})
	if err != nil {
		stopRealProvider(t, outer)
		t.Fatal(err)
	}
	listener.SetUnlinkOnClose(false)
	if err := os.WriteFile(lockPath, []byte("retain\n"), 0o600); err != nil {
		_ = listener.Close()
		stopRealProvider(t, outer)
		t.Fatal(err)
	}
	beforeSockets := snapshotDirectoryIdentities(t, options.x11SocketDirectory)
	beforeLocks := snapshotDirectoryIdentities(t, options.x11LockDirectory)
	reader, writer, err := os.Pipe()
	if err != nil {
		_ = listener.Close()
		stopRealProvider(t, outer)
		t.Fatal(err)
	}
	providerError := runNestedCompositor(context.Background(), request, writer, options)
	_ = reader.Close()
	if providerError == nil || !strings.Contains(providerError.Error(), "not private") {
		_ = listener.Close()
		stopRealProvider(t, outer)
		t.Fatalf("preoccupied private X11 namespace was accepted: %v", providerError)
	}
	if after := snapshotDirectoryIdentities(t, options.x11SocketDirectory); !reflect.DeepEqual(after, beforeSockets) {
		_ = listener.Close()
		stopRealProvider(t, outer)
		t.Fatalf("preexisting X11 socket changed: before=%v after=%v", beforeSockets, after)
	}
	if after := snapshotDirectoryIdentities(t, options.x11LockDirectory); !reflect.DeepEqual(after, beforeLocks) {
		_ = listener.Close()
		stopRealProvider(t, outer)
		t.Fatalf("preexisting X11 lock changed: before=%v after=%v", beforeLocks, after)
	}
	if after := snapshotDirectoryIdentities(t, runtimePath); !reflect.DeepEqual(after, beforeRuntime) {
		_ = listener.Close()
		stopRealProvider(t, outer)
		t.Fatalf("X11 rejection changed the outer runtime: before=%v after=%v", beforeRuntime, after)
	}
	if err := listener.Close(); err != nil {
		stopRealProvider(t, outer)
		t.Fatal(err)
	}
	for _, path := range []string{socketPath, lockPath} {
		if err := os.Remove(path); err != nil && !errors.Is(err, os.ErrNotExist) {
			stopRealProvider(t, outer)
			t.Fatal(err)
		}
	}
	stopRealProvider(t, outer)
	requireEmptyRuntime(t, runtimePath)
}

func TestX11ProbeRejectsWrongGeometryAndPeer(t *testing.T) {
	directory := privateNestedDirectoryForTest(t)
	socketPath := filepath.Join(directory, "X0")
	listener, err := net.ListenUnix("unix", &net.UnixAddr{Name: socketPath, Net: "unix"})
	if err != nil {
		t.Fatal(err)
	}
	defer listener.Close()
	go serveFakeX11(listener, 1280, 720, "good")
	valid := x11ProbeExpectation{
		width: 1280, height: 720, peerPID: os.Getpid(), peerUID: uint32(os.Geteuid()),
	}
	if err := probeX11Display(context.Background(), socketPath, valid, time.Second); err != nil {
		t.Fatalf("valid X11 display was rejected: %v", err)
	}
	wrongGeometry := valid
	wrongGeometry.width++
	if err := probeX11Display(
		context.Background(), socketPath, wrongGeometry, time.Second,
	); err == nil {
		t.Fatal("wrong X11 geometry was accepted")
	}
	wrongPeer := valid
	wrongPeer.peerPID++
	if err := probeX11Display(
		context.Background(), socketPath, wrongPeer, time.Second,
	); err == nil {
		t.Fatal("wrong X11 peer was accepted")
	}
}

func TestFixedDirectoryRejectsSymlinkModeAndIdentityChanges(t *testing.T) {
	root := t.TempDir()
	directoryPath := filepath.Join(root, "x11")
	if err := os.Mkdir(directoryPath, 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.Chmod(directoryPath, 0o700); err != nil {
		t.Fatal(err)
	}
	linkPath := filepath.Join(root, "x11-link")
	if err := os.Symlink(directoryPath, linkPath); err != nil {
		t.Fatal(err)
	}
	if _, err := openFixedDirectory(linkPath, uint32(os.Geteuid()), 0o700); err == nil {
		t.Fatal("fixed-directory admission followed a symlink")
	}
	directory, err := openFixedDirectory(directoryPath, uint32(os.Geteuid()), 0o700)
	if err != nil {
		t.Fatal(err)
	}
	defer directory.close()
	if err := os.Chmod(directoryPath, 0o750); err != nil {
		t.Fatal(err)
	}
	if err := directory.verify(); err == nil {
		t.Fatal("fixed-directory mode change was accepted")
	}
	if err := os.Chmod(directoryPath, 0o700); err != nil {
		t.Fatal(err)
	}
	movedPath := filepath.Join(root, "x11-original")
	if err := os.Rename(directoryPath, movedPath); err != nil {
		t.Fatal(err)
	}
	if err := os.Mkdir(directoryPath, 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.Chmod(directoryPath, 0o700); err != nil {
		t.Fatal(err)
	}
	if err := directory.verify(); err == nil {
		t.Fatal("fixed-directory identity replacement was accepted")
	}
}

func TestGamescopeScopedNamesAreOpaqueAndDeterministic(t *testing.T) {
	firstReady, firstLimiter, firstSession := gamescopeScopedNames("seat-generation-one")
	repeatedReady, repeatedLimiter, repeatedSession := gamescopeScopedNames("seat-generation-one")
	secondReady, secondLimiter, secondSession := gamescopeScopedNames("seat-generation-two")
	if firstReady != repeatedReady || firstLimiter != repeatedLimiter ||
		firstSession != repeatedSession || firstReady == secondReady ||
		firstLimiter == secondLimiter || firstSession == secondSession {
		t.Fatal("Gamescope artifact derivation is not deterministic and namespaced")
	}
	for _, name := range []string{firstReady, firstLimiter, firstSession} {
		if strings.Contains(name, "seat-generation-one") || len(name) > 96 {
			t.Fatalf("Gamescope artifact name leaks authority or is unbounded: %q", name)
		}
	}
}

func TestNestedProviderErrorsDoNotEchoPrivatePaths(t *testing.T) {
	request := nestedRequestFromDisplay(
		displayRequest("nested-error-redaction", "polaris-capture-redaction"),
		"polaris-wayland-redaction",
	)
	options := fakeNestedOptions(t, privateDisplayRuntimeDirectoryForTest(t), "good")
	privatePath := "/private/secret/renderD128"
	request.RenderNode = privatePath
	reader, writer, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	err = runNestedCompositor(context.Background(), request, writer, options)
	_ = reader.Close()
	if err == nil || strings.Contains(err.Error(), privatePath) {
		t.Fatalf("private path leaked through provider error: %v", err)
	}
}

// A helper that refuses to start must say how it finished. Gamescope reports the
// reason on its own stderr, but the provider error carried nothing at all, so a
// missing DRM primary node read only as a readiness timeout.
func TestChildExitDescriptionNamesHowAHelperFinished(t *testing.T) {
	if got := describeChildExit(nil); got != "an unreported status" {
		t.Fatalf("nil child described as %q", got)
	}
	for _, testCase := range []struct {
		path     string
		expected string
	}{
		{path: "/bin/true", expected: "status 0"},
		{path: "/bin/false", expected: "status 1"},
	} {
		child, err := startManagedChild(testCase.path, 0, nil, nil, nil)
		if err != nil {
			t.Fatalf("%s did not start: %v", testCase.path, err)
		}
		select {
		case <-child.done:
		case <-time.After(10 * time.Second):
			_ = child.stop(time.Second)
			t.Fatalf("%s did not exit", testCase.path)
		}
		if got := describeChildExit(child); got != testCase.expected {
			t.Fatalf("%s described as %q, wanted %q", testCase.path, got, testCase.expected)
		}
		_ = child.stop(time.Second)
	}
}
