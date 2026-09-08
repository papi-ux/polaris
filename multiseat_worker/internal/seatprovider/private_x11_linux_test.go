//go:build linux

package seatprovider

import (
	"os"
	"path/filepath"
	"syscall"
	"testing"
	"time"
)

func privateX11Options(t *testing.T) providerOptions {
	t.Helper()
	options := defaultProviderOptions()
	options.x11LockDirectory = t.TempDir()
	if err := os.Chmod(options.x11LockDirectory, 0700); err != nil {
		t.Fatal(err)
	}
	options.x11SocketDirectory = filepath.Join(options.x11LockDirectory, ".X11-unix")
	return options
}

func TestPrivateX11CreatesAndRemovesOnlyItsOwnNamespace(t *testing.T) {
	options := privateX11Options(t)
	prepared, cleanup, err := preparePrivateX11(options)
	if err != nil || cleanup == nil {
		t.Fatal(err)
	}
	if prepared.x11DirectoryOwnerUID != uint32(os.Geteuid()) || prepared.x11LockDirectoryMode != 0700 || prepared.x11DirectoryMode != 01777 {
		t.Fatal("private namespace contract changed")
	}
	var status syscall.Stat_t
	if err := syscall.Lstat(options.x11SocketDirectory, &status); err != nil || status.Mode&07777 != 01777 {
		t.Fatal("X11 directory mode is invalid")
	}
	if err := cleanup(); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Lstat(options.x11SocketDirectory); !os.IsNotExist(err) {
		t.Fatal("owned X11 directory remains")
	}
	if err := syscall.Lstat(options.x11LockDirectory, &status); err != nil || status.Mode&07777 != 0700 {
		t.Fatal("parent was changed")
	}
}

func TestPrivateX11RetainsPreexistingAndReplacedNamespaces(t *testing.T) {
	options := privateX11Options(t)
	if err := os.Mkdir(options.x11SocketDirectory, 0700); err != nil {
		t.Fatal(err)
	}
	if _, cleanup, err := preparePrivateX11(options); err == nil || cleanup != nil {
		t.Fatal("preexisting X11 namespace was adopted")
	}
	if err := os.Remove(options.x11SocketDirectory); err != nil {
		t.Fatal(err)
	}
	_, cleanup, err := preparePrivateX11(options)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.Rename(options.x11SocketDirectory, options.x11SocketDirectory+"-owned"); err != nil {
		t.Fatal(err)
	}
	if err := os.Mkdir(options.x11SocketDirectory, 0700); err != nil {
		t.Fatal(err)
	}
	if err := cleanup(); err == nil {
		t.Fatal("replacement namespace was removed")
	}
	if _, err := os.Stat(options.x11SocketDirectory); err != nil {
		t.Fatal("replacement was not retained")
	}
}

func TestIdentityReadRejectsFIFOReplacementWithoutBlocking(t *testing.T) {
	path := filepath.Join(t.TempDir(), "record")
	if err := os.WriteFile(path, []byte("record"), 0600); err != nil {
		t.Fatal(err)
	}
	identity, err := lstatIdentity(path)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.Remove(path); err != nil {
		t.Fatal(err)
	}
	if err := syscall.Mkfifo(path, 0600); err != nil {
		t.Fatal(err)
	}
	start := time.Now()
	if _, err := readIdentityBounded(path, identity, 1024); err == nil {
		t.Fatal("replacement FIFO was accepted")
	}
	if time.Since(start) > time.Second {
		t.Fatal("replacement FIFO blocked launcher admission")
	}
}
