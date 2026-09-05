//go:build linux

package seatprovider

import (
	"context"
	"errors"
	"io"
	"os"
	"os/signal"
	"path/filepath"
	"slices"
	"strings"
	"syscall"
	"time"

	"github.com/papi-ux/polaris/multiseat_worker/internal/seatruntime"
)

const (
	defaultRuntimeDirectory = "/run/polaris"
	defaultDBusDaemonPath   = "/usr/bin/dbus-daemon"
	defaultPipeWirePath     = "/usr/bin/pipewire"
	// Fedora exposes pipewire-pulse as a symlink to the same trusted binary.
	// The explicit pulse configuration selects the service role, so pin the
	// regular executable itself and never follow the packaging symlink.
	defaultPipeWirePulsePath = "/usr/bin/pipewire"
	defaultPWCLIPath         = "/usr/bin/pw-cli"
	defaultPactlPath         = "/usr/bin/pactl"
	defaultGSTLaunchPath     = "/usr/bin/gst-launch-1.0"
	defaultGSTInspectPath    = "/usr/bin/gst-inspect-1.0"

	maximumProviderOutput = 64 * 1024
)

type providerOptions struct {
	runtimeDirectory   string
	runtimeOwnerUID    uint32
	executableOwnerUID uint32
	dbusDaemonPath     string
	pipeWirePath       string
	pipeWirePulsePath  string
	pwCLIPath          string
	pactlPath          string
	gstLaunchPath      string
	gstInspectPath     string
	gstPluginPath      string
	softwareDisplay    bool
	startupTimeout     time.Duration
	probeTimeout       time.Duration
	stopTimeout        time.Duration
	probeInterval      time.Duration
}

func defaultProviderOptions() providerOptions {
	return providerOptions{
		runtimeDirectory:   defaultRuntimeDirectory,
		runtimeOwnerUID:    uint32(os.Geteuid()),
		executableOwnerUID: 0,
		dbusDaemonPath:     defaultDBusDaemonPath,
		pipeWirePath:       defaultPipeWirePath,
		pipeWirePulsePath:  defaultPipeWirePulsePath,
		pwCLIPath:          defaultPWCLIPath,
		pactlPath:          defaultPactlPath,
		gstLaunchPath:      defaultGSTLaunchPath,
		gstInspectPath:     defaultGSTInspectPath,
		startupTimeout:     5 * time.Second,
		probeTimeout:       time.Second,
		stopTimeout:        time.Second,
		probeInterval:      20 * time.Millisecond,
	}
}

func validAbsolutePath(path string) bool {
	return filepath.IsAbs(path) && filepath.Clean(path) == path && path != "/" &&
		!strings.ContainsAny(path, "\x00\n\r")
}

func normalizeProviderOptions(options providerOptions) (providerOptions, error) {
	if !validAbsolutePath(options.runtimeDirectory) ||
		!validAbsolutePath(options.dbusDaemonPath) ||
		!validAbsolutePath(options.pipeWirePath) ||
		!validAbsolutePath(options.pipeWirePulsePath) ||
		!validAbsolutePath(options.pwCLIPath) ||
		!validAbsolutePath(options.pactlPath) ||
		!validAbsolutePath(options.gstLaunchPath) ||
		!validAbsolutePath(options.gstInspectPath) ||
		(options.gstPluginPath != "" &&
			!validAbsolutePath(options.gstPluginPath)) ||
		options.startupTimeout <= 0 || options.probeTimeout <= 0 ||
		options.stopTimeout <= 0 || options.probeInterval <= 0 ||
		options.probeInterval > options.startupTimeout {
		return providerOptions{}, errors.New("runtime provider options are invalid")
	}
	return options, nil
}

func parseProviderInvocation(
	expected seatruntime.Stage,
	arguments []string,
	environment []string,
) (seatruntime.Request, error) {
	separator := slices.Index(arguments, "--")
	if len(arguments) < 2 || arguments[0] != "serve-resource-v1" ||
		separator != len(arguments)-1 {
		return seatruntime.Request{}, errors.New("runtime provider invocation is invalid")
	}
	helperArguments := append([]string(nil), arguments[:separator]...)
	helperArguments[0] = "serve"
	request, err := seatruntime.ParseInvocation(helperArguments, environment)
	if err != nil || request.Stage != expected {
		return seatruntime.Request{}, errors.New("runtime provider invocation is invalid")
	}
	return request, nil
}

func openReadinessWriter() (*os.File, error) {
	var status syscall.Stat_t
	if err := syscall.Fstat(3, &status); err != nil ||
		status.Mode&syscall.S_IFMT != syscall.S_IFIFO {
		return nil, errors.New("runtime provider readiness descriptor is invalid")
	}
	flags, _, errno := syscall.Syscall(syscall.SYS_FCNTL, 3, syscall.F_GETFL, 0)
	if errno != 0 || int(flags)&syscall.O_ACCMODE != syscall.O_WRONLY {
		return nil, errors.New("runtime provider readiness descriptor is invalid")
	}
	descriptorFlags, _, errno := syscall.Syscall(
		syscall.SYS_FCNTL,
		3,
		syscall.F_GETFD,
		0,
	)
	if errno != 0 || int(descriptorFlags)&syscall.FD_CLOEXEC != 0 {
		return nil, errors.New("runtime provider readiness descriptor is invalid")
	}
	_, _, errno = syscall.Syscall(
		syscall.SYS_FCNTL,
		3,
		syscall.F_SETFD,
		descriptorFlags|syscall.FD_CLOEXEC,
	)
	if errno != 0 {
		return nil, errors.New("runtime provider readiness descriptor could not be sealed")
	}
	writer := os.NewFile(3, "polaris-runtime-ready")
	if writer == nil {
		return nil, errors.New("runtime provider readiness descriptor is invalid")
	}
	return writer, nil
}

func publishReadiness(writer io.WriteCloser) error {
	if writer == nil {
		return errors.New("runtime provider readiness writer is missing")
	}
	written, writeError := io.WriteString(writer, seatruntime.ReadyRecord)
	closeError := writer.Close()
	if writeError != nil || written != len(seatruntime.ReadyRecord) || closeError != nil {
		return errors.New("runtime provider readiness could not be published")
	}
	return nil
}

func signalContext() (context.Context, context.CancelFunc) {
	return signal.NotifyContext(context.Background(), syscall.SIGTERM, syscall.SIGINT)
}

type runtimeDirectory struct {
	path string
	file *os.File
	dev  uint64
	ino  uint64
	uid  uint32
}

func openRuntimeDirectory(path string, expectedUID uint32) (*runtimeDirectory, error) {
	if !validAbsolutePath(path) {
		return nil, errors.New("runtime provider directory is invalid")
	}
	descriptor, err := syscall.Open(
		path,
		syscall.O_RDONLY|syscall.O_DIRECTORY|syscall.O_NOFOLLOW|syscall.O_CLOEXEC,
		0,
	)
	if err != nil {
		return nil, errors.New("runtime provider directory is unavailable")
	}
	file := os.NewFile(uintptr(descriptor), "polaris-runtime-directory")
	if file == nil {
		_ = syscall.Close(descriptor)
		return nil, errors.New("runtime provider directory is unavailable")
	}
	var status syscall.Stat_t
	if err := syscall.Fstat(descriptor, &status); err != nil ||
		status.Mode&syscall.S_IFMT != syscall.S_IFDIR ||
		status.Uid != expectedUID || status.Mode&0o7777 != 0o700 {
		_ = file.Close()
		return nil, errors.New("runtime provider directory ownership or mode is invalid")
	}
	runtime := &runtimeDirectory{
		path: path,
		file: file,
		dev:  uint64(status.Dev),
		ino:  status.Ino,
		uid:  expectedUID,
	}
	if err := runtime.verify(); err != nil {
		_ = file.Close()
		return nil, err
	}
	return runtime, nil
}

func (runtime *runtimeDirectory) verify() error {
	if runtime == nil || runtime.file == nil {
		return errors.New("runtime provider directory is invalid")
	}
	var status syscall.Stat_t
	if err := syscall.Lstat(runtime.path, &status); err != nil ||
		status.Mode&syscall.S_IFMT != syscall.S_IFDIR ||
		status.Uid != runtime.uid || status.Mode&0o7777 != 0o700 ||
		uint64(status.Dev) != runtime.dev || status.Ino != runtime.ino {
		return errors.New("runtime provider directory identity changed")
	}
	return nil
}

func (runtime *runtimeDirectory) close() {
	if runtime != nil && runtime.file != nil {
		_ = runtime.file.Close()
	}
}

type artifactIdentity struct {
	dev  uint64
	ino  uint64
	mode uint32
	uid  uint32
}

func lstatIdentity(path string) (artifactIdentity, error) {
	var status syscall.Stat_t
	if err := syscall.Lstat(path, &status); err != nil {
		return artifactIdentity{}, err
	}
	return artifactIdentity{
		dev:  uint64(status.Dev),
		ino:  status.Ino,
		mode: status.Mode,
		uid:  status.Uid,
	}, nil
}

func sameIdentity(left artifactIdentity, right artifactIdentity) bool {
	return left.dev == right.dev && left.ino == right.ino &&
		left.mode == right.mode && left.uid == right.uid
}
