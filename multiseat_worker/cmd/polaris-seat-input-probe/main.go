//go:build linux && amd64

// polaris-seat-input-probe is an opt-in physical acceptance helper. Production
// worker startup never calls it; it reads only the four reserved seat aliases.
package main

import (
	"encoding/binary"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"syscall"
	"time"

	"github.com/papi-ux/polaris/multiseat_worker/internal/seatinput"
)

type device struct {
	Path  string `json:"path"`
	Major uint32 `json:"major"`
	Minor uint32 `json:"minor"`
}
type request struct {
	Token   string   `json:"token"`
	Devices []device `json:"devices"`
	Absent  []string `json:"absent"`
}
type observation struct {
	Markers          [4]int `json:"markers"`
	Events           [4]int `json:"events"`
	IdentityVerified bool   `json:"identity_verified"`
}

var aliases = [4]string{"/dev/input/polaris-keyboard", "/dev/input/polaris-mouse-relative", "/dev/input/polaris-mouse-absolute", "/dev/input/polaris-gamepad-0"}
var tokenPattern = regexp.MustCompile(`^[0-9a-f]{32}$`)
var eventPattern = regexp.MustCompile(`^/dev/input/event[0-9]+$`)

func marker(index int, kind, code uint16, value int32) bool {
	switch index {
	case 0:
		return kind == 1 && code == 30 && value == 1
	case 1:
		return kind == 2 && code == 0 && value == 7
	case 2:
		return kind == 3 && code == 0 && value > 0
	case 3:
		return kind == 1 && code == 304 && value == 1
	}
	return false
}
func readyPath(token string) (string, error) {
	if !tokenPattern.MatchString(token) {
		return "", errors.New("invalid probe token")
	}
	return filepath.Join("/run/polaris", "input-probe-"+token+".ready"), nil
}

type observationWindow struct{ deadline, drainUntil time.Time }

func (window *observationWindow) advance(now time.Time, finish bool) (bool, error) {
	if !now.Before(window.deadline) {
		return false, errors.New("host finish signal did not complete before deadline")
	}
	if finish && window.drainUntil.IsZero() {
		window.drainUntil = now.Add(250 * time.Millisecond)
	}
	return !window.drainUntil.IsZero() && !now.Before(window.drainUntil), nil
}
func recordEvent(result *observation, index int, event []byte) error {
	if len(event) != 24 {
		return errors.New("truncated kernel event")
	}
	kind, code := binary.LittleEndian.Uint16(event[16:18]), binary.LittleEndian.Uint16(event[18:20])
	value := int32(binary.LittleEndian.Uint32(event[20:24]))
	if kind == 0 && code == 3 {
		return errors.New("kernel input observation dropped events")
	}
	if kind != 0 {
		result.Events[index]++
	}
	if marker(index, kind, code, value) {
		result.Markers[index]++
	}
	if result.Events[index] > 4096 {
		return errors.New("input observation limit exceeded")
	}
	return nil
}

func observe(req request) (observation, error) {
	var result observation
	ready, err := readyPath(req.Token)
	if err != nil {
		return result, err
	}
	if len(req.Devices) != len(aliases) || len(req.Absent) > 32 {
		return result, errors.New("invalid device manifest")
	}
	// This setting is installed by the same host authority that mounts the
	// fixed aliases. Exercise the production identity reader before injecting.
	inputs, err := seatinput.Open(seatinput.Directory, os.Getenv("POLARIS_INPUT_SEAT"))
	if err != nil {
		return result, err
	}
	defer inputs.Close()
	result.IdentityVerified = true
	for _, path := range append(req.Absent, "/dev/uinput", "/dev/uhid") {
		if path != "/dev/uinput" && path != "/dev/uhid" && !eventPattern.MatchString(path) {
			return result, errors.New("invalid excluded node")
		}
		if _, err := os.Lstat(path); !errors.Is(err, os.ErrNotExist) {
			return result, errors.New("unallocated input node is visible")
		}
	}
	entries, err := os.ReadDir("/dev/input")
	if err != nil || len(entries) != len(aliases) {
		return result, errors.New("unexpected input namespace")
	}
	fds := make([]int, 0, len(aliases))
	defer func() {
		for _, fd := range fds {
			syscall.Close(fd)
		}
	}()
	for index, dev := range req.Devices {
		if dev.Path != aliases[index] {
			return result, errors.New("unexpected seat alias")
		}
		fd, err := syscall.Open(dev.Path, syscall.O_RDONLY|syscall.O_NONBLOCK|syscall.O_NOFOLLOW|syscall.O_CLOEXEC, 0)
		if err != nil {
			return result, fmt.Errorf("allocated input %d cannot be opened: %w", index, err)
		}
		fds = append(fds, fd)
		var stat syscall.Stat_t
		if err := syscall.Fstat(fd, &stat); err != nil {
			return result, err
		}
		major := uint32((stat.Rdev>>8)&0xfff | (stat.Rdev>>32)&0xfffff000)
		minor := uint32(stat.Rdev&0xff | (stat.Rdev>>12)&0xffffff00)
		if stat.Mode&syscall.S_IFMT != syscall.S_IFCHR || major != dev.Major || minor != dev.Minor {
			return result, errors.New("allocated device identity changed")
		}
	}
	fd, err := syscall.Open(ready, syscall.O_WRONLY|syscall.O_CREAT|syscall.O_EXCL|syscall.O_NOFOLLOW|syscall.O_CLOEXEC, 0600)
	if err != nil {
		return result, err
	}
	if _, err = syscall.Write(fd, []byte("ready\n")); err != nil {
		syscall.Close(fd)
		os.Remove(ready)
		return result, err
	}
	syscall.Close(fd)
	defer os.Remove(ready)
	finish := ready + ".finish"
	defer os.Remove(finish)
	window := observationWindow{deadline: time.Now().Add(10 * time.Second)}
	finishSeen := false
	buffer := make([]byte, 24*64)
	for {
		if !finishSeen {
			if info, err := os.Lstat(finish); err == nil {
				if !info.Mode().IsRegular() || info.Mode().Perm() != 0600 {
					return result, errors.New("invalid finish signal")
				}
				finishSeen = true
			} else if !errors.Is(err, os.ErrNotExist) {
				return result, err
			}
		}
		for index, fd := range fds {
			count, err := syscall.Read(fd, buffer)
			if err == syscall.EAGAIN || err == syscall.EINTR {
				continue
			}
			if err != nil {
				return result, err
			}
			if count%24 != 0 {
				return result, errors.New("truncated kernel event")
			}
			for offset := 0; offset < count; offset += 24 {
				if err := recordEvent(&result, index, buffer[offset:offset+24]); err != nil {
					return result, err
				}
			}
		}
		completed, err := window.advance(time.Now(), finishSeen)
		if err != nil {
			return result, err
		}
		if completed {
			if err := inputs.Verify(); err != nil {
				return result, err
			}
			return result, nil
		}
		time.Sleep(5 * time.Millisecond)
	}
}
func run() error {
	if len(os.Args) != 3 {
		return errors.New("expected observe manifest or ready token")
	}
	if os.Args[1] == "ready" || os.Args[1] == "finish" {
		path, err := readyPath(os.Args[2])
		if err != nil {
			return err
		}
		info, err := os.Lstat(path)
		if err != nil || !info.Mode().IsRegular() || info.Mode().Perm() != 0600 {
			return errors.New("probe is not ready")
		}
		if os.Args[1] == "finish" {
			fd, err := syscall.Open(path+".finish", syscall.O_WRONLY|syscall.O_CREAT|syscall.O_EXCL|syscall.O_NOFOLLOW|syscall.O_CLOEXEC, 0600)
			if err != nil {
				return err
			}
			return syscall.Close(fd)
		}
		return nil
	}
	if os.Args[1] != "observe" || len(os.Args[2]) > 16384 {
		return errors.New("invalid observation request")
	}
	var req request
	if err := json.Unmarshal([]byte(os.Args[2]), &req); err != nil {
		return err
	}
	result, err := observe(req)
	if err != nil {
		return err
	}
	return json.NewEncoder(os.Stdout).Encode(result)
}
func main() {
	if err := run(); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
