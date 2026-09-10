//go:build linux

package main

import (
	"context"
	"errors"
	"net"
	"os"
	"path/filepath"
	"syscall"
	"testing"
	"time"
)

func TestWorkerHealthCancellationStopsChallengeRead(t *testing.T) {
	path := filepath.Join(t.TempDir(), "health.sock")
	listener, err := net.ListenUnix("unix", &net.UnixAddr{Name: path, Net: "unix"})
	if err != nil {
		t.Fatal(err)
	}
	defer listener.Close()
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	done := make(chan error, 1)
	go func() {
		done <- probeWorkerSocket(ctx, path, workerConfig{}, goldenCapability(), channelControl, uint32(os.Geteuid()))
	}()
	if err := listener.SetDeadline(time.Now().Add(time.Second)); err != nil {
		t.Fatal(err)
	}
	connection, err := listener.AcceptUnix()
	if err != nil {
		t.Fatal(err)
	}
	defer connection.Close()
	// The real connection is established but its peer never sends a challenge.
	cancel()
	select {
	case err := <-done:
		if err == nil {
			t.Fatal("accepted canceled health check")
		}
	case <-time.After(500 * time.Millisecond):
		t.Fatal("health challenge ignored operation cancellation")
	}
}

func TestWorkerHealthDeadlineIncludesFullConnectBacklog(t *testing.T) {
	path := filepath.Join(t.TempDir(), "health.sock")
	listener, err := syscall.Socket(syscall.AF_UNIX, syscall.SOCK_STREAM|syscall.SOCK_NONBLOCK|syscall.SOCK_CLOEXEC, 0)
	if err != nil {
		t.Fatal(err)
	}
	defer syscall.Close(listener)
	address := &syscall.SockaddrUnix{Name: path}
	if err := syscall.Bind(listener, address); err != nil {
		t.Fatal(err)
	}
	if err := syscall.Listen(listener, 0); err != nil {
		t.Fatal(err)
	}
	full := false
	for i := 0; i < 4; i++ {
		connection, err := syscall.Socket(syscall.AF_UNIX, syscall.SOCK_STREAM|syscall.SOCK_NONBLOCK|syscall.SOCK_CLOEXEC, 0)
		if err != nil {
			t.Fatal(err)
		}
		defer syscall.Close(connection)
		err = syscall.Connect(connection, address)
		if errors.Is(err, syscall.EAGAIN) {
			full = true
			break
		}
		if err != nil {
			t.Fatal(err)
		}
	}
	if !full {
		t.Fatal("failed to establish an actually full Unix listener backlog")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 50*time.Millisecond)
	defer cancel()
	done := make(chan error, 1)
	go func() {
		done <- probeWorkerSocket(ctx, path, workerConfig{}, goldenCapability(), channelControl, uint32(os.Geteuid()))
	}()
	select {
	case err := <-done:
		if err == nil {
			t.Fatal("accepted unserved health check")
		}
	case <-time.After(500 * time.Millisecond):
		t.Fatal("health connect escaped the operation deadline")
	}
}
