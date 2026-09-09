//go:build linux

package seatprovider

import (
	"bufio"
	"context"
	"encoding/hex"
	"errors"
	"io"
	"net"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"time"

	"github.com/papi-ux/polaris/multiseat_worker/internal/seatruntime"
)

type sessionBusArtifactSpec struct {
	relative string
	mode     uint32
	required bool
}

var sessionBusArtifactSpecs = []sessionBusArtifactSpec{
	{relative: "bus", mode: syscall.S_IFSOCK, required: true},
	{relative: "dbus-1/services", mode: syscall.S_IFDIR},
	{relative: "dbus-1", mode: syscall.S_IFDIR},
}

func RunSessionBus(arguments []string, environment []string) error {
	request, err := parseProviderInvocation(
		seatruntime.StageSessionBus,
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
	options, err := normalizeProviderOptions(defaultProviderOptions())
	if err != nil {
		_ = ready.Close()
		return err
	}
	context, cancel := signalContext()
	defer cancel()
	return runSessionBus(context, request, ready, options)
}

func readBoundedLine(file *os.File, timeout time.Duration, maximum int) (string, error) {
	if file == nil || timeout <= 0 || maximum <= 0 {
		return "", errors.New("runtime provider child output is invalid")
	}
	if err := file.SetReadDeadline(time.Now().Add(timeout)); err != nil {
		return "", errors.New("runtime provider child output deadline failed")
	}
	reader := bufio.NewReader(io.LimitReader(file, int64(maximum+1)))
	line, err := reader.ReadString('\n')
	if err != nil || len(line) > maximum {
		return "", errors.New("runtime provider child output is invalid")
	}
	return line, nil
}

func readBoundedLineContext(
	parent context.Context,
	file *os.File,
	timeout time.Duration,
	maximum int,
) (string, error) {
	if parent == nil {
		return "", errors.New("runtime provider child output context is missing")
	}
	type lineResult struct {
		line string
		err  error
	}
	result := make(chan lineResult, 1)
	go func() {
		line, err := readBoundedLine(file, timeout, maximum)
		result <- lineResult{line: line, err: err}
	}()
	select {
	case received := <-result:
		return received.line, received.err
	case <-parent.Done():
		_ = file.Close()
		return "", errors.New("runtime provider child output was canceled")
	}
}

func validGUID(value string) bool {
	if len(value) != 32 {
		return false
	}
	for _, character := range []byte(value) {
		if character < '0' || character > '9' {
			if character < 'a' || character > 'f' {
				return false
			}
		}
	}
	return true
}

func validateBusAddress(line string, socketPath string) (string, error) {
	prefix := "unix:path=" + socketPath + ",guid="
	if !strings.HasPrefix(line, prefix) || !strings.HasSuffix(line, "\n") {
		return "", errors.New("runtime session bus address is invalid")
	}
	guid := strings.TrimSuffix(strings.TrimPrefix(line, prefix), "\n")
	if !validGUID(guid) || line != prefix+guid+"\n" {
		return "", errors.New("runtime session bus address is invalid")
	}
	return guid, nil
}

func probeSessionBus(socketPath string, guid string, uid uint32, timeout time.Duration) error {
	connection, err := net.DialTimeout("unix", socketPath, timeout)
	if err != nil {
		return errors.New("runtime session bus authentication failed")
	}
	defer connection.Close()
	if err := connection.SetDeadline(time.Now().Add(timeout)); err != nil {
		return errors.New("runtime session bus authentication failed")
	}
	identity := strconv.FormatUint(uint64(uid), 10)
	external := hex.EncodeToString([]byte(identity))
	if _, err := io.WriteString(connection, "\x00AUTH EXTERNAL "+external+"\r\n"); err != nil {
		return errors.New("runtime session bus authentication failed")
	}
	response, err := bufio.NewReader(io.LimitReader(connection, 129)).ReadString('\n')
	if err != nil || response != "OK "+guid+"\r\n" {
		return errors.New("runtime session bus authentication failed")
	}
	if _, err := io.WriteString(connection, "BEGIN\r\n"); err != nil {
		return errors.New("runtime session bus authentication failed")
	}
	return nil
}

func sessionBusEnvironment(runtimePath string) []string {
	return []string{
		"LC_ALL=C",
		"HOME=/nonexistent",
		"XDG_RUNTIME_DIR=" + runtimePath,
		"DBUS_SESSION_BUS_ADDRESS=unix:path=" + filepath.Join(runtimePath, "bus"),
	}
}

func validSessionBusArtifact(
	identity artifactIdentity,
	spec sessionBusArtifactSpec,
	uid uint32,
) bool {
	return identity.mode&syscall.S_IFMT == spec.mode && identity.uid == uid &&
		(spec.mode != syscall.S_IFDIR || identity.mode&0o7777 == 0o700)
}

func captureSessionBusArtifacts(
	runtime *runtimeDirectory,
) (map[string]artifactIdentity, error) {
	if err := runtime.verify(); err != nil {
		return nil, err
	}
	captured := make(map[string]artifactIdentity, len(sessionBusArtifactSpecs))
	for _, spec := range sessionBusArtifactSpecs {
		identity, err := runtime.pins.capture(filepath.Join(runtime.path, spec.relative))
		if errors.Is(err, os.ErrNotExist) && !spec.required {
			continue
		}
		if err != nil || !validSessionBusArtifact(identity, spec, runtime.uid) {
			return nil, errors.New("runtime session bus artifact set is invalid")
		}
		captured[spec.relative] = identity
	}
	return captured, nil
}

func cleanupSessionBus(
	runtime *runtimeDirectory,
	known map[string]artifactIdentity,
	allowOwnedCapture bool,
) error {
	if runtime == nil {
		return errors.New("runtime session bus cleanup is invalid")
	}
	for _, spec := range sessionBusArtifactSpecs {
		if err := runtime.verify(); err != nil {
			return err
		}
		path := filepath.Join(runtime.path, spec.relative)
		identity, err := runtime.pins.capture(path)
		if errors.Is(err, os.ErrNotExist) {
			continue
		}
		if err != nil || !validSessionBusArtifact(identity, spec, runtime.uid) {
			return errors.New("runtime session bus cleanup found an unexpected artifact")
		}
		captured, present := known[spec.relative]
		if present {
			if !sameIdentity(identity, captured) {
				return errors.New("runtime session bus artifact identity changed")
			}
		} else if !allowOwnedCapture {
			return errors.New("runtime session bus cleanup found an unowned artifact")
		}
		if spec.mode == syscall.S_IFDIR {
			entries, err := os.ReadDir(path)
			if err != nil || len(entries) != 0 {
				return errors.New("runtime session bus cleanup found unexpected directory content")
			}
		}
		if err := runtime.verify(); err != nil {
			return err
		}
		if err := os.Remove(path); err != nil {
			return errors.New("runtime session bus artifact could not be removed")
		}
		if _, err := runtime.pins.capture(path); !errors.Is(err, os.ErrNotExist) {
			return errors.New("runtime session bus artifact removal was not durable")
		}
	}
	return nil
}

func runSessionBus(
	parent context.Context,
	request seatruntime.Request,
	ready io.WriteCloser,
	options providerOptions,
) (result error) {
	if parent == nil || ready == nil || request.Stage != seatruntime.StageSessionBus {
		if ready != nil {
			_ = ready.Close()
		}
		return errors.New("runtime session bus provider is invalid")
	}
	defer func() {
		if ready != nil {
			_ = ready.Close()
		}
	}()
	if _, err := seatruntime.Arguments(request); err != nil {
		return errors.New("runtime session bus request is invalid")
	}
	options, err := normalizeProviderOptions(options)
	if err != nil {
		return err
	}
	select {
	case <-parent.Done():
		return errors.New("runtime session bus startup was canceled")
	default:
	}
	runtime, err := openRuntimeDirectory(options.runtimeDirectory, options.runtimeOwnerUID)
	if err != nil {
		return err
	}
	defer runtime.close()
	busPath := filepath.Join(runtime.path, "bus")
	for _, spec := range sessionBusArtifactSpecs {
		if _, err := lstatIdentity(filepath.Join(runtime.path, spec.relative)); !errors.Is(err, os.ErrNotExist) {
			return errors.New("runtime session bus artifact already exists")
		}
	}
	addressReader, addressWriter, err := os.Pipe()
	if err != nil {
		return errors.New("runtime session bus address pipe could not be created")
	}
	defer addressReader.Close()
	child, err := startManagedChild(
		options.dbusDaemonPath,
		options.executableOwnerUID,
		[]string{
			"--session",
			"--nofork",
			"--nopidfile",
			"--nosyslog",
			"--address=unix:path=" + busPath,
			"--print-address=4",
		},
		sessionBusEnvironment(runtime.path),
		[]*os.File{addressWriter},
	)
	_ = addressWriter.Close()
	if err != nil {
		return err
	}
	var artifacts map[string]artifactIdentity
	readyPublished := false
	defer func() {
		stopError := child.stop(options.stopTimeout)
		cleanupError := cleanupSessionBus(runtime, artifacts, !readyPublished)
		result = errors.Join(result, stopError, cleanupError)
	}()
	address, err := readBoundedLineContext(
		parent,
		addressReader,
		options.startupTimeout,
		512,
	)
	if err != nil {
		return err
	}
	guid, err := validateBusAddress(address, busPath)
	if err != nil {
		return err
	}
	if err := runtime.verify(); err != nil {
		return err
	}
	if err := probeSessionBus(
		busPath,
		guid,
		options.runtimeOwnerUID,
		options.probeTimeout,
	); err != nil {
		return err
	}
	artifacts, err = captureSessionBusArtifacts(runtime)
	if err != nil {
		return err
	}
	if child.exited() {
		return errors.New("runtime session bus exited before readiness")
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
		return errors.New("runtime session bus exited unexpectedly")
	}
}
