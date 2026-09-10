//go:build linux

package main

import (
	"os"
	"path/filepath"
	"syscall"
	"testing"
	"time"
)

func TestHealthPrivateFilesRejectFIFOWithoutWriter(t *testing.T) {
	readers := []struct {
		name string
		read func(string) error
	}{
		{"capability", func(path string) error { _, err := readCapability(path, uint32(os.Geteuid())); return err }},
		{"ready", func(path string) error { _, err := readReadyState(path, uint32(os.Geteuid())); return err }},
	}
	for _, reader := range readers {
		t.Run(reader.name, func(t *testing.T) {
			path := filepath.Join(t.TempDir(), "private-file")
			if err := syscall.Mkfifo(path, 0600); err != nil {
				t.Fatal(err)
			}
			done := make(chan error, 1)
			go func() { done <- reader.read(path) }()
			select {
			case err := <-done:
				if err == nil {
					t.Fatal("accepted FIFO as private regular file")
				}
			case <-time.After(500 * time.Millisecond):
				// Unblock the old implementation before reporting failure so
				// the regression itself does not leave a blocked goroutine.
				writer, err := syscall.Open(path, syscall.O_WRONLY|syscall.O_NONBLOCK|syscall.O_CLOEXEC, 0)
				if err == nil {
					_ = syscall.Close(writer)
				}
				select {
				case <-done:
				case <-time.After(time.Second):
					t.Fatal("FIFO reader could not be retired")
				}
				t.Fatal("private file validation blocked waiting for a FIFO writer")
			}
		})
	}
}
