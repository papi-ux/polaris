//go:build linux

// Package seatinput verifies read-only worker aliases against the host's
// generation-specific input seat. Device creation stays on the host. Steam may
// translate gamepad input into its separately allocated output device.
package seatinput

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"syscall"
	"unsafe"
)

const Directory = "/dev/input"
const maximumDevices = 22 // normal input set plus one Steam translation output

type role struct {
	name, kernel, phys string
	compositor         bool
	steamOutput        bool
}

func deviceRole(name string) (role, bool) {
	r := role{name: name, compositor: true}
	switch name {
	case "polaris-keyboard":
		r.kernel, r.phys = "keyboard", "keyboard"
	case "polaris-mouse-relative":
		r.kernel, r.phys = "mouse", "mouse"
	case "polaris-mouse-absolute":
		r.kernel, r.phys = "mouse (absolute)", "mouse"
	case "polaris-touch":
		r.kernel, r.phys = "touch", "touch"
	case "polaris-pen":
		r.kernel, r.phys = "pen", "pen"
	default:
		if _, ok := eventMinor(name); ok {
			r.kernel, r.phys, r.compositor, r.steamOutput = "steam-gamepad-0", "steam-gamepad/0", false, true
			return r, true
		}
		const prefix = "polaris-gamepad-"
		value := strings.TrimPrefix(name, prefix)
		slot, err := strconv.ParseUint(value, 10, 32)
		if !strings.HasPrefix(name, prefix) || err != nil || slot >= 16 || strconv.FormatUint(slot, 10) != value {
			return role{}, false
		}
		r.kernel, r.phys, r.compositor = "gamepad-"+value, "gamepad/"+value, false
	}
	return r, true
}

// Proton discovers Steam's virtual controller through its canonical eventN
// path and sysfs identity. A matching basename is only a candidate; Open also
// requires the exact generation name, physical identity, evdev number and ID.
func eventMinor(name string) (uint64, bool) {
	value := strings.TrimPrefix(name, "event")
	n, err := strconv.ParseUint(value, 10, 32)
	if !strings.HasPrefix(name, "event") || err != nil || strconv.FormatUint(n, 10) != value {
		return 0, false
	}
	if n < 32 {
		return n + 64, true
	}
	return n, n >= 256
}

func validSeat(seat string) bool {
	if len(seat) == 0 || len(seat) > 128 {
		return false
	}
	for i, c := range []byte(seat) {
		if (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') {
			continue
		}
		if i == 0 || (c != '-' && c != '_' && c != '.') {
			return false
		}
	}
	return true
}

func expectedIdentity(seat string, r role) (string, string) {
	digest := sha256.Sum256([]byte(seat))
	name := "Polaris multiseat " + hex.EncodeToString(digest[:16]) + " " + r.kernel
	prefix := "polaris/client-input-seat-isolated/"
	if !r.compositor {
		prefix = "polaris/client-gamepad-seat-isolated/"
	}
	return name, prefix + seat + "/" + r.phys
}

func ioctl(fd int, number uintptr, data unsafe.Pointer) error {
	_, _, errno := syscall.Syscall(syscall.SYS_IOCTL, uintptr(fd), number, uintptr(data))
	if errno != 0 {
		return errno
	}
	return nil
}

func kernelString(fd int, number uintptr) (string, error) {
	var buffer [256]byte
	if err := ioctl(fd, 0x80000000|uintptr(len(buffer))<<16|uintptr('E')<<8|number, unsafe.Pointer(&buffer[0])); err != nil {
		return "", err
	}
	end := bytes.IndexByte(buffer[:], 0)
	if end < 0 {
		return "", errors.New("unterminated input identity")
	}
	return string(buffer[:end]), nil
}

type Device struct {
	file   *os.File
	status syscall.Stat_t
	role   role
}
type Set struct {
	directory *os.File
	status    syscall.Stat_t
	path      string
	devices   []Device
}

// Open admits only the reserved aliases, opens each through a retained directory
// descriptor, and verifies the kernel identity via the resulting evdev FD. No
// account database, udev environment, symlink or ambient device path is trusted.
func Open(path, seat string) (_ *Set, result error) {
	if !validSeat(seat) || !filepath.IsAbs(path) || filepath.Clean(path) != path {
		return nil, errors.New("invalid input allocation")
	}
	fd, err := syscall.Open(path, syscall.O_RDONLY|syscall.O_DIRECTORY|syscall.O_NOFOLLOW|syscall.O_CLOEXEC, 0)
	if err != nil {
		return nil, errors.New("input namespace unavailable")
	}
	s := &Set{directory: os.NewFile(uintptr(fd), "seat-input-directory"), path: path}
	defer func() {
		if result != nil {
			s.Close()
		}
	}()
	if err := syscall.Fstat(fd, &s.status); err != nil {
		return nil, err
	}
	if s.status.Mode&0o022 != 0 {
		return nil, errors.New("writable input namespace")
	}
	names, err := s.directory.Readdirnames(maximumDevices + 1)
	if (err != nil && err != io.EOF) || len(names) < 3 || len(names) > maximumDevices {
		return nil, errors.New("invalid input namespace")
	}
	sort.Strings(names)
	seen := make(map[string]bool)
	identities := make(map[uint64]bool)
	for _, name := range names {
		r, ok := deviceRole(name)
		if !ok {
			return nil, errors.New("unallocated input alias")
		}
		deviceFD, err := syscall.Openat(fd, name, syscall.O_RDONLY|syscall.O_NONBLOCK|syscall.O_NOFOLLOW|syscall.O_CLOEXEC, 0)
		if err != nil {
			return nil, errors.New("allocated input unavailable")
		}
		d := Device{file: os.NewFile(uintptr(deviceFD), "seat-input"), role: r}
		s.devices = append(s.devices, d)
		device := &s.devices[len(s.devices)-1]
		if err := syscall.Fstat(deviceFD, &device.status); err != nil {
			return nil, err
		}
		major := (device.status.Rdev>>8)&0xfff | (device.status.Rdev>>32)&0xfffff000
		if device.status.Mode&syscall.S_IFMT != syscall.S_IFCHR || major != 13 || identities[device.status.Rdev] {
			return nil, errors.New("invalid input device identity")
		}
		if r.steamOutput {
			minor := (device.status.Rdev & 0xff) | ((device.status.Rdev >> 12) & 0xffffff00)
			expectedMinor, ok := eventMinor(name)
			var id [4]uint16
			if !ok || minor != expectedMinor || ioctl(deviceFD, 0x80084502, unsafe.Pointer(&id[0])) != nil ||
				id != [4]uint16{3, 0x28de, 0x11ff, 0} {
				return nil, errors.New("Steam output device identity is invalid")
			}
		}
		var version int32
		if err := ioctl(deviceFD, 0x80044501, unsafe.Pointer(&version)); err != nil || version != 0x10001 {
			return nil, errors.New("input alias is not evdev")
		}
		kernelName, err := kernelString(deviceFD, 0x06)
		wantName, wantPhys := expectedIdentity(seat, r)
		if err != nil || kernelName != wantName {
			return nil, errors.New("input belongs to another allocation")
		}
		phys, err := kernelString(deviceFD, 0x07)
		// inputtino's shared UHID path may omit phys. The authoritative host
		// contract permits that case; the exact hashed kernel name is required.
		if !validPhysicalIdentity(phys, err, wantPhys) {
			return nil, errors.New("input physical identity changed")
		}
		seen[name], identities[device.status.Rdev] = true, true
	}
	for _, name := range []string{"polaris-keyboard", "polaris-mouse-relative", "polaris-mouse-absolute"} {
		if !seen[name] {
			return nil, errors.New("required input alias missing")
		}
	}
	outputs := 0
	for _, device := range s.devices {
		if device.role.steamOutput {
			outputs++
		}
	}
	if outputs > 1 || (outputs == 1 && (!seen["polaris-gamepad-0"] || seen["polaris-gamepad-1"])) {
		return nil, errors.New("Steam output requires one allocated source gamepad")
	}
	missing := false
	for slot := 0; slot < 16; slot++ {
		if !seen[fmt.Sprintf("polaris-gamepad-%d", slot)] {
			missing = true
		} else if missing {
			return nil, errors.New("input gamepad allocation has a gap")
		}
	}
	if err := s.Verify(); err != nil {
		return nil, err
	}
	return s, nil
}

func validPhysicalIdentity(phys string, err error, expected string) bool {
	// evdev returns ENOENT when input_dev::phys is null. This is the same
	// optional field accepted by the host sysfs authority, not a missing node.
	if errors.Is(err, syscall.ENOENT) {
		return phys == ""
	}
	return err == nil && (phys == "" || phys == expected)
}

func (s *Set) Close() {
	if s == nil {
		return
	}
	for _, device := range s.devices {
		_ = device.file.Close()
	}
	if s.directory != nil {
		_ = s.directory.Close()
	}
}

func sameNode(a, b syscall.Stat_t) bool {
	return a.Dev == b.Dev && a.Ino == b.Ino && a.Rdev == b.Rdev && a.Mode == b.Mode
}

// Verify also fails when a retained virtual device has been removed at source.
func (s *Set) Verify() error {
	if s == nil || s.directory == nil {
		return errors.New("input allocation unavailable")
	}
	var status syscall.Stat_t
	if err := syscall.Lstat(s.path, &status); err != nil || !sameNode(status, s.status) {
		return errors.New("input namespace replaced")
	}
	entries, err := boundedNames(s.path, maximumDevices+1)
	if err != nil || len(entries) != len(s.devices) {
		return errors.New("input namespace changed")
	}
	for _, device := range s.devices {
		if err := syscall.Lstat(filepath.Join(s.path, device.role.name), &status); err != nil || !sameNode(status, device.status) {
			return errors.New("input alias replaced")
		}
		var version int32
		if err := ioctl(int(device.file.Fd()), 0x80044501, unsafe.Pointer(&version)); err != nil || version != 0x10001 {
			return errors.New("input device retired")
		}
	}
	return nil
}

func (s *Set) compositorDevices() ([]Device, error) {
	order := []string{"polaris-keyboard", "polaris-mouse-relative", "polaris-mouse-absolute"}
	selected := make([]Device, 3)
	seen := 0
	for _, device := range s.devices {
		if !device.role.compositor {
			continue
		} // Workload reads gamepads directly.
		index := -1
		for i, name := range order {
			if device.role.name == name {
				index = i
				break
			}
		}
		if index < 0 || selected[index].role.name != "" {
			return nil, errors.New("compositor input role is unsupported")
		}
		selected[index] = device
		seen++
	}
	if seen != 3 {
		return nil, errors.New("compositor input is incomplete")
	}
	return selected, nil
}

// CompositorFiles duplicates the verified descriptors in a fixed role order.
// The caller closes these copies after spawning its image-owned producer.
// Touch/pen need explicit mappings before this experiment may admit them.
func (s *Set) CompositorFiles() (_ []*os.File, result error) {
	if err := s.Verify(); err != nil {
		return nil, err
	}
	selected, err := s.compositorDevices()
	if err != nil {
		return nil, err
	}
	files := make([]*os.File, 0, len(selected))
	defer func() {
		if result != nil {
			for _, file := range files {
				_ = file.Close()
			}
		}
	}()
	for _, device := range selected {
		fd, _, errno := syscall.Syscall(syscall.SYS_FCNTL, device.file.Fd(), syscall.F_DUPFD_CLOEXEC, 0)
		if errno != 0 {
			return nil, errors.New("compositor input descriptor cannot be retained")
		}
		files = append(files, os.NewFile(fd, "compositor-input"))
	}
	if err := s.Verify(); err != nil {
		return nil, err
	}
	return files, nil
}

// VerifyConsumer requires the actual compositor process to retain all admitted
// input descriptors. Actual event delivery is a separate acceptance test.
func (s *Set) VerifyConsumer(pid int, pidFD int) error {
	if err := s.Verify(); err != nil {
		return err
	}
	if pid <= 0 || !consumerAlive(pidFD) {
		return errors.New("input consumer unavailable")
	}
	root := fmt.Sprintf("/proc/%d/fd", pid)
	entries, err := boundedNames(root, 4097)
	if err != nil || len(entries) > 4096 {
		return errors.New("input consumer descriptors unavailable")
	}
	opened := make(map[uint64]bool)
	for _, entry := range entries {
		var status syscall.Stat_t
		if syscall.Stat(filepath.Join(root, entry), &status) == nil && status.Mode&syscall.S_IFMT == syscall.S_IFCHR {
			if !readableConsumerFD(fmt.Sprintf("/proc/%d/fdinfo/%s", pid, entry)) {
				continue
			}
			var after syscall.Stat_t
			if syscall.Stat(filepath.Join(root, entry), &after) == nil && sameNode(status, after) {
				opened[status.Rdev] = true
			}
		}
	}
	for _, d := range s.devices {
		if d.role.compositor && !opened[d.status.Rdev] {
			return errors.New("compositor did not accept allocated input")
		}
	}
	if !consumerAlive(pidFD) {
		return errors.New("input consumer exited during verification")
	}
	return nil
}

func boundedNames(path string, limit int) ([]string, error) {
	fd, err := syscall.Open(path, syscall.O_RDONLY|syscall.O_DIRECTORY|syscall.O_NOFOLLOW|syscall.O_CLOEXEC, 0)
	if err != nil {
		return nil, err
	}
	file := os.NewFile(uintptr(fd), "bounded-input-directory")
	defer file.Close()
	names, err := file.Readdirnames(limit)
	if err == io.EOF {
		err = nil
	}
	return names, err
}

// A pidfd remains bound to the original child after wait/reap and PID reuse.
func consumerAlive(pidFD int) bool {
	if pidFD < 0 {
		return false
	}
	poll := struct {
		FD              int32
		Events, Revents int16
	}{FD: int32(pidFD), Events: 1}
	for {
		// Runtime preemption signals can interrupt a zero-timeout poll too.
		// Retry the same pinned lifetime; EINTR does not establish child death.
		timeout := syscall.Timespec{}
		poll.Revents = 0
		count, _, errno := syscall.Syscall6(syscall.SYS_PPOLL, uintptr(unsafe.Pointer(&poll)), 1, uintptr(unsafe.Pointer(&timeout)), 0, 0, 0)
		if errno == syscall.EINTR {
			continue
		}
		return errno == 0 && count == 0 && poll.Revents == 0
	}
}

func readableConsumerFD(path string) bool {
	fd, err := syscall.Open(path, syscall.O_RDONLY|syscall.O_NONBLOCK|syscall.O_NOFOLLOW|syscall.O_CLOEXEC, 0)
	if err != nil {
		return false
	}
	file := os.NewFile(uintptr(fd), "input-consumer-fdinfo")
	defer file.Close()
	content, err := io.ReadAll(io.LimitReader(file, 4097))
	if err != nil || len(content) > 4096 {
		return false
	}
	found := false
	for _, line := range strings.Split(string(content), "\n") {
		if !strings.HasPrefix(line, "flags:") {
			continue
		}
		if found {
			return false
		}
		found = true
		flags, err := strconv.ParseUint(strings.TrimSpace(strings.TrimPrefix(line, "flags:")), 8, 32)
		if err != nil || flags&0x200000 != 0 || flags&syscall.O_ACCMODE != syscall.O_RDONLY {
			return false
		}
	}
	return found
}

// SteamOutput returns only the retained, verified output owned by this seat.
// No caller-supplied path or input creation endpoint is accepted.
func (s *Set) SteamOutput() (string, string, error) {
	if err := s.Verify(); err != nil {
		return "", "", err
	}
	for _, device := range s.devices {
		if device.role.steamOutput {
			name, err := kernelString(int(device.file.Fd()), 0x06)
			if err != nil {
				return "", "", err
			}
			return filepath.Join(s.path, device.role.name), name, nil
		}
	}
	return "", "", nil
}

// SteamOutputSysname obtains UI_GET_SYSNAME from the verified device's sysfs
// entry. The workload cannot supply either the input identity or sysfs path.
func (s *Set) SteamOutputSysname() (string, error) {
	path, name, err := s.SteamOutput()
	if err != nil || path == "" {
		return "", errors.Join(err, errors.New("Steam output was not allocated"))
	}
	device, err := filepath.EvalSymlinks(filepath.Join("/sys/class/input", filepath.Base(path), "device"))
	if err != nil {
		return "", errors.New("Steam output sysfs identity is unavailable")
	}
	sysname := filepath.Base(device)
	number := strings.TrimPrefix(sysname, "input")
	id, err := strconv.ParseUint(number, 10, 32)
	if err != nil || sysname != "input"+strconv.FormatUint(id, 10) {
		return "", errors.New("Steam output sysfs name is invalid")
	}
	identity, err := os.ReadFile(filepath.Join(device, "name"))
	if err != nil || strings.TrimSuffix(string(identity), "\n") != name {
		return "", errors.New("Steam output sysfs identity differs")
	}
	if err := s.Verify(); err != nil {
		return "", err
	}
	return sysname, nil
}

// SteamOutputWriter retains the same device as SteamOutput for the broker.
// It cannot open an arbitrary path, and rechecks the complete allocation around
// reopening the sole output with write access.
func (s *Set) SteamOutputWriter() (*os.File, error) {
	if err := s.Verify(); err != nil {
		return nil, err
	}
	for _, device := range s.devices {
		if !device.role.steamOutput {
			continue
		}
		fd, err := syscall.Openat(int(s.directory.Fd()), device.role.name,
			syscall.O_RDWR|syscall.O_NONBLOCK|syscall.O_CLOEXEC|syscall.O_NOFOLLOW, 0)
		if err != nil {
			return nil, err
		}
		file := os.NewFile(uintptr(fd), "steam-output")
		var status syscall.Stat_t
		if err := syscall.Fstat(fd, &status); err != nil || !sameNode(status, device.status) {
			_ = file.Close()
			return nil, errors.New("Steam output changed during admission")
		}
		if err := s.Verify(); err != nil {
			_ = file.Close()
			return nil, err
		}
		return file, nil
	}
	return nil, errors.New("Steam output was not allocated")
}
