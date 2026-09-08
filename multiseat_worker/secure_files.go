//go:build linux

package main

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"os"
	"path/filepath"
	"syscall"
	"time"
)

type readyState struct {
	Version         int    `json:"version"`
	PID             int    `json:"pid"`
	ControllerEpoch string `json:"controller_epoch"`
	LogicalGPU      string `json:"logical_gpu"`
	Slot            uint32 `json:"slot"`
	Generation      uint64 `json:"generation"`
	WorkerName      string `json:"worker_name"`
}

type fileIdentity struct {
	device uint64
	inode  uint64
}

func identityOf(path string) (fileIdentity, error) {
	info, err := os.Lstat(path)
	if err != nil {
		return fileIdentity{}, err
	}
	metadata, ok := info.Sys().(*syscall.Stat_t)
	if !ok {
		return fileIdentity{}, errors.New("file identity is unavailable")
	}
	return fileIdentity{device: uint64(metadata.Dev), inode: metadata.Ino}, nil
}

func removeExact(path string, expected fileIdentity) {
	observed, err := identityOf(path)
	if err == nil && observed == expected {
		_ = os.Remove(path)
	}
}

func privateDirectory(path string, expectedUID uint32) error {
	if !filepath.IsAbs(path) || filepath.Clean(path) != path {
		return errors.New("private directory path is invalid")
	}
	info, err := os.Lstat(path)
	if err != nil {
		return errors.New("private directory is inaccessible")
	}
	metadata, ok := info.Sys().(*syscall.Stat_t)
	if !ok || !info.IsDir() || metadata.Uid != expectedUID || info.Mode().Perm() != 0o700 ||
		info.Mode()&(os.ModeSetuid|os.ModeSetgid|os.ModeSticky) != 0 {
		return errors.New("private directory ownership or mode is unsafe")
	}
	return nil
}

func openPrivateRegular(path string, expectedUID uint32, maxBytes int64) (*os.File, error) {
	descriptor, err := syscall.Open(path, syscall.O_RDONLY|syscall.O_CLOEXEC|syscall.O_NOFOLLOW, 0)
	if err != nil {
		return nil, errors.New("private file is inaccessible")
	}
	file := os.NewFile(uintptr(descriptor), filepath.Base(path))
	if file == nil {
		_ = syscall.Close(descriptor)
		return nil, errors.New("private file descriptor is invalid")
	}
	metadata := syscall.Stat_t{}
	if err := syscall.Fstat(descriptor, &metadata); err != nil ||
		metadata.Mode&syscall.S_IFMT != syscall.S_IFREG || metadata.Uid != expectedUID ||
		metadata.Mode&0o077 != 0 || metadata.Mode&syscall.S_IRUSR == 0 ||
		metadata.Mode&(syscall.S_IXUSR|syscall.S_ISUID|syscall.S_ISGID|syscall.S_ISVTX) != 0 ||
		metadata.Size < 0 || metadata.Size > maxBytes {
		_ = file.Close()
		return nil, errors.New("private file ownership, mode, or size is unsafe")
	}
	return file, nil
}

func readCapability(path string, expectedUID uint32) ([capabilitySize]byte, error) {
	var empty [capabilitySize]byte
	file, err := openPrivateRegular(path, expectedUID, capabilitySize*2+1)
	if err != nil {
		return empty, err
	}
	defer file.Close()
	encoded, err := io.ReadAll(io.LimitReader(file, capabilitySize*2+2))
	if err != nil {
		return empty, errors.New("capability cannot be read")
	}
	encoded = bytes.TrimSuffix(encoded, []byte{'\n'})
	return parseCapability(string(encoded))
}

func stateFor(config workerConfig, pid int) readyState {
	return readyState{
		Version:         1,
		PID:             pid,
		ControllerEpoch: config.Identity.ControllerEpoch,
		LogicalGPU:      config.Identity.LogicalGPU,
		Slot:            config.Identity.Slot,
		Generation:      config.Identity.Generation,
		WorkerName:      config.Identity.WorkerName,
	}
}

func writeReadyState(path string, config workerConfig, expectedUID uint32) (fileIdentity, error) {
	directory := filepath.Dir(path)
	if err := privateDirectory(directory, expectedUID); err != nil {
		return fileIdentity{}, err
	}
	payload, err := json.Marshal(stateFor(config, os.Getpid()))
	if err != nil || len(payload) > 4096 {
		return fileIdentity{}, errors.New("worker ready state cannot be encoded")
	}
	temporary, err := os.CreateTemp(directory, ".seat-worker-ready-")
	if err != nil {
		return fileIdentity{}, errors.New("worker ready state cannot be created")
	}
	temporaryPath := temporary.Name()
	committed := false
	defer func() {
		_ = temporary.Close()
		if !committed {
			_ = os.Remove(temporaryPath)
		}
	}()
	if err := temporary.Chmod(0o600); err != nil {
		return fileIdentity{}, errors.New("worker ready state mode cannot be set")
	}
	if _, err := temporary.Write(payload); err != nil || temporary.Sync() != nil || temporary.Close() != nil {
		return fileIdentity{}, errors.New("worker ready state cannot be written")
	}
	if err := os.Rename(temporaryPath, path); err != nil {
		return fileIdentity{}, errors.New("worker ready state cannot be committed")
	}
	committed = true
	identity, err := identityOf(path)
	if err != nil {
		return fileIdentity{}, errors.New("worker ready state identity cannot be read")
	}
	return identity, nil
}

func readReadyState(path string, expectedUID uint32) (readyState, error) {
	var state readyState
	file, err := openPrivateRegular(path, expectedUID, 4096)
	if err != nil {
		return state, err
	}
	defer file.Close()
	decoder := json.NewDecoder(io.LimitReader(file, 4097))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&state); err != nil {
		return state, errors.New("worker ready state is malformed")
	}
	var trailing any
	if err := decoder.Decode(&trailing); err != io.EOF {
		return state, errors.New("worker ready state has trailing data")
	}
	return state, nil
}

func socketReady(path string, expectedUID uint32) bool {
	info, err := os.Lstat(path)
	if err != nil || info.Mode()&os.ModeSocket == 0 || info.Mode().Perm() != 0o600 ||
		info.Mode()&(os.ModeSetuid|os.ModeSetgid|os.ModeSticky) != 0 {
		return false
	}
	metadata, ok := info.Sys().(*syscall.Stat_t)
	return ok && metadata.Uid == expectedUID
}

func checkHealth(config workerConfig, paths workerPaths, expectedUID uint32) error {
	ctx, cancel := context.WithTimeout(context.Background(), 2*handshakeTimeout)
	defer cancel()
	return checkHealthContext(ctx, config, paths, expectedUID)
}

func checkHealthContext(ctx context.Context, config workerConfig, paths workerPaths, expectedUID uint32) error {
	if err := ctx.Err(); err != nil {
		return err
	}
	if err := privateDirectory(paths.IPC, expectedUID); err != nil {
		return err
	}
	if err := privateDirectory(paths.Auth, expectedUID); err != nil {
		return err
	}
	if err := privateDirectory(paths.State, expectedUID); err != nil {
		return err
	}
	capability, err := readCapability(filepath.Join(paths.Auth, capabilityFileName), expectedUID)
	if err != nil {
		return err
	}
	state, err := readReadyState(filepath.Join(paths.State, readyFileName), expectedUID)
	if err != nil {
		return err
	}
	if state != stateFor(config, state.PID) || state.PID <= 0 {
		return errors.New("worker ready state does not match this generation")
	}
	if err := syscall.Kill(state.PID, 0); err != nil {
		return errors.New("worker process is not alive")
	}
	if !socketReady(filepath.Join(paths.IPC, controlSocketName), expectedUID) ||
		!socketReady(filepath.Join(paths.IPC, mediaSocketName), expectedUID) {
		return errors.New("worker IPC sockets are not ready")
	}
	if err := probeWorkerSocket(
		ctx,
		filepath.Join(paths.IPC, controlSocketName),
		config,
		capability,
		channelControl,
		expectedUID,
	); err != nil {
		return err
	}
	if err := probeWorkerSocket(
		ctx,
		filepath.Join(paths.IPC, mediaSocketName),
		config,
		capability,
		channelMedia,
		expectedUID,
	); err != nil {
		return err
	}
	return nil
}

func removeOwnedSocket(path string, expectedUID uint32) error {
	info, err := os.Lstat(path)
	if os.IsNotExist(err) {
		return nil
	}
	if err != nil || info.Mode()&os.ModeSocket == 0 {
		return errors.New("refusing to replace a non-socket IPC path")
	}
	metadata, ok := info.Sys().(*syscall.Stat_t)
	if !ok || metadata.Uid != expectedUID {
		return errors.New("refusing to replace an unowned IPC socket")
	}
	connection, dialError := net.DialTimeout("unix", path, 100*time.Millisecond)
	if dialError == nil {
		_ = connection.Close()
		return errors.New("refusing to replace an active IPC socket")
	}
	if !errors.Is(dialError, syscall.ECONNREFUSED) {
		return errors.New("refusing to replace an IPC socket with uncertain liveness")
	}
	if err := os.Remove(path); err != nil {
		return fmt.Errorf("remove stale IPC socket: %w", err)
	}
	return nil
}
