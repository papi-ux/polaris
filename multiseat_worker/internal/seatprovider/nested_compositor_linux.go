//go:build linux

package seatprovider

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"io"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"time"

	"github.com/papi-ux/polaris/multiseat_worker/internal/seatruntime"
)

const (
	gamescopeWaylandSocket         = "gamescope-0"
	gamescopeReadyNamePrefix       = "polaris-gamescope-ready-"
	gamescopeLimiterNamePrefix     = "polaris-gamescope-limiter-"
	gamescopeSessionNamePrefix     = "polaris-gamescope-session-"
	gamescopeArtifactNameDomain    = "polaris-gamescope-artifacts-v1\x00"
	gamescopeSessionRecordHeader   = "POLARIS-GAMESCOPE-SESSION/1\n"
	maximumGamescopeReadyRecord    = 64
	defaultGamescopeStartupTimeout = 120 * time.Second
)

type gamescopePaths struct {
	parentSocket  string
	targetSocket  string
	readyFIFO     string
	limiterFile   string
	sessionRecord string
}

type gamescopeReadyInfo struct {
	displayNumber uint32
	displayName   string
	waylandName   string
}

func RunNestedCompositor(arguments []string, environment []string) error {
	request, err := parseProviderInvocation(
		seatruntime.StageNestedCompositor,
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
	// Gamescope initialization is intentionally independent of application
	// discovery. Steam Big Picture can legitimately take longer than the old
	// application timeout and may never expose an app id.
	options.startupTimeout = defaultGamescopeStartupTimeout
	options, err = normalizeProviderOptions(options)
	if err != nil {
		_ = ready.Close()
		return err
	}
	providerContext, cancel := signalContext()
	defer cancel()
	return runNestedCompositor(providerContext, request, ready, options)
}

func gamescopeScopedNames(runtimeNamespace string) (string, string, string) {
	digest := sha256.Sum256([]byte(gamescopeArtifactNameDomain + runtimeNamespace))
	encoded := hex.EncodeToString(digest[:])
	return gamescopeReadyNamePrefix + encoded,
		gamescopeLimiterNamePrefix + encoded,
		gamescopeSessionNamePrefix + encoded
}

func gamescopeArguments(
	request seatruntime.Request,
	readyFIFO string,
	software bool,
) []string {
	backend := "wayland"
	if software {
		backend = "headless"
	}
	arguments := []string{
		"--backend", backend,
		"--expose-wayland",
		"--xwayland-count", "1",
		"--force-composition",
		"--force-windows-fullscreen",
		"--keep-alive",
		"-W", strconv.FormatUint(uint64(request.DisplayWidth), 10),
		"-H", strconv.FormatUint(uint64(request.DisplayHeight), 10),
		"-w", strconv.FormatUint(uint64(request.DisplayWidth), 10),
		"-h", strconv.FormatUint(uint64(request.DisplayHeight), 10),
	}
	// Gamescope 3.16.x accepts only integer Hz here. For fractional modes the
	// parent output remains authoritative; the post-start Wayland probe still
	// requires exact millihertz and fails closed if Gamescope does not inherit it.
	if request.DisplayRefreshMillihertz%1000 == 0 {
		arguments = append(
			arguments,
			"-r",
			strconv.FormatUint(uint64(request.DisplayRefreshMillihertz/1000), 10),
		)
	}
	return append(arguments, "-R", readyFIFO)
}

func gamescopeEnvironment(
	request seatruntime.Request,
	paths gamescopePaths,
	options providerOptions,
) []string {
	environment := []string{
		"PATH=/usr/bin",
		"LC_ALL=C",
		"HOME=/nonexistent",
		"XDG_CONFIG_HOME=/nonexistent",
		"XDG_CACHE_HOME=/nonexistent",
		"XDG_DATA_HOME=/nonexistent",
		"XDG_RUNTIME_DIR=" + options.runtimeDirectory,
		"DBUS_SESSION_BUS_ADDRESS=unix:path=" + filepath.Join(options.runtimeDirectory, "bus"),
		"WAYLAND_DISPLAY=" + request.ParentWaylandSocket,
		"POLARIS_RENDER_NODE=" + request.RenderNode,
		"POLARIS_DISPLAY_WIDTH=" + strconv.FormatUint(uint64(request.DisplayWidth), 10),
		"POLARIS_DISPLAY_HEIGHT=" + strconv.FormatUint(uint64(request.DisplayHeight), 10),
		"POLARIS_DISPLAY_REFRESH_MILLIHZ=" + strconv.FormatUint(uint64(request.DisplayRefreshMillihertz), 10),
		"GAMESCOPE_LIMITER_FILE=" + paths.limiterFile,
		"MESA_SHADER_CACHE_DISABLE=true",
	}
	if options.softwareGamescope {
		environment = append(environment,
			"VK_DRIVER_FILES="+options.softwareVulkanICDPath,
			"LIBGL_ALWAYS_SOFTWARE=1",
			"MESA_LOADER_DRIVER_OVERRIDE=llvmpipe",
			"XWAYLAND_NO_GLAMOR=1",
			"POLARIS_GAMESCOPE_X11_SOCKET_DIRECTORY="+options.x11SocketDirectory,
			"POLARIS_GAMESCOPE_X11_LOCK_DIRECTORY="+options.x11LockDirectory,
		)
	}
	return environment
}

func parseSmallCanonicalDecimal(value string, maximum uint64) (uint32, bool) {
	if value == "" || (len(value) > 1 && value[0] == '0') {
		return 0, false
	}
	parsed, err := strconv.ParseUint(value, 10, 32)
	if err != nil || parsed > maximum {
		return 0, false
	}
	return uint32(parsed), true
}

func parseGamescopeReadyRecord(
	line string,
	allowSharedX11 bool,
) (gamescopeReadyInfo, error) {
	if len(line) == 0 || len(line) > maximumGamescopeReadyRecord ||
		!strings.HasSuffix(line, "\n") || strings.Count(line, " ") != 1 {
		return gamescopeReadyInfo{}, errors.New("runtime Gamescope readiness record is invalid")
	}
	content := strings.TrimSuffix(line, "\n")
	display, waylandName, present := strings.Cut(content, " ")
	if !present || len(display) < 2 || display[0] != ':' ||
		waylandName != gamescopeWaylandSocket {
		return gamescopeReadyInfo{}, errors.New("runtime Gamescope readiness record is invalid")
	}
	displayNumber, valid := parseSmallCanonicalDecimal(display[1:], 32)
	if !valid || (!allowSharedX11 && displayNumber != 0) {
		return gamescopeReadyInfo{}, errors.New("runtime Gamescope X11 display is invalid")
	}
	return gamescopeReadyInfo{
		displayNumber: displayNumber,
		displayName:   display,
		waylandName:   waylandName,
	}, nil
}

func automaticGamescopeArtifact(name string) (uint32, bool, bool) {
	if !strings.HasPrefix(name, "gamescope-") {
		return 0, false, false
	}
	value := strings.TrimPrefix(name, "gamescope-")
	mode := uint32(syscall.S_IFSOCK)
	switch {
	case strings.HasSuffix(value, "-ei.lock"):
		value = strings.TrimSuffix(value, "-ei.lock")
		mode = syscall.S_IFREG
	case strings.HasSuffix(value, "-ei"):
		value = strings.TrimSuffix(value, "-ei")
	case strings.HasSuffix(value, ".lock"):
		value = strings.TrimSuffix(value, ".lock")
		mode = syscall.S_IFREG
	}
	index, valid := parseSmallCanonicalDecimal(value, 127)
	if !valid || index != 0 {
		return 0, true, false
	}
	return mode, true, true
}

func gamescopeArtifactMode(
	name string,
	paths gamescopePaths,
) (uint32, bool, bool) {
	switch filepath.Join(filepath.Dir(paths.targetSocket), name) {
	case paths.targetSocket:
		return syscall.S_IFSOCK, true, true
	case paths.readyFIFO:
		return syscall.S_IFIFO, true, true
	case paths.limiterFile, paths.sessionRecord:
		return syscall.S_IFREG, true, true
	}
	return automaticGamescopeArtifact(name)
}

func rejectExistingGamescopeArtifacts(
	runtime *runtimeDirectory,
	paths gamescopePaths,
) error {
	if runtime == nil {
		return errors.New("runtime Gamescope artifact check is invalid")
	}
	if err := runtime.verify(); err != nil {
		return err
	}
	entries, err := os.ReadDir(runtime.path)
	if err != nil {
		return errors.New("runtime Gamescope artifact set is unavailable")
	}
	for _, entry := range entries {
		_, matched, _ := gamescopeArtifactMode(entry.Name(), paths)
		if matched {
			return errors.New("runtime Gamescope artifact already exists")
		}
	}
	return nil
}

func captureCreatedArtifact(runtime *runtimeDirectory, path string, descriptor int) (artifactIdentity, error) {
	var created syscall.Stat_t
	statError := syscall.Fstat(descriptor, &created)
	identity, identityError := runtime.pins.capture(path)
	if statError != nil || identityError != nil || !sameIdentity(identity, artifactIdentity{
		dev: uint64(created.Dev), ino: created.Ino, mode: created.Mode, uid: created.Uid,
	}) {
		return artifactIdentity{rejected: true}, errors.New("runtime Gamescope created artifact was replaced")
	}
	return identity, nil
}

func createGamescopeRegularArtifact(
	runtime *runtimeDirectory,
	path string,
	content []byte,
) (artifactIdentity, error) {
	if runtime == nil || filepath.Dir(path) != runtime.path {
		return artifactIdentity{}, errors.New("runtime Gamescope artifact path is invalid")
	}
	if err := runtime.verify(); err != nil {
		return artifactIdentity{}, err
	}
	descriptor, err := syscall.Open(
		path,
		syscall.O_WRONLY|syscall.O_CREAT|syscall.O_EXCL|syscall.O_NOFOLLOW|syscall.O_CLOEXEC,
		0o600,
	)
	if err != nil {
		return artifactIdentity{rejected: true}, errors.New("runtime Gamescope artifact could not be created")
	}
	file := os.NewFile(uintptr(descriptor), "polaris-gamescope-artifact")
	if file == nil {
		_ = syscall.Close(descriptor)
		return artifactIdentity{rejected: true}, errors.New("runtime Gamescope artifact could not be created")
	}
	writeError := error(nil)
	for remaining := content; len(remaining) > 0; {
		written, err := file.Write(remaining)
		if err != nil || written <= 0 || written > len(remaining) {
			writeError = errors.New("runtime Gamescope artifact could not be written")
			break
		}
		remaining = remaining[written:]
	}
	if writeError == nil && len(content) > 0 {
		writeError = file.Sync()
	}
	identity, identityError := captureCreatedArtifact(runtime, path, descriptor)
	closeError := file.Close()
	if identityError != nil || identity.mode&syscall.S_IFMT != syscall.S_IFREG ||
		identity.uid != runtime.uid || identity.mode&0o7777 != 0o600 {
		return artifactIdentity{rejected: true}, errors.New("runtime Gamescope created artifact ownership is invalid")
	}
	if writeError != nil || closeError != nil {
		return identity, errors.New("runtime Gamescope artifact is invalid")
	}
	return identity, nil
}

func createGamescopeReadyFIFO(
	runtime *runtimeDirectory,
	path string,
) (*os.File, artifactIdentity, error) {
	if runtime == nil || filepath.Dir(path) != runtime.path {
		return nil, artifactIdentity{}, errors.New("runtime Gamescope readiness path is invalid")
	}
	if err := runtime.verify(); err != nil {
		return nil, artifactIdentity{}, err
	}
	if err := syscall.Mkfifo(path, 0o600); err != nil {
		return nil, artifactIdentity{rejected: true}, errors.New("runtime Gamescope readiness FIFO could not be created")
	}
	identity, err := runtime.pins.capture(path)
	if err != nil || identity.mode&syscall.S_IFMT != syscall.S_IFIFO ||
		identity.uid != runtime.uid || identity.mode&0o7777 != 0o600 {
		return nil, artifactIdentity{rejected: true}, errors.New("runtime Gamescope readiness FIFO is invalid")
	}
	descriptor, err := syscall.Open(
		path,
		syscall.O_RDWR|syscall.O_NONBLOCK|syscall.O_NOFOLLOW|syscall.O_CLOEXEC,
		0,
	)
	if err != nil {
		return nil, identity, errors.New("runtime Gamescope readiness FIFO is unavailable")
	}
	file := os.NewFile(uintptr(descriptor), "polaris-gamescope-ready")
	if file == nil {
		_ = syscall.Close(descriptor)
		return nil, identity, errors.New("runtime Gamescope readiness FIFO is unavailable")
	}
	opened, err := captureCreatedArtifact(runtime, path, descriptor)
	if err != nil || !sameIdentity(opened, identity) {
		_ = file.Close()
		return nil, artifactIdentity{rejected: true}, errors.New("runtime Gamescope readiness FIFO was replaced")
	}
	return file, identity, nil
}

func waitForGamescopeReadyRecord(
	parent context.Context,
	child *managedChild,
	ready *os.File,
	deadline time.Time,
) (string, error) {
	if parent == nil || child == nil || ready == nil {
		return "", errors.New("runtime Gamescope readiness wait is invalid")
	}
	remaining := time.Until(deadline)
	if remaining <= 0 {
		return "", errors.New("runtime Gamescope readiness timed out")
	}
	type readResult struct {
		line string
		err  error
	}
	result := make(chan readResult, 1)
	go func() {
		line, err := readBoundedLine(ready, remaining, maximumGamescopeReadyRecord)
		result <- readResult{line: line, err: err}
	}()
	select {
	case <-parent.Done():
		_ = ready.Close()
		return "", errors.New("runtime Gamescope startup was canceled")
	case <-child.done:
		_ = ready.Close()
		return "", errors.New("runtime Gamescope exited before readiness")
	case received := <-result:
		if received.err != nil {
			return "", errors.New("runtime Gamescope readiness was invalid")
		}
		return received.line, nil
	}
}

func validGamescopeArtifact(
	identity artifactIdentity,
	mode uint32,
	uid uint32,
) bool {
	return identity.mode&syscall.S_IFMT == mode && identity.uid == uid &&
		identity.mode&0o077 == 0
}

func captureGamescopeArtifacts(
	runtime *runtimeDirectory,
	paths gamescopePaths,
	previous map[string]artifactIdentity,
	requireComplete bool,
) (map[string]artifactIdentity, error) {
	captured := make(map[string]artifactIdentity)
	if runtime == nil {
		return captured, errors.New("runtime Gamescope artifact capture is invalid")
	}
	if err := runtime.verify(); err != nil {
		return captured, err
	}
	entries, err := os.ReadDir(runtime.path)
	if err != nil {
		return captured, errors.New("runtime Gamescope artifact set is unavailable")
	}
	var result error
	for _, entry := range entries {
		mode, matched, recognized := gamescopeArtifactMode(entry.Name(), paths)
		if !matched {
			continue
		}
		if !recognized {
			result = errors.Join(
				result,
				errors.New("runtime Gamescope created an unexpected artifact"),
			)
			continue
		}
		path := filepath.Join(runtime.path, entry.Name())
		identity, err := runtime.pins.capture(path)
		if err != nil || !validGamescopeArtifact(identity, mode, runtime.uid) {
			result = errors.Join(
				result,
				errors.New("runtime Gamescope artifact is invalid"),
			)
			continue
		}
		if earlier, present := previous[path]; present && !sameIdentity(earlier, identity) {
			result = errors.Join(
				result,
				errors.New("runtime Gamescope artifact identity changed"),
			)
			continue
		}
		captured[path] = identity
	}
	if !requireComplete {
		return captured, result
	}
	required := []string{
		paths.readyFIFO,
		paths.limiterFile,
		paths.sessionRecord,
		paths.targetSocket,
		filepath.Join(runtime.path, gamescopeWaylandSocket),
		filepath.Join(runtime.path, gamescopeWaylandSocket+".lock"),
	}
	for _, path := range required {
		if _, present := captured[path]; !present {
			result = errors.Join(
				result,
				errors.New("runtime Gamescope artifact set is incomplete"),
			)
		}
	}
	sourceIdentity, sourcePresent := captured[filepath.Join(runtime.path, gamescopeWaylandSocket)]
	targetIdentity, targetPresent := captured[paths.targetSocket]
	if sourcePresent && targetPresent && !sameIdentity(sourceIdentity, targetIdentity) {
		result = errors.Join(
			result,
			errors.New("runtime Gamescope Wayland alias is invalid"),
		)
	}
	eiSocket := filepath.Join(runtime.path, gamescopeWaylandSocket+"-ei")
	eiLock := eiSocket + ".lock"
	_, socketPresent := captured[eiSocket]
	_, lockPresent := captured[eiLock]
	if socketPresent != lockPresent {
		result = errors.Join(
			result,
			errors.New("runtime Gamescope input endpoint is incomplete"),
		)
	}
	return captured, result
}

func prepareGamescopeWaylandAlias(
	runtime *runtimeDirectory,
	paths gamescopePaths,
) error {
	source := filepath.Join(runtime.path, gamescopeWaylandSocket)
	lock := source + ".lock"
	for _, artifact := range []struct {
		path string
		mode uint32
	}{
		{source, syscall.S_IFSOCK},
		{lock, syscall.S_IFREG},
	} {
		if err := runtime.verify(); err != nil {
			return err
		}
		identity, err := lstatIdentity(artifact.path)
		if err != nil || !validGamescopeArtifact(identity, artifact.mode, runtime.uid) {
			return errors.New("runtime Gamescope Wayland endpoint is invalid")
		}
	}
	if err := os.Link(source, paths.targetSocket); err != nil {
		return errors.New("runtime Gamescope Wayland alias could not be created")
	}
	sourceIdentity, sourceError := lstatIdentity(source)
	targetIdentity, targetError := lstatIdentity(paths.targetSocket)
	if sourceError != nil || targetError != nil ||
		!sameIdentity(sourceIdentity, targetIdentity) {
		return errors.New("runtime Gamescope Wayland alias identity is invalid")
	}
	return nil
}

func gamescopeSessionRecord(info gamescopeReadyInfo, targetWayland string) []byte {
	return []byte(gamescopeSessionRecordHeader +
		"DISPLAY=" + info.displayName + "\n" +
		"STEAM_GAME_DISPLAY_0=" + info.displayName + "\n" +
		"WAYLAND_DISPLAY=" + targetWayland + "\n" +
		"GAMESCOPE_WAYLAND_DISPLAY=" + targetWayland + "\n")
}

func verifyParentWaylandIdentity(
	runtime *runtimeDirectory,
	path string,
	expected artifactIdentity,
) error {
	if err := runtime.verify(); err != nil {
		return err
	}
	identity, err := lstatIdentity(path)
	if err != nil || !sameIdentity(identity, expected) {
		return errors.New("runtime parent Wayland identity changed")
	}
	return nil
}

func captureX11Baseline(
	socketDirectory *fixedDirectory,
	lockDirectory *fixedDirectory,
	allowShared bool,
) (map[string]artifactIdentity, error) {
	baseline := make(map[string]artifactIdentity)
	if socketDirectory == nil || lockDirectory == nil {
		return baseline, errors.New("runtime X11 namespace is invalid")
	}
	if err := socketDirectory.verify(); err != nil {
		return baseline, err
	}
	if err := lockDirectory.verify(); err != nil {
		return baseline, err
	}
	for display := uint32(0); display <= 32; display++ {
		socketPath, lockPath, _ := x11DisplayPaths(
			socketDirectory.path,
			lockDirectory.path,
			display,
		)
		for _, path := range []string{socketPath, lockPath} {
			identity, err := socketDirectory.pins.capture(path)
			if errors.Is(err, os.ErrNotExist) {
				continue
			}
			if err != nil {
				return baseline, errors.New("runtime X11 namespace changed during inspection")
			}
			baseline[path] = identity
		}
	}
	if !allowShared && len(baseline) != 0 {
		return baseline, errors.New("runtime X11 namespace is not private")
	}
	if err := socketDirectory.verify(); err != nil {
		return baseline, err
	}
	if err := lockDirectory.verify(); err != nil {
		return baseline, err
	}
	return baseline, nil
}

func captureGamescopeX11Artifacts(
	socketDirectory *fixedDirectory,
	lockDirectory *fixedDirectory,
	info gamescopeReadyInfo,
	childPID int,
	uid uint32,
	baseline map[string]artifactIdentity,
) (map[string]artifactIdentity, string, error) {
	captured := make(map[string]artifactIdentity, 2)
	if err := socketDirectory.verify(); err != nil {
		return captured, "", err
	}
	if err := lockDirectory.verify(); err != nil {
		return captured, "", err
	}
	socketPath, lockPath, err := x11DisplayPaths(
		socketDirectory.path,
		lockDirectory.path,
		info.displayNumber,
	)
	if err != nil {
		return captured, "", err
	}
	for _, artifact := range []struct {
		path string
		mode uint32
	}{
		{socketPath, syscall.S_IFSOCK},
		{lockPath, syscall.S_IFREG},
	} {
		identity, err := socketDirectory.pins.capture(artifact.path)
		if err != nil || !validGamescopeArtifact(identity, artifact.mode, uid) {
			return captured, "", errors.New("runtime Gamescope X11 artifact is invalid")
		}
		if earlier, present := baseline[artifact.path]; present &&
			sameIdentity(earlier, identity) {
			return captured, "", errors.New("runtime Gamescope reused an unowned X11 artifact")
		}
		captured[artifact.path] = identity
	}
	if err := validateX11Lock(lockPath, captured[lockPath], childPID); err != nil {
		return captured, "", err
	}
	if err := socketDirectory.verify(); err != nil {
		return captured, "", err
	}
	if err := lockDirectory.verify(); err != nil {
		return captured, "", err
	}
	return captured, socketPath, nil
}

func capturePartialGamescopeX11Artifacts(
	socketDirectory *fixedDirectory,
	lockDirectory *fixedDirectory,
	childPID int,
	uid uint32,
	baseline map[string]artifactIdentity,
) (map[string]artifactIdentity, error) {
	captured := make(map[string]artifactIdentity)
	if childPID <= 0 || socketDirectory == nil || lockDirectory == nil {
		return captured, errors.New("runtime Gamescope partial X11 capture is invalid")
	}
	if err := socketDirectory.verify(); err != nil {
		return captured, err
	}
	if err := lockDirectory.verify(); err != nil {
		return captured, err
	}
	var result error
	for display := uint32(0); display <= 32; display++ {
		socketPath, lockPath, _ := x11DisplayPaths(
			socketDirectory.path,
			lockDirectory.path,
			display,
		)
		socketIdentity, socketError := socketDirectory.pins.capture(socketPath)
		lockIdentity, lockError := socketDirectory.pins.capture(lockPath)
		socketMissing := errors.Is(socketError, os.ErrNotExist)
		lockMissing := errors.Is(lockError, os.ErrNotExist)
		if socketMissing && lockMissing {
			continue
		}
		if socketError != nil || lockError != nil {
			result = errors.Join(
				result,
				errors.New("runtime Gamescope partial X11 artifact set is incomplete"),
			)
			continue
		}
		baselineSocket, hadSocket := baseline[socketPath]
		baselineLock, hadLock := baseline[lockPath]
		if hadSocket && hadLock &&
			sameIdentity(baselineSocket, socketIdentity) &&
			sameIdentity(baselineLock, lockIdentity) {
			continue
		}
		if !validGamescopeArtifact(socketIdentity, syscall.S_IFSOCK, uid) ||
			!validGamescopeArtifact(lockIdentity, syscall.S_IFREG, uid) ||
			validateX11Lock(lockPath, lockIdentity, childPID) != nil {
			result = errors.Join(
				result,
				errors.New("runtime Gamescope partial X11 artifact is invalid"),
			)
			continue
		}
		captured[socketPath] = socketIdentity
		captured[lockPath] = lockIdentity
	}
	result = errors.Join(result, socketDirectory.verify(), lockDirectory.verify())
	return captured, result
}

func cleanupKnownArtifacts(known map[string]artifactIdentity, preferred []string) error {
	var result error
	seen := make(map[string]struct{}, len(known))
	ordered := make([]string, 0, len(known))
	for _, path := range preferred {
		if _, present := known[path]; present {
			ordered = append(ordered, path)
			seen[path] = struct{}{}
		}
	}
	for path := range known {
		if _, present := seen[path]; !present {
			ordered = append(ordered, path)
		}
	}
	for _, path := range ordered {
		expected := known[path]
		identity, err := lstatIdentity(path)
		if errors.Is(err, os.ErrNotExist) {
			continue
		}
		if err != nil || !sameIdentity(identity, expected) {
			result = errors.Join(
				result,
				errors.New("runtime Gamescope cleanup found a replacement artifact"),
			)
			continue
		}
		if err := os.Remove(path); err != nil {
			result = errors.Join(
				result,
				errors.New("runtime Gamescope artifact could not be removed"),
			)
		}
	}
	return result
}

func cleanupGamescopeRuntimeArtifacts(
	runtime *runtimeDirectory,
	paths gamescopePaths,
	known map[string]artifactIdentity,
	allowOwnedCapture bool,
) error {
	if runtime == nil {
		return errors.New("runtime Gamescope cleanup is invalid")
	}
	var result error
	if allowOwnedCapture {
		captured, err := captureGamescopeArtifacts(runtime, paths, known, false)
		if known == nil {
			known = make(map[string]artifactIdentity)
		}
		for path, identity := range captured {
			known[path] = identity
		}
		result = errors.Join(result, err)
	}
	result = errors.Join(result, cleanupKnownArtifacts(known, []string{
		paths.targetSocket,
		paths.sessionRecord,
		paths.readyFIFO,
		paths.limiterFile,
	}))
	if err := runtime.verify(); err != nil {
		return errors.Join(result, err)
	}
	entries, err := os.ReadDir(runtime.path)
	if err != nil {
		return errors.Join(result, errors.New("runtime Gamescope artifact set is unavailable"))
	}
	for _, entry := range entries {
		_, matched, _ := gamescopeArtifactMode(entry.Name(), paths)
		if matched {
			result = errors.Join(
				result,
				errors.New("runtime Gamescope cleanup retained an unexpected artifact"),
			)
			break
		}
	}
	return result
}

func cleanupGamescopeX11Artifacts(
	socketDirectory *fixedDirectory,
	lockDirectory *fixedDirectory,
	known map[string]artifactIdentity,
) error {
	if socketDirectory == nil || lockDirectory == nil {
		return errors.New("runtime Gamescope X11 cleanup is invalid")
	}
	if err := socketDirectory.verify(); err != nil {
		return err
	}
	if err := lockDirectory.verify(); err != nil {
		return err
	}
	var socketPath string
	var lockPath string
	for path, identity := range known {
		switch identity.mode & syscall.S_IFMT {
		case syscall.S_IFSOCK:
			socketPath = path
		case syscall.S_IFREG:
			lockPath = path
		}
	}
	return cleanupKnownArtifacts(known, []string{socketPath, lockPath})
}

func runNestedCompositor(
	parent context.Context,
	request seatruntime.Request,
	ready io.WriteCloser,
	options providerOptions,
) (result error) {
	if parent == nil || ready == nil || request.Stage != seatruntime.StageNestedCompositor {
		if ready != nil {
			_ = ready.Close()
		}
		return errors.New("runtime Gamescope provider is invalid")
	}
	defer func() {
		if ready != nil {
			_ = ready.Close()
		}
	}()
	if _, err := seatruntime.Arguments(request); err != nil ||
		request.Compositor != "gamescope" ||
		!strings.HasPrefix(request.ParentWaylandSocket, displayCaptureSocketPrefix) ||
		!strings.HasPrefix(request.WaylandSocket, "polaris-wayland-") ||
		strings.HasSuffix(request.WaylandSocket, ".lock") {
		return errors.New("runtime Gamescope request is invalid")
	}
	if request.DisplayHDR {
		return errors.New("runtime Gamescope HDR is unsupported")
	}
	options, err := normalizeProviderOptions(options)
	if err != nil {
		return err
	}
	select {
	case <-parent.Done():
		return errors.New("runtime Gamescope startup was canceled")
	default:
	}
	runtime, err := openRuntimeDirectory(options.runtimeDirectory, options.runtimeOwnerUID)
	if err != nil {
		return err
	}
	defer runtime.close()
	x11Sockets, err := openFixedDirectory(
		options.x11SocketDirectory,
		options.x11DirectoryOwnerUID,
		options.x11DirectoryMode,
	)
	if err != nil {
		return err
	}
	defer x11Sockets.close()
	x11Locks, err := openFixedDirectory(
		options.x11LockDirectory,
		options.x11DirectoryOwnerUID,
		options.x11DirectoryMode,
	)
	if err != nil {
		return err
	}
	defer x11Locks.close()
	readyName, limiterName, sessionName := gamescopeScopedNames(request.RuntimeNamespace)
	paths := gamescopePaths{
		parentSocket:  filepath.Join(runtime.path, request.ParentWaylandSocket),
		targetSocket:  filepath.Join(runtime.path, request.WaylandSocket),
		readyFIFO:     filepath.Join(runtime.path, readyName),
		limiterFile:   filepath.Join(runtime.path, limiterName),
		sessionRecord: filepath.Join(runtime.path, sessionName),
	}
	for _, path := range []string{
		paths.parentSocket,
		paths.targetSocket,
		paths.readyFIFO,
		paths.limiterFile,
		paths.sessionRecord,
	} {
		if !validAbsolutePath(path) || filepath.Dir(path) != runtime.path {
			return errors.New("runtime Gamescope path is invalid")
		}
	}
	if !validUnixSocketPath(paths.parentSocket) ||
		!validUnixSocketPath(paths.targetSocket) {
		return errors.New("runtime Gamescope socket path is too long")
	}
	if err := rejectExistingGamescopeArtifacts(runtime, paths); err != nil {
		return err
	}
	parentIdentity, err := runtime.pins.capture(paths.parentSocket)
	if err != nil || !validGamescopeArtifact(
		parentIdentity,
		syscall.S_IFSOCK,
		options.runtimeOwnerUID,
	) {
		return errors.New("runtime parent Wayland socket is invalid")
	}
	parentExpectation := waylandProbeExpectation{
		width:         request.DisplayWidth,
		height:        request.DisplayHeight,
		refresh:       request.DisplayRefreshMillihertz,
		requireDMABuf: !options.softwareGamescope,
		peerPID:       0,
		peerUID:       options.runtimeOwnerUID,
	}
	if err := probeWaylandDisplay(
		parent,
		paths.parentSocket,
		parentExpectation,
		options.probeTimeout,
	); err != nil {
		return errors.New("runtime parent Wayland display is unavailable")
	}
	if !options.softwareGamescope {
		if err := validateDisplayRenderNode(request.RenderNode); err != nil {
			return err
		}
	}
	for _, executablePath := range []string{options.gamescopePath, options.xWaylandPath} {
		executable, err := openTrustedExecutable(executablePath, options.executableOwnerUID)
		if err != nil {
			return err
		}
		_ = executable.Close()
	}
	x11Baseline, err := captureX11Baseline(
		x11Sockets,
		x11Locks,
		options.allowSharedX11,
	)
	if err != nil {
		return err
	}
	knownRuntime := make(map[string]artifactIdentity)
	readyFIFO, readyIdentity, err := createGamescopeReadyFIFO(runtime, paths.readyFIFO)
	if readyIdentity != (artifactIdentity{}) {
		knownRuntime[paths.readyFIFO] = readyIdentity
	}
	if err != nil {
		cleanupError := cleanupGamescopeRuntimeArtifacts(runtime, paths, knownRuntime, true)
		return errors.Join(err, cleanupError)
	}
	limiterIdentity, err := createGamescopeRegularArtifact(runtime, paths.limiterFile, nil)
	if limiterIdentity != (artifactIdentity{}) {
		knownRuntime[paths.limiterFile] = limiterIdentity
	}
	if err != nil {
		_ = readyFIFO.Close()
		cleanupError := cleanupGamescopeRuntimeArtifacts(runtime, paths, knownRuntime, true)
		return errors.Join(err, cleanupError)
	}
	child, err := startManagedChildWithUmask(
		options.gamescopePath,
		options.executableOwnerUID,
		gamescopeArguments(request, paths.readyFIFO, options.softwareGamescope),
		gamescopeEnvironment(request, paths, options),
		nil,
		0o077,
	)
	if err != nil {
		_ = readyFIFO.Close()
		cleanupError := cleanupGamescopeRuntimeArtifacts(runtime, paths, knownRuntime, false)
		return errors.Join(err, cleanupError)
	}
	readyPublished := false
	knownX11 := make(map[string]artifactIdentity)
	defer func() {
		_ = readyFIFO.Close()
		stopError := child.stop(options.stopTimeout)
		if !readyPublished && len(knownX11) == 0 {
			capturedX11, captureError := capturePartialGamescopeX11Artifacts(
				x11Sockets,
				x11Locks,
				child.command.Process.Pid,
				options.runtimeOwnerUID,
				x11Baseline,
			)
			for path, identity := range capturedX11 {
				knownX11[path] = identity
			}
			result = errors.Join(result, captureError)
		}
		runtimeCleanupError := cleanupGamescopeRuntimeArtifacts(
			runtime,
			paths,
			knownRuntime,
			!readyPublished,
		)
		x11CleanupError := cleanupGamescopeX11Artifacts(x11Sockets, x11Locks, knownX11)
		result = errors.Join(result, stopError, runtimeCleanupError, x11CleanupError)
	}()
	deadline := time.Now().Add(options.startupTimeout)
	line, err := waitForGamescopeReadyRecord(parent, child, readyFIFO, deadline)
	if err != nil {
		return err
	}
	if err := readyFIFO.Close(); err != nil {
		return errors.New("runtime Gamescope readiness FIFO could not be closed")
	}
	info, err := parseGamescopeReadyRecord(line, options.allowSharedX11)
	if err != nil {
		return err
	}
	if err := prepareGamescopeWaylandAlias(runtime, paths); err != nil {
		return err
	}
	knownX11, x11Socket, err := captureGamescopeX11Artifacts(
		x11Sockets,
		x11Locks,
		info,
		child.command.Process.Pid,
		options.runtimeOwnerUID,
		x11Baseline,
	)
	if err != nil {
		return err
	}
	remaining := time.Until(deadline)
	if remaining <= 0 {
		return errors.New("runtime Gamescope protocol readiness timed out")
	}
	if err := probeWaylandDisplay(
		parent,
		filepath.Join(runtime.path, info.waylandName),
		waylandProbeExpectation{
			width:         request.DisplayWidth,
			height:        request.DisplayHeight,
			refresh:       request.DisplayRefreshMillihertz,
			requireDMABuf: false,
			peerPID:       child.command.Process.Pid,
			peerUID:       options.runtimeOwnerUID,
		},
		boundedProbeTimeout(remaining, options.probeTimeout),
	); err != nil {
		return errors.New("runtime Gamescope Wayland display is unavailable")
	}
	remaining = time.Until(deadline)
	if remaining <= 0 {
		return errors.New("runtime Gamescope X11 readiness timed out")
	}
	if err := probeX11Display(
		parent,
		x11Socket,
		x11ProbeExpectation{
			width:   request.DisplayWidth,
			height:  request.DisplayHeight,
			peerPID: child.command.Process.Pid,
			peerUID: options.runtimeOwnerUID,
		},
		boundedProbeTimeout(remaining, options.probeTimeout),
	); err != nil {
		return err
	}
	if err := verifyParentWaylandIdentity(runtime, paths.parentSocket, parentIdentity); err != nil {
		return err
	}
	remaining = time.Until(deadline)
	if remaining <= 0 {
		return errors.New("runtime parent Wayland revalidation timed out")
	}
	if err := probeWaylandDisplay(
		parent,
		paths.parentSocket,
		parentExpectation,
		boundedProbeTimeout(remaining, options.probeTimeout),
	); err != nil {
		return errors.New("runtime parent Wayland display changed during Gamescope startup")
	}
	if child.exited() {
		return errors.New("runtime Gamescope exited before readiness")
	}
	sessionIdentity, err := createGamescopeRegularArtifact(
		runtime,
		paths.sessionRecord,
		gamescopeSessionRecord(info, request.WaylandSocket),
	)
	if sessionIdentity != (artifactIdentity{}) {
		knownRuntime[paths.sessionRecord] = sessionIdentity
	}
	if err != nil {
		return err
	}
	capturedRuntime, err := captureGamescopeArtifacts(
		runtime,
		paths,
		knownRuntime,
		true,
	)
	// A failed capture omits conflicting paths. Retain their original ownership
	// so deferred partial cleanup cannot rediscover and adopt the replacements.
	for path, identity := range capturedRuntime {
		knownRuntime[path] = identity
	}
	if err != nil {
		return err
	}
	if err := publishReadiness(ready); err != nil {
		return err
	}
	readyPublished = true
	ready = nil
	select {
	case <-parent.Done():
		return nil
	case <-child.done:
		return errors.New("runtime Gamescope exited unexpectedly")
	}
}
