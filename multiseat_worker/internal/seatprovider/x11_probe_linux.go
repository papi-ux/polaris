//go:build linux

package seatprovider

import (
	"context"
	"encoding/binary"
	"errors"
	"io"
	"net"
	"os"
	"path/filepath"
	"strconv"
	"syscall"
	"time"
)

const maximumX11SetupSize = 1024 * 1024

type fixedDirectory struct {
	path string
	file *os.File
	dev  uint64
	ino  uint64
	uid  uint32
	mode uint32
	pins artifactPins
}

func openFixedDirectory(
	path string,
	expectedUID uint32,
	expectedMode uint32,
) (*fixedDirectory, error) {
	if !validAbsolutePath(path) || expectedMode == 0 || expectedMode > 0o7777 {
		return nil, errors.New("runtime fixed directory expectation is invalid")
	}
	descriptor, err := syscall.Open(
		path,
		syscall.O_RDONLY|syscall.O_DIRECTORY|syscall.O_NOFOLLOW|syscall.O_CLOEXEC,
		0,
	)
	if err != nil {
		return nil, errors.New("runtime fixed directory is unavailable")
	}
	file := os.NewFile(uintptr(descriptor), "polaris-fixed-directory")
	if file == nil {
		_ = syscall.Close(descriptor)
		return nil, errors.New("runtime fixed directory is unavailable")
	}
	var status syscall.Stat_t
	if err := syscall.Fstat(descriptor, &status); err != nil ||
		status.Mode&syscall.S_IFMT != syscall.S_IFDIR ||
		status.Uid != expectedUID || status.Mode&0o7777 != expectedMode {
		_ = file.Close()
		return nil, errors.New("runtime fixed directory ownership or mode is invalid")
	}
	directory := &fixedDirectory{
		path: path,
		file: file,
		dev:  uint64(status.Dev),
		ino:  status.Ino,
		uid:  expectedUID,
		mode: expectedMode,
	}
	if err := directory.verify(); err != nil {
		_ = file.Close()
		return nil, err
	}
	return directory, nil
}

func (directory *fixedDirectory) verify() error {
	if directory == nil || directory.file == nil {
		return errors.New("runtime fixed directory is invalid")
	}
	var status syscall.Stat_t
	if err := syscall.Lstat(directory.path, &status); err != nil ||
		status.Mode&syscall.S_IFMT != syscall.S_IFDIR ||
		status.Uid != directory.uid || status.Mode&0o7777 != directory.mode ||
		uint64(status.Dev) != directory.dev || status.Ino != directory.ino {
		return errors.New("runtime fixed directory identity changed")
	}
	return nil
}

func (directory *fixedDirectory) close() {
	if directory != nil && directory.file != nil {
		directory.pins.close()
		_ = directory.file.Close()
	}
}

func x11DisplayPaths(
	socketDirectory string,
	lockDirectory string,
	display uint32,
) (string, string, error) {
	if !validAbsolutePath(socketDirectory) || !validAbsolutePath(lockDirectory) ||
		display > 32 {
		return "", "", errors.New("runtime X11 display is invalid")
	}
	decimal := strconv.FormatUint(uint64(display), 10)
	return filepath.Join(socketDirectory, "X"+decimal),
		filepath.Join(lockDirectory, ".X"+decimal+"-lock"), nil
}

func readIdentityBounded(
	path string,
	expected artifactIdentity,
	maximum int,
) ([]byte, error) {
	if !validAbsolutePath(path) || maximum <= 0 {
		return nil, errors.New("runtime artifact read is invalid")
	}
	descriptor, err := syscall.Open(
		path,
		syscall.O_RDONLY|syscall.O_NOFOLLOW|syscall.O_CLOEXEC,
		0,
	)
	if err != nil {
		return nil, errors.New("runtime artifact is unavailable")
	}
	file := os.NewFile(uintptr(descriptor), "polaris-runtime-artifact")
	if file == nil {
		_ = syscall.Close(descriptor)
		return nil, errors.New("runtime artifact is unavailable")
	}
	defer file.Close()
	var status syscall.Stat_t
	if err := syscall.Fstat(descriptor, &status); err != nil ||
		!sameIdentity(expected, artifactIdentity{
			dev:  uint64(status.Dev),
			ino:  status.Ino,
			mode: status.Mode,
			uid:  status.Uid,
		}) {
		return nil, errors.New("runtime artifact identity changed")
	}
	content, err := io.ReadAll(io.LimitReader(file, int64(maximum+1)))
	if err != nil || len(content) > maximum {
		return nil, errors.New("runtime artifact content is invalid")
	}
	return content, nil
}

func validateX11Lock(
	path string,
	identity artifactIdentity,
	expectedPID int,
) error {
	if expectedPID <= 0 || identity.mode&syscall.S_IFMT != syscall.S_IFREG ||
		identity.mode&0o077 != 0 {
		return errors.New("runtime X11 lock is invalid")
	}
	content, err := readIdentityBounded(path, identity, 11)
	if err != nil || len(content) != 11 || content[10] != 0 {
		return errors.New("runtime X11 lock is invalid")
	}
	expected := []byte("          ")
	decimal := strconv.Itoa(expectedPID)
	if len(decimal) > len(expected) {
		return errors.New("runtime X11 lock is invalid")
	}
	copy(expected[len(expected)-len(decimal):], decimal)
	if string(content[:10]) != string(expected) {
		return errors.New("runtime X11 lock owner is invalid")
	}
	return nil
}

type x11ProbeExpectation struct {
	width   uint32
	height  uint32
	peerPID int
	peerUID uint32
}

func probeX11Display(
	parent context.Context,
	socketPath string,
	expectation x11ProbeExpectation,
	timeout time.Duration,
) error {
	if parent == nil || !validUnixSocketPath(socketPath) || timeout <= 0 ||
		expectation.width == 0 || expectation.width > 65535 ||
		expectation.height == 0 || expectation.height > 65535 ||
		expectation.peerPID <= 0 {
		return errors.New("runtime X11 probe is invalid")
	}
	dialer := net.Dialer{Timeout: timeout}
	connectionValue, err := dialer.DialContext(parent, "unix", socketPath)
	if err != nil {
		return errors.New("runtime X11 display is unavailable")
	}
	connection, ok := connectionValue.(*net.UnixConn)
	if !ok {
		_ = connectionValue.Close()
		return errors.New("runtime X11 display is invalid")
	}
	defer connection.Close()
	if err := connection.SetDeadline(time.Now().Add(timeout)); err != nil {
		return errors.New("runtime X11 probe deadline failed")
	}
	if err := verifyUnixPeer(
		connection,
		expectation.peerPID,
		expectation.peerUID,
	); err != nil {
		return errors.New("runtime X11 peer identity is invalid")
	}
	request := []byte{
		'l', 0,
		11, 0,
		0, 0,
		0, 0,
		0, 0,
		0, 0,
	}
	for len(request) > 0 {
		written, err := connection.Write(request)
		if err != nil || written <= 0 || written > len(request) {
			return errors.New("runtime X11 setup request failed")
		}
		request = request[written:]
	}
	var header [8]byte
	if _, err := io.ReadFull(connection, header[:]); err != nil ||
		header[0] != 1 || header[1] != 0 ||
		binary.LittleEndian.Uint16(header[2:4]) != 11 ||
		binary.LittleEndian.Uint16(header[4:6]) != 0 {
		return errors.New("runtime X11 setup response is invalid")
	}
	length := int(binary.LittleEndian.Uint16(header[6:8])) * 4
	if length < 40 || length > maximumX11SetupSize {
		return errors.New("runtime X11 setup response is invalid")
	}
	payload := make([]byte, length)
	if _, err := io.ReadFull(connection, payload); err != nil {
		return errors.New("runtime X11 setup response is incomplete")
	}
	vendorLength := int(binary.LittleEndian.Uint16(payload[16:18]))
	rootCount := int(payload[20])
	formatCount := int(payload[21])
	if rootCount != 1 || formatCount == 0 {
		return errors.New("runtime X11 screen set is invalid")
	}
	vendorPadded := (vendorLength + 3) &^ 3
	screenOffset := 32 + vendorPadded + formatCount*8
	if vendorPadded < vendorLength || screenOffset < 32 ||
		screenOffset+40 > len(payload) {
		return errors.New("runtime X11 screen is invalid")
	}
	width := uint32(binary.LittleEndian.Uint16(payload[screenOffset+20 : screenOffset+22]))
	height := uint32(binary.LittleEndian.Uint16(payload[screenOffset+22 : screenOffset+24]))
	if width != expectation.width || height != expectation.height {
		return errors.New("runtime X11 screen does not match the allocation")
	}
	return nil
}
