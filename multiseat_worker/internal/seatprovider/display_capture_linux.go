//go:build linux

package seatprovider

import (
	"context"
	"errors"
	"io"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"time"

	"github.com/papi-ux/polaris/multiseat_worker/internal/seatinput"
	"github.com/papi-ux/polaris/multiseat_worker/internal/seatruntime"
)

const displayCaptureSocketPrefix = "polaris-capture-"

type displayArtifactCandidate struct {
	sourceSocket string
	sourceLock   string
	mediaSocket  string
}

func RunDisplayCapture(arguments []string, environment []string) error {
	request, err := parseProviderInvocation(
		seatruntime.StageDisplayCapture,
		arguments,
		environment,
	)
	if err != nil {
		return err
	}
	ready, err := openReadinessWriter()
	if err != nil {
		return err
	}
	options := defaultProviderOptions()
	options.startupTimeout = 15 * time.Second
	options, err = normalizeProviderOptions(options)
	if err != nil {
		_ = ready.Close()
		return err
	}
	context, cancel := signalContext()
	defer cancel()
	return runDisplayCapture(context, request, ready, options)
}

func validUnixSocketPath(path string) bool {
	var address syscall.RawSockaddrUnix
	return validAbsolutePath(path) && len(path) < len(address.Path)
}

func greatestCommonDivisor(left uint32, right uint32) uint32 {
	for right != 0 {
		left, right = right, left%right
	}
	return left
}

func displayFrameRate(refreshMillihertz uint32) string {
	divisor := greatestCommonDivisor(refreshMillihertz, 1000)
	return strconv.FormatUint(uint64(refreshMillihertz/divisor), 10) + "/" +
		strconv.FormatUint(uint64(1000/divisor), 10)
}

func displayCaps(request seatruntime.Request, software bool) string {
	prefix := "video/x-raw(memory:DMABuf)"
	if software {
		prefix = "video/x-raw,format=BGRx"
	}
	return prefix +
		",width=" + strconv.FormatUint(uint64(request.DisplayWidth), 10) +
		",height=" + strconv.FormatUint(uint64(request.DisplayHeight), 10) +
		",framerate=" + displayFrameRate(request.DisplayRefreshMillihertz)
}

func displayEnvironment(options providerOptions) []string {
	return []string{
		"LC_ALL=C",
		"HOME=/nonexistent",
		"XDG_CONFIG_HOME=/nonexistent",
		"XDG_CACHE_HOME=/nonexistent",
		"XDG_DATA_HOME=/nonexistent",
		"XDG_RUNTIME_DIR=" + options.runtimeDirectory,
		"GST_REGISTRY=/dev/null",
		"GST_REGISTRY_1_0=/dev/null",
		"GST_PLUGIN_PATH=",
		"GST_PLUGIN_PATH_1_0=" + options.gstPluginPath,
	}
}

func displayProducerArguments(
	request seatruntime.Request,
	mediaSocket string,
	software bool,
) []string {
	renderTarget := request.RenderNode
	if software {
		renderTarget = "software"
	}
	arguments := []string{"-q", "waylanddisplaysrc", "render-node=" + renderTarget, "!"}
	if software {
		// The plugin's CPU buffers do not carry FDs. A real format conversion
		// honors unixfdsink's shared-memory allocation proposal on GStreamer
		// 1.26. The hardware path retains its original DMA-BUF caps unchanged.
		arguments = append(arguments,
			strings.Replace(displayCaps(request, true), "format=BGRx", "format=RGBx", 1),
			"!", "videoconvert", "!",
		)
	}
	return append(arguments,
		displayCaps(request, software), "!", "unixfdsink",
		"socket-path="+mediaSocket,
		"sync=false", "async=false", "enable-last-sample=false", "wait-for-connection=false",
	)
}

func displayProbeArguments(
	request seatruntime.Request,
	mediaSocket string,
	software bool,
) []string {
	return []string{
		"-q",
		"unixfdsrc",
		"socket-path=" + mediaSocket,
		"num-buffers=1",
		"!",
		displayCaps(request, software),
		"!",
		"fakesink",
		"sync=false",
		"async=false",
		"enable-last-sample=false",
	}
}

func validDisplayRenderNodeName(path string) bool {
	const prefix = "/dev/dri/renderD"
	if !strings.HasPrefix(path, prefix) || len(path) == len(prefix) ||
		len(path) > len(prefix)+6 {
		return false
	}
	for _, character := range []byte(strings.TrimPrefix(path, prefix)) {
		if character < '0' || character > '9' {
			return false
		}
	}
	return true
}

func validateDisplayRenderNode(path string) error {
	if !validDisplayRenderNodeName(path) {
		return errors.New("runtime display render node is invalid")
	}
	descriptor, err := syscall.Open(
		path,
		syscall.O_RDWR|syscall.O_NOFOLLOW|syscall.O_NONBLOCK|syscall.O_CLOEXEC,
		0,
	)
	if err != nil {
		return errors.New("runtime display render node is unavailable")
	}
	defer syscall.Close(descriptor)
	var status syscall.Stat_t
	if err := syscall.Fstat(descriptor, &status); err != nil ||
		status.Mode&syscall.S_IFMT != syscall.S_IFCHR {
		return errors.New("runtime display render node is invalid")
	}
	return nil
}

func automaticWaylandArtifact(name string) (string, bool) {
	base := strings.TrimSuffix(name, ".lock")
	if !strings.HasPrefix(base, "wayland-") || len(base) == len("wayland-") {
		return "", false
	}
	for _, character := range []byte(strings.TrimPrefix(base, "wayland-")) {
		if character < '0' || character > '9' {
			return "", false
		}
	}
	if name != base && name != base+".lock" {
		return "", false
	}
	return base, true
}

func rejectExistingDisplayArtifacts(
	runtime *runtimeDirectory,
	targetSocket string,
	mediaSocket string,
) error {
	if runtime == nil || !validUnixSocketPath(targetSocket) ||
		!validUnixSocketPath(mediaSocket) {
		return errors.New("runtime display artifact paths are invalid")
	}
	if err := runtime.verify(); err != nil {
		return err
	}
	entries, err := os.ReadDir(runtime.path)
	if err != nil {
		return errors.New("runtime display artifact set is unavailable")
	}
	targetName := filepath.Base(targetSocket)
	mediaName := filepath.Base(mediaSocket)
	for _, entry := range entries {
		name := entry.Name()
		_, automatic := automaticWaylandArtifact(name)
		if automatic || name == targetName || name == targetName+".lock" ||
			name == mediaName {
			return errors.New("runtime display artifact already exists")
		}
	}
	return nil
}

func validDisplayArtifact(identity artifactIdentity, mode uint32, uid uint32) bool {
	return identity.mode&syscall.S_IFMT == mode && identity.uid == uid &&
		identity.mode&0o077 == 0
}

func findDisplayArtifactCandidate(
	runtime *runtimeDirectory,
	mediaSocket string,
) (displayArtifactCandidate, bool, error) {
	if runtime == nil || !validUnixSocketPath(mediaSocket) {
		return displayArtifactCandidate{}, false,
			errors.New("runtime display artifact probe is invalid")
	}
	if err := runtime.verify(); err != nil {
		return displayArtifactCandidate{}, false, err
	}
	entries, err := os.ReadDir(runtime.path)
	if err != nil {
		return displayArtifactCandidate{}, false,
			errors.New("runtime display artifact set is unavailable")
	}
	type pair struct {
		socket bool
		lock   bool
	}
	automatic := make(map[string]pair)
	for _, entry := range entries {
		base, matched := automaticWaylandArtifact(entry.Name())
		if !matched {
			continue
		}
		current := automatic[base]
		identity, err := lstatIdentity(filepath.Join(runtime.path, entry.Name()))
		if err != nil {
			return displayArtifactCandidate{}, false,
				errors.New("runtime display artifact changed during discovery")
		}
		if entry.Name() == base {
			if current.socket || !validDisplayArtifact(
				identity,
				syscall.S_IFSOCK,
				runtime.uid,
			) {
				return displayArtifactCandidate{}, false,
					errors.New("runtime display socket is invalid")
			}
			current.socket = true
		} else {
			if current.lock || !validDisplayArtifact(
				identity,
				syscall.S_IFREG,
				runtime.uid,
			) {
				return displayArtifactCandidate{}, false,
					errors.New("runtime display lock is invalid")
			}
			current.lock = true
		}
		automatic[base] = current
	}
	if len(automatic) > 1 {
		return displayArtifactCandidate{}, false,
			errors.New("runtime display created multiple Wayland sockets")
	}
	mediaIdentity, mediaError := lstatIdentity(mediaSocket)
	if mediaError != nil && !errors.Is(mediaError, os.ErrNotExist) {
		return displayArtifactCandidate{}, false,
			errors.New("runtime display media socket changed during discovery")
	}
	mediaReady := mediaError == nil
	if mediaReady && !validDisplayArtifact(
		mediaIdentity,
		syscall.S_IFSOCK,
		runtime.uid,
	) {
		return displayArtifactCandidate{}, false,
			errors.New("runtime display media socket is invalid")
	}
	for base, state := range automatic {
		if !state.socket || !state.lock || !mediaReady {
			return displayArtifactCandidate{}, false, nil
		}
		return displayArtifactCandidate{
			sourceSocket: filepath.Join(runtime.path, base),
			sourceLock:   filepath.Join(runtime.path, base+".lock"),
			mediaSocket:  mediaSocket,
		}, true, nil
	}
	return displayArtifactCandidate{}, false, nil
}

func waitForDisplayArtifactCandidate(
	parent context.Context,
	child *managedChild,
	runtime *runtimeDirectory,
	mediaSocket string,
	deadline time.Time,
	interval time.Duration,
) (displayArtifactCandidate, error) {
	for {
		if parent == nil || child == nil {
			return displayArtifactCandidate{},
				errors.New("runtime display artifact wait is invalid")
		}
		if child.exited() {
			return displayArtifactCandidate{},
				errors.New("runtime display exited before readiness")
		}
		candidate, ready, err := findDisplayArtifactCandidate(runtime, mediaSocket)
		if err != nil {
			return displayArtifactCandidate{}, err
		}
		if ready {
			return candidate, nil
		}
		if time.Until(deadline) <= 0 {
			return displayArtifactCandidate{},
				errors.New("runtime display artifact readiness timed out")
		}
		timer := time.NewTimer(interval)
		select {
		case <-parent.Done():
			if !timer.Stop() {
				<-timer.C
			}
			return displayArtifactCandidate{},
				errors.New("runtime display startup was canceled")
		case <-child.done:
			if !timer.Stop() {
				<-timer.C
			}
			return displayArtifactCandidate{},
				errors.New("runtime display exited before readiness")
		case <-timer.C:
		}
	}
}

func prepareDisplayArtifacts(
	runtime *runtimeDirectory,
	candidate displayArtifactCandidate,
	targetSocket string,
) (map[string]artifactIdentity, error) {
	known := make(map[string]artifactIdentity, 4)
	if runtime == nil || !validUnixSocketPath(targetSocket) ||
		!validUnixSocketPath(candidate.sourceSocket) ||
		!validAbsolutePath(candidate.sourceLock) ||
		!validUnixSocketPath(candidate.mediaSocket) {
		return known, errors.New("runtime display artifact preparation is invalid")
	}
	for _, artifact := range []struct {
		path string
		mode uint32
	}{
		{candidate.sourceSocket, syscall.S_IFSOCK},
		{candidate.sourceLock, syscall.S_IFREG},
		{candidate.mediaSocket, syscall.S_IFSOCK},
	} {
		if err := runtime.verify(); err != nil {
			return known, err
		}
		identity, err := runtime.pins.capture(artifact.path)
		if err != nil || !validDisplayArtifact(identity, artifact.mode, runtime.uid) {
			return known, errors.New("runtime display artifact changed before capture")
		}
		known[artifact.path] = identity
	}
	if err := runtime.verify(); err != nil {
		return known, err
	}
	if err := os.Link(candidate.sourceSocket, targetSocket); err != nil {
		return known, errors.New("runtime display socket alias could not be created")
	}
	targetIdentity, err := runtime.pins.capture(targetSocket)
	if err != nil || !sameIdentity(targetIdentity, known[candidate.sourceSocket]) {
		return known, errors.New("runtime display socket alias identity is invalid")
	}
	known[targetSocket] = targetIdentity
	return known, nil
}

func displayArtifactMatchesName(
	path string,
	targetSocket string,
	mediaSocket string,
) bool {
	name := filepath.Base(path)
	_, automatic := automaticWaylandArtifact(name)
	return automatic || path == targetSocket || path == mediaSocket ||
		path == targetSocket+".lock"
}

func verifyDisplayArtifacts(
	runtime *runtimeDirectory,
	known map[string]artifactIdentity,
	targetSocket string,
	mediaSocket string,
) error {
	if runtime == nil || len(known) != 4 {
		return errors.New("runtime display artifact ownership is incomplete")
	}
	if err := runtime.verify(); err != nil {
		return err
	}
	entries, err := os.ReadDir(runtime.path)
	if err != nil {
		return errors.New("runtime display artifact set is unavailable")
	}
	seen := 0
	for _, entry := range entries {
		path := filepath.Join(runtime.path, entry.Name())
		if !displayArtifactMatchesName(path, targetSocket, mediaSocket) {
			continue
		}
		expected, present := known[path]
		identity, err := lstatIdentity(path)
		if !present || err != nil || !sameIdentity(identity, expected) {
			return errors.New("runtime display artifact identity changed")
		}
		seen++
	}
	if seen != len(known) {
		return errors.New("runtime display artifact disappeared")
	}
	return nil
}

func capturePartialDisplayArtifacts(
	runtime *runtimeDirectory,
	targetSocket string,
	mediaSocket string,
) (map[string]artifactIdentity, error) {
	known := make(map[string]artifactIdentity)
	if runtime == nil {
		return known, errors.New("runtime display partial cleanup is invalid")
	}
	if err := runtime.verify(); err != nil {
		return known, err
	}
	entries, err := os.ReadDir(runtime.path)
	if err != nil {
		return known, errors.New("runtime display artifact set is unavailable")
	}
	for _, entry := range entries {
		path := filepath.Join(runtime.path, entry.Name())
		if !displayArtifactMatchesName(path, targetSocket, mediaSocket) {
			continue
		}
		identity, err := runtime.pins.capture(path)
		if err != nil || identity.uid != runtime.uid || identity.mode&0o077 != 0 {
			return known, errors.New("runtime display partial artifact is invalid")
		}
		name := entry.Name()
		mode := uint32(syscall.S_IFSOCK)
		if strings.HasSuffix(name, ".lock") {
			mode = syscall.S_IFREG
		}
		if identity.mode&syscall.S_IFMT != mode {
			return known, errors.New("runtime display partial artifact is invalid")
		}
		known[path] = identity
	}
	if targetIdentity, present := known[targetSocket]; present {
		ownedAlias := false
		for path, identity := range known {
			if path != targetSocket && filepath.Base(path) != filepath.Base(mediaSocket) &&
				identity.mode&syscall.S_IFMT == syscall.S_IFSOCK &&
				sameIdentity(identity, targetIdentity) {
				ownedAlias = true
				break
			}
		}
		if !ownedAlias {
			return known, errors.New("runtime display socket alias is unowned")
		}
	}
	return known, nil
}

func cleanupDisplayArtifacts(
	runtime *runtimeDirectory,
	known map[string]artifactIdentity,
	targetSocket string,
	mediaSocket string,
	allowOwnedCapture bool,
) error {
	if runtime == nil {
		return errors.New("runtime display cleanup is invalid")
	}
	var result error
	if len(known) < 4 && allowOwnedCapture {
		captured, err := capturePartialDisplayArtifacts(runtime, targetSocket, mediaSocket)
		if err != nil {
			// A replacement or malformed artifact must not prevent removal of
			// producer inodes that were already captured before the race. Do not
			// merge the partial set when its ownership proof is incomplete.
			result = errors.Join(result, err)
		} else {
			if known == nil {
				known = make(map[string]artifactIdentity, len(captured))
			}
			for path, identity := range captured {
				if previous, present := known[path]; present &&
					!sameIdentity(previous, identity) {
					result = errors.Join(
						result,
						errors.New("runtime display partial artifact identity changed"),
					)
					continue
				}
				known[path] = identity
			}
		}
	}
	preferred := []string{targetSocket, mediaSocket}
	for path := range known {
		if path != targetSocket && path != mediaSocket &&
			!strings.HasSuffix(path, ".lock") {
			preferred = append(preferred, path)
		}
	}
	for path := range known {
		if strings.HasSuffix(path, ".lock") {
			preferred = append(preferred, path)
		}
	}
	seen := make(map[string]struct{}, len(preferred))
	for _, path := range preferred {
		if _, duplicate := seen[path]; duplicate {
			continue
		}
		seen[path] = struct{}{}
		expected, present := known[path]
		if !present {
			continue
		}
		if err := runtime.verify(); err != nil {
			return err
		}
		identity, err := lstatIdentity(path)
		if errors.Is(err, os.ErrNotExist) {
			continue
		}
		if err != nil || !sameIdentity(identity, expected) {
			result = errors.Join(
				result,
				errors.New("runtime display cleanup found a replacement artifact"),
			)
			continue
		}
		if err := os.Remove(path); err != nil {
			result = errors.Join(
				result,
				errors.New("runtime display artifact could not be removed"),
			)
		}
	}
	if err := runtime.verify(); err != nil {
		return err
	}
	entries, err := os.ReadDir(runtime.path)
	if err != nil {
		return errors.New("runtime display artifact set is unavailable")
	}
	for _, entry := range entries {
		path := filepath.Join(runtime.path, entry.Name())
		if displayArtifactMatchesName(path, targetSocket, mediaSocket) {
			result = errors.Join(
				result,
				errors.New("runtime display cleanup retained an unexpected artifact"),
			)
			break
		}
	}
	return result
}

func waitForWaylandDisplay(
	parent context.Context,
	child *managedChild,
	socketPath string,
	expectation waylandProbeExpectation,
	deadline time.Time,
	options providerOptions,
) error {
	for {
		if parent == nil || child == nil {
			return errors.New("runtime Wayland readiness wait is invalid")
		}
		if child.exited() {
			return errors.New("runtime display exited before readiness")
		}
		remaining := time.Until(deadline)
		if remaining <= 0 {
			return errors.New("runtime Wayland readiness timed out")
		}
		if err := probeWaylandDisplay(
			parent,
			socketPath,
			expectation,
			boundedProbeTimeout(remaining, options.probeTimeout),
		); err == nil {
			return nil
		}
		timer := time.NewTimer(options.probeInterval)
		select {
		case <-parent.Done():
			if !timer.Stop() {
				<-timer.C
			}
			return errors.New("runtime display startup was canceled")
		case <-child.done:
			if !timer.Stop() {
				<-timer.C
			}
			return errors.New("runtime display exited before readiness")
		case <-timer.C:
		}
	}
}

func runDisplayCapture(
	parent context.Context,
	request seatruntime.Request,
	ready io.WriteCloser,
	options providerOptions,
) (result error) {
	if parent == nil || ready == nil || request.Stage != seatruntime.StageDisplayCapture {
		if ready != nil {
			_ = ready.Close()
		}
		return errors.New("runtime display provider is invalid")
	}
	defer func() {
		if ready != nil {
			_ = ready.Close()
		}
	}()
	if _, err := seatruntime.Arguments(request); err != nil ||
		!strings.HasPrefix(request.CaptureWaylandSocket, displayCaptureSocketPrefix) ||
		strings.HasSuffix(request.CaptureWaylandSocket, ".lock") {
		return errors.New("runtime display request is invalid")
	}
	if request.DisplayHDR {
		return errors.New("runtime display HDR is unsupported")
	}
	options, err := normalizeProviderOptions(options)
	if err != nil {
		return err
	}
	select {
	case <-parent.Done():
		return errors.New("runtime display startup was canceled")
	default:
	}
	runtime, err := openRuntimeDirectory(options.runtimeDirectory, options.runtimeOwnerUID)
	if err != nil {
		return err
	}
	defer runtime.close()
	mediaName, err := seatruntime.CaptureMediaSocketName(request.RuntimeNamespace)
	if err != nil {
		return errors.New("runtime display media endpoint is invalid")
	}
	targetSocket := filepath.Join(runtime.path, request.CaptureWaylandSocket)
	mediaSocket := filepath.Join(runtime.path, mediaName)
	if !validUnixSocketPath(targetSocket) || !validUnixSocketPath(targetSocket+".lock") ||
		!validUnixSocketPath(mediaSocket) {
		return errors.New("runtime display socket path is too long")
	}
	if err := rejectExistingDisplayArtifacts(runtime, targetSocket, mediaSocket); err != nil {
		return err
	}
	if !options.softwareDisplay {
		if err := validateDisplayRenderNode(request.RenderNode); err != nil {
			return err
		}
	}
	var inputs *seatinput.Set
	arguments := displayProducerArguments(request, mediaSocket, options.softwareDisplay)
	if request.InputSeat != "" {
		inputs, err = seatinput.Open(seatinput.Directory, request.InputSeat)
		if err != nil {
			return err
		}
		defer inputs.Close()
		// Insert properties before the first pipeline separator. Every value
		// comes from verified fixed aliases, never from provider catalog argv.
		arguments = append(append(append([]string{}, arguments[:3]...), inputs.CompositorArguments()...), arguments[3:]...)
	}
	environment := displayEnvironment(options)
	child, err := startManagedChildWithUmask(
		options.gstLaunchPath,
		options.executableOwnerUID,
		arguments,
		environment,
		nil,
		0o077,
	)
	if err != nil {
		return err
	}
	var artifacts map[string]artifactIdentity
	readyPublished := false
	defer func() {
		stopError := child.stop(options.stopTimeout)
		cleanupError := cleanupDisplayArtifacts(
			runtime,
			artifacts,
			targetSocket,
			mediaSocket,
			!readyPublished,
		)
		result = errors.Join(result, stopError, cleanupError)
	}()
	deadline := time.Now().Add(options.startupTimeout)
	candidate, err := waitForDisplayArtifactCandidate(
		parent,
		child,
		runtime,
		mediaSocket,
		deadline,
		options.probeInterval,
	)
	if err != nil {
		return err
	}
	artifacts, err = prepareDisplayArtifacts(runtime, candidate, targetSocket)
	if err != nil {
		return err
	}
	if err := waitForWaylandDisplay(
		parent,
		child,
		targetSocket,
		waylandProbeExpectation{
			width:         request.DisplayWidth,
			height:        request.DisplayHeight,
			refresh:       request.DisplayRefreshMillihertz,
			requireDMABuf: !options.softwareDisplay,
			peerPID:       child.command.Process.Pid,
			peerUID:       options.runtimeOwnerUID,
		},
		deadline,
		options,
	); err != nil {
		return err
	}
	remaining := time.Until(deadline)
	if remaining <= 0 {
		return errors.New("runtime display frame readiness timed out")
	}
	if _, err := runTrustedCommand(
		options.gstLaunchPath,
		options.executableOwnerUID,
		displayProbeArguments(request, mediaSocket, options.softwareDisplay),
		environment,
		remaining,
	); err != nil {
		return errors.New("runtime display frame transport is unavailable")
	}
	if child.exited() {
		return errors.New("runtime display exited before readiness")
	}
	if err := verifyDisplayArtifacts(runtime, artifacts, targetSocket, mediaSocket); err != nil {
		return err
	}
	if inputs != nil {
		if child.pidFD == nil {
			return errors.New("runtime input consumer lifetime unavailable")
		}
		if err := inputs.VerifyConsumer(child.command.Process.Pid, int(child.pidFD.Fd())); err != nil {
			return err
		}
	}
	if child.exited() {
		return errors.New("runtime display exited during input verification")
	}
	if err := publishReadiness(ready); err != nil {
		return err
	}
	readyPublished = true
	ready = nil
	ticker := time.NewTicker(time.Second)
	defer ticker.Stop()
	for {
		select {
		case <-parent.Done():
			return nil
		case <-child.done:
			return errors.New("runtime display exited unexpectedly")
		case <-ticker.C:
			if inputs != nil {
				if err := inputs.VerifyConsumer(child.command.Process.Pid, int(child.pidFD.Fd())); err != nil {
					return err
				}
			}
		}
	}
}
