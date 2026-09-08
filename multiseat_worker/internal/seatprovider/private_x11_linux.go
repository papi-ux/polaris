//go:build linux

package seatprovider

import (
	"errors"
	"os"
	"path/filepath"
	"syscall"
)

// Only the worker's pre-admitted private /tmp (0700, owned by its UID) may be
// initialized here. Shared system /tmp retains the existing probe-only path.
func preparePrivateX11(options providerOptions) (providerOptions, func() error, error) {
	return preparePrivateX11WithFilesystem(options, syscall.Openat, syscall.Fstat)
}

func preparePrivateX11WithFilesystem(options providerOptions,
	openat func(int, string, int, uint32) (int, error),
	fstat func(int, *syscall.Stat_t) error,
) (providerOptions, func() error, error) {
	var parentStatus syscall.Stat_t
	if err := syscall.Lstat(options.x11LockDirectory, &parentStatus); err != nil {
		return options, nil, err
	}
	if parentStatus.Mode&syscall.S_IFMT != syscall.S_IFDIR {
		return options, nil, errors.New("X11 parent is not a directory")
	}
	if parentStatus.Mode&0o7777 != 0o700 {
		return options, nil, nil
	}
	if parentStatus.Uid != options.runtimeOwnerUID || filepath.Dir(options.x11SocketDirectory) != options.x11LockDirectory {
		return options, nil, errors.New("private X11 parent ownership is invalid")
	}
	parent, err := openFixedDirectory(options.x11LockDirectory, options.runtimeOwnerUID, 0o700)
	if err != nil {
		return options, nil, err
	}
	if err := syscall.Mkdirat(int(parent.file.Fd()), filepath.Base(options.x11SocketDirectory), 0o700); err != nil {
		parent.close()
		return options, nil, errors.New("private X11 namespace already exists or cannot be created")
	}
	fd, err := openat(int(parent.file.Fd()), filepath.Base(options.x11SocketDirectory), syscall.O_RDONLY|syscall.O_DIRECTORY|syscall.O_NOFOLLOW|syscall.O_CLOEXEC, 0)
	if err != nil {
		parent.close()
		return options, nil, errors.New("private X11 cleanup unproven; destroy worker private tmpfs")
	}
	file := os.NewFile(uintptr(fd), "private-X11-namespace")
	var status syscall.Stat_t
	if err := fstat(fd, &status); err != nil {
		file.Close()
		parent.close()
		return options, nil, errors.New("private X11 cleanup unproven; destroy worker private tmpfs")
	}
	cleanup := func() error {
		defer file.Close()
		defer parent.close()
		if err := parent.verify(); err != nil {
			return err
		}
		var current syscall.Stat_t
		if err := syscall.Lstat(options.x11SocketDirectory, &current); errors.Is(err, os.ErrNotExist) {
			return nil
		} else if err != nil || current.Dev != status.Dev || current.Ino != status.Ino || current.Mode&syscall.S_IFMT != syscall.S_IFDIR || current.Uid != options.runtimeOwnerUID {
			return errors.New("private X11 namespace replaced")
		}
		return os.Remove(options.x11SocketDirectory) // empty directory only
	}
	if status.Uid != options.runtimeOwnerUID {
		return options, cleanup, errors.New("created X11 namespace has unexpected owner")
	}
	if err := syscall.Fchmod(fd, 0o1777); err != nil {
		return options, cleanup, err
	}
	if err := parent.verify(); err != nil {
		return options, cleanup, err
	}
	options.x11DirectoryOwnerUID = options.runtimeOwnerUID
	options.x11DirectoryMode = 0o1777
	options.x11LockDirectoryMode = 0o700
	return options, cleanup, nil
}
