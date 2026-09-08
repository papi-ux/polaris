//go:build linux

package seatprovider

import (
	"context"
	"errors"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"

	"github.com/papi-ux/polaris/multiseat_worker/internal/seatruntime"
)

type launcherSession struct {
	display                string
	pid                    int
	width, height, refresh uint32
	cookie                 processCookie
	lifetime               *processLifetime
}

func gamescopeLauncherRecord(info gamescopeReadyInfo, request seatruntime.Request, pid int, cookie processCookie) []byte {
	return []byte("POLARIS-GAMESCOPE-SESSION/3\n" +
		"DISPLAY=" + info.displayName + "\n" +
		"STEAM_GAME_DISPLAY_0=" + info.displayName + "\n" +
		"WAYLAND_DISPLAY=" + request.WaylandSocket + "\n" +
		"GAMESCOPE_WAYLAND_DISPLAY=" + request.WaylandSocket + "\n" +
		"PID=" + strconv.Itoa(pid) + "\n" +
		"WIDTH=" + strconv.FormatUint(uint64(request.DisplayWidth), 10) + "\n" +
		"HEIGHT=" + strconv.FormatUint(uint64(request.DisplayHeight), 10) + "\n" +
		"REFRESH_MILLIHZ=" + strconv.FormatUint(uint64(request.DisplayRefreshMillihertz), 10) + "\n" +
		"PROCESS_DEVICE=" + strconv.FormatUint(cookie.device, 10) + "\n" +
		"PROCESS_INODE=" + strconv.FormatUint(cookie.inode, 10) + "\n")
}

func parseLauncherSession(content []byte, wayland string) (launcherSession, error) {
	var result launcherSession
	lines := strings.Split(string(content), "\n")
	if len(lines) != 12 || lines[0] != "POLARIS-GAMESCOPE-SESSION/3" ||
		lines[1] != "DISPLAY=:0" || lines[2] != "STEAM_GAME_DISPLAY_0=:0" ||
		lines[3] != "WAYLAND_DISPLAY="+wayland || lines[4] != "GAMESCOPE_WAYLAND_DISPLAY="+wayland || lines[11] != "" {
		return result, errors.New("launcher session record is invalid")
	}
	values := make([]uint32, 4)
	for i, prefix := range []string{"PID=", "WIDTH=", "HEIGHT=", "REFRESH_MILLIHZ="} {
		if !strings.HasPrefix(lines[i+5], prefix) {
			return result, errors.New("launcher session field is invalid")
		}
		value, ok := parseSmallCanonicalDecimal(strings.TrimPrefix(lines[i+5], prefix), 0x7fffffff)
		if !ok || value == 0 {
			return result, errors.New("launcher session value is invalid")
		}
		values[i] = value
	}
	if values[0] <= 1 || values[1] > 16384 || values[2] > 16384 || values[3] < 1000 || values[3] > 1000000 {
		return result, errors.New("launcher display allocation is invalid")
	}
	var cookie [2]uint64
	for i, prefix := range []string{"PROCESS_DEVICE=", "PROCESS_INODE="} {
		text := strings.TrimPrefix(lines[9+i], prefix)
		value, err := strconv.ParseUint(text, 10, 64)
		if err != nil || value == 0 || strconv.FormatUint(value, 10) != text || !strings.HasPrefix(lines[9+i], prefix) {
			return result, errors.New("launcher process cookie is invalid")
		}
		cookie[i] = value
	}
	return launcherSession{display: ":0", pid: int(values[0]), width: values[1], height: values[2], refresh: values[3], cookie: processCookie{cookie[0], cookie[1]}}, nil
}

func readLauncherSession(parent context.Context, request seatruntime.Request, runtime *runtimeDirectory, options providerOptions) (launcherSession, error) {
	var result launcherSession
	_, _, name := gamescopeScopedNames(request.RuntimeNamespace)
	path := filepath.Join(runtime.path, name)
	identity, err := runtime.pins.capture(path)
	if err != nil || identity.mode&syscall.S_IFMT != syscall.S_IFREG || identity.mode&0o7777 != 0o600 || identity.uid != options.runtimeOwnerUID {
		return result, errors.New("launcher session identity is invalid")
	}
	content, err := readIdentityBounded(path, identity, 1024)
	if err != nil {
		return result, err
	}
	result, err = parseLauncherSession(content, request.WaylandSocket)
	if err != nil {
		return result, err
	}
	lifetime, err := retainProcessLifetime(result.pid, -1, result.cookie)
	if err != nil {
		return result, err
	}
	success := false
	defer func() {
		if !success {
			lifetime.close()
		}
	}()
	if err := probeWaylandDisplay(parent, filepath.Join(runtime.path, request.WaylandSocket), waylandProbeExpectation{
		width: result.width, height: result.height, refresh: result.refresh, peerPID: result.pid, peerUID: options.runtimeOwnerUID,
	}, options.probeTimeout); err != nil {
		return result, err
	}
	xSocket, xLock, err := x11DisplayPaths(options.x11SocketDirectory, options.x11LockDirectory, 0)
	if err != nil {
		return result, err
	}
	lockIdentity, err := runtime.pins.capture(xLock)
	if err != nil || lockIdentity.uid != options.runtimeOwnerUID {
		return result, errors.New("launcher X11 lock identity is invalid")
	}
	if err := validateX11Lock(xLock, lockIdentity, result.pid); err != nil {
		return result, err
	}
	if err := probeX11Display(parent, xSocket, x11ProbeExpectation{
		width: result.width, height: result.height, peerPID: result.pid, peerUID: options.runtimeOwnerUID,
	}, options.probeTimeout); err != nil {
		return result, err
	}
	current, err := lstatIdentity(path)
	if err != nil || !sameIdentity(current, identity) {
		return result, errors.New("launcher session was replaced")
	}
	if err := runtime.verify(); err != nil {
		return result, err
	}
	if err := lifetime.verify(); err != nil {
		return result, err
	}
	result.lifetime = lifetime
	success = true
	return result, nil
}
