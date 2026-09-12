//go:build linux

package seatprovider

import (
	"context"
	"io"
	"net"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/papi-ux/polaris/multiseat_worker/internal/seatmedia"
	"github.com/papi-ux/polaris/multiseat_worker/internal/seatruntime"
)

// This explicitly selected test uses real software codecs and provider IPC.
// It never opens a display, GPU, audio device, or virtual input node.
func TestRealEncoderBridgeKeepsTwoSeatsIndependent(t *testing.T) {
	helper := os.Getenv("POLARIS_ENCODER_TEST_HELPER")
	if helper == "" {
		t.Skip("requires the built native software encoder helper")
	}
	if !filepath.IsAbs(helper) {
		t.Fatal("native encoder helper must be absolute")
	}
	type seat struct {
		connection *net.UnixConn
		cancel     context.CancelFunc
		done       chan error
		path       string
	}
	start := func(namespace string) seat {
		root, err := os.MkdirTemp("/tmp", "eb-")
		if err != nil {
			t.Fatal(err)
		}
		wrapper := filepath.Join(root, "encoder")
		script := "#!/bin/sh\nexec '" + strings.ReplaceAll(helper, "'", "'\\''") + "' --self-test <&5 >&4\n"
		if err := os.WriteFile(wrapper, []byte(script), 0o700); err != nil {
			t.Fatal(err)
		}
		options := defaultProviderOptions()
		options.runtimeDirectory = root
		options.executableOwnerUID = uint32(os.Geteuid())
		options.encoderPath = wrapper
		request := encoderTestRequest()
		request.RuntimeNamespace = namespace
		request.DisplayWidth = 640
		request.DisplayHeight = 480
		reader, writer, err := os.Pipe()
		if err != nil {
			t.Fatal(err)
		}
		ctx, cancel := context.WithCancel(t.Context())
		done := make(chan error, 1)
		go func() { done <- runEncoder(ctx, request, writer, options) }()
		t.Cleanup(func() { cancel(); reader.Close(); os.Remove(wrapper); os.Remove(root) })
		reader.SetReadDeadline(time.Now().Add(10 * time.Second))
		ready := make([]byte, len(seatruntime.ReadyRecord))
		if _, err := io.ReadFull(reader, ready); err != nil {
			t.Fatal(err)
		}
		if string(ready) != seatruntime.ReadyRecord {
			t.Fatal("invalid provider readiness")
		}
		reader.Close()
		name, _ := seatruntime.EncodedMediaSocketName(namespace)
		path := filepath.Join(root, name)
		connection, err := net.DialUnix("unix", nil, &net.UnixAddr{Name: path, Net: "unix"})
		if err != nil {
			t.Fatal(err)
		}
		t.Cleanup(func() { connection.Close() })
		connection.SetReadDeadline(time.Now().Add(3 * time.Second))
		kind, body, err := seatmedia.Read(connection)
		if err != nil || kind != seatmedia.Config || len(body) != 32 {
			t.Fatal("provider contract unavailable", err)
		}
		if _, err := connection.Write([]byte{seatmedia.Start}); err != nil {
			t.Fatal(err)
		}
		return seat{connection, cancel, done, path}
	}
	first, second := start("encoder-a"), start("encoder-b")
	nextVideo := func(value seat) {
		value.connection.SetReadDeadline(time.Now().Add(3 * time.Second))
		for i := 0; i < 100; i++ {
			kind, _, err := seatmedia.Read(value.connection)
			if err != nil {
				t.Fatal(err)
			}
			if kind == seatmedia.Video {
				return
			}
		}
		t.Fatal("no video from seat")
	}
	nextVideo(first)
	nextVideo(second)
	first.cancel()
	select {
	case err := <-first.done:
		if err != nil {
			t.Fatal("first seat teardown failed", err)
		}
	case <-time.After(3 * time.Second):
		t.Fatal("first seat did not stop")
	}
	if _, err := os.Lstat(first.path); !os.IsNotExist(err) {
		t.Fatal("retired encoder socket remains", err)
	}
	nextVideo(second)
	second.cancel()
	select {
	case err := <-second.done:
		if err != nil {
			t.Fatal("second seat teardown failed", err)
		}
	case <-time.After(3 * time.Second):
		t.Fatal("second seat did not stop")
	}
}
