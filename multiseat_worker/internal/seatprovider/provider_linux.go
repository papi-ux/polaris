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
	"sync"
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
	defaultPipeWirePulsePath  = "/usr/bin/pipewire"
	defaultPWCLIPath          = "/usr/bin/pw-cli"
	defaultPWDumpPath         = "/usr/bin/pw-dump"
	defaultWirePlumberPath    = "/usr/bin/wireplumber"
	defaultPactlPath          = "/usr/bin/pactl"
	defaultGSTLaunchPath      = "/usr/bin/gst-launch-1.0"
	defaultGSTInspectPath     = "/usr/bin/gst-inspect-1.0"
	defaultGamescopePath      = "/usr/bin/gamescope"
	defaultXWaylandPath       = "/usr/bin/Xwayland"
	defaultX11SocketDirectory = "/tmp/.X11-unix"
	defaultX11LockDirectory   = "/tmp"

	maximumProviderOutput = 64 * 1024
)

type providerOptions struct {
	runtimeDirectory      string
	runtimeOwnerUID       uint32
	executableOwnerUID    uint32
	dbusDaemonPath        string
	pipeWirePath          string
	pipeWirePulsePath     string
	pwCLIPath             string
	pwDumpPath            string
	wirePlumberPath       string
	pactlPath             string
	gstLaunchPath         string
	gstInspectPath        string
	gstPluginPath         string
	softwareDisplay       bool
	gamescopePath         string
	xWaylandPath          string
	x11SocketDirectory    string
	x11LockDirectory      string
	x11DirectoryOwnerUID  uint32
	x11DirectoryMode      uint32
	x11LockDirectoryMode  uint32
	softwareGamescope     bool
	softwareVulkanICDPath string
	allowSharedX11        bool
	startupTimeout        time.Duration
	probeTimeout          time.Duration
	stopTimeout           time.Duration
	probeInterval         time.Duration
}

func defaultProviderOptions() providerOptions {
	return providerOptions{
		runtimeDirectory:     defaultRuntimeDirectory,
		runtimeOwnerUID:      uint32(os.Geteuid()),
		executableOwnerUID:   0,
		dbusDaemonPath:       defaultDBusDaemonPath,
		pipeWirePath:         defaultPipeWirePath,
		pipeWirePulsePath:    defaultPipeWirePulsePath,
		pwCLIPath:            defaultPWCLIPath,
		pwDumpPath:           defaultPWDumpPath,
		wirePlumberPath:      defaultWirePlumberPath,
		pactlPath:            defaultPactlPath,
		gstLaunchPath:        defaultGSTLaunchPath,
		gstInspectPath:       defaultGSTInspectPath,
		gamescopePath:        defaultGamescopePath,
		xWaylandPath:         defaultXWaylandPath,
		x11SocketDirectory:   defaultX11SocketDirectory,
		x11LockDirectory:     defaultX11LockDirectory,
		x11DirectoryOwnerUID: 0,
		x11DirectoryMode:     0o1777,
		startupTimeout:       5 * time.Second,
		probeTimeout:         time.Second,
		stopTimeout:          time.Second,
		probeInterval:        20 * time.Millisecond,
	}
}

func validAbsolutePath(path string) bool {
	return filepath.IsAbs(path) && filepath.Clean(path) == path && path != "/" &&
		!strings.ContainsAny(path, "\x00\n\r")
}

func normalizeProviderOptions(options providerOptions) (providerOptions, error) {
	if options.x11LockDirectoryMode == 0 {
		options.x11LockDirectoryMode = options.x11DirectoryMode
	}
	if !validAbsolutePath(options.runtimeDirectory) ||
		!validAbsolutePath(options.dbusDaemonPath) ||
		!validAbsolutePath(options.pipeWirePath) ||
		!validAbsolutePath(options.pipeWirePulsePath) ||
		!validAbsolutePath(options.pwCLIPath) ||
		!validAbsolutePath(options.pwDumpPath) ||
		!validAbsolutePath(options.wirePlumberPath) ||
		!validAbsolutePath(options.pactlPath) ||
		!validAbsolutePath(options.gstLaunchPath) ||
		!validAbsolutePath(options.gstInspectPath) ||
		!validAbsolutePath(options.gamescopePath) ||
		!validAbsolutePath(options.xWaylandPath) ||
		!validAbsolutePath(options.x11SocketDirectory) ||
		!validAbsolutePath(options.x11LockDirectory) ||
		(options.gstPluginPath != "" &&
			!validAbsolutePath(options.gstPluginPath)) ||
		(options.softwareVulkanICDPath != "" &&
			!validAbsolutePath(options.softwareVulkanICDPath)) ||
		options.x11DirectoryMode == 0 || options.x11DirectoryMode > 0o7777 ||
		options.x11LockDirectoryMode > 0o7777 ||
		(options.softwareGamescope && options.softwareVulkanICDPath == "") ||
		(!options.softwareGamescope && options.softwareVulkanICDPath != "") ||
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
	pins artifactPins
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
		runtime.pins.close()
		_ = runtime.file.Close()
	}
}

// Keep captured inodes allocated through child shutdown and cleanup. Comparing
// dev/ino alone cannot identify an unlinked inode after the kernel reuses it.
// O_PATH pins grant no read/write access and never reach a provider child.
type artifactPins struct {
	mutex  sync.Mutex
	files  map[[2]uint64]*os.File
	closed bool
}

const maximumArtifactPins = 256
const linuxOPath = 0x200000

func (pins *artifactPins) capture(path string) (artifactIdentity, error) {
	pins.mutex.Lock()
	defer pins.mutex.Unlock()
	if pins.closed {
		return artifactIdentity{}, errors.New("runtime artifact ownership is closed")
	}
	expected, err := lstatIdentity(path)
	if err != nil {
		return artifactIdentity{}, err
	}
	if expected.mode&syscall.S_IFMT == syscall.S_IFLNK {
		return artifactIdentity{}, errors.New("runtime artifact cannot be a symlink")
	}
	descriptor, err := syscall.Open(path, linuxOPath|syscall.O_NOFOLLOW|syscall.O_CLOEXEC, 0)
	if err != nil {
		return artifactIdentity{}, err
	}
	var status syscall.Stat_t
	if err := syscall.Fstat(descriptor, &status); err != nil || !sameIdentity(expected, artifactIdentity{
		dev: uint64(status.Dev), ino: status.Ino, mode: status.Mode, uid: status.Uid,
	}) {
		_ = syscall.Close(descriptor)
		return artifactIdentity{}, errors.New("runtime artifact changed while acquiring ownership")
	}
	key := [2]uint64{expected.dev, expected.ino}
	if _, present := pins.files[key]; present {
		_ = syscall.Close(descriptor)
		return expected, nil
	}
	if len(pins.files) >= maximumArtifactPins {
		_ = syscall.Close(descriptor)
		return artifactIdentity{}, errors.New("runtime artifact ownership limit reached")
	}
	file := os.NewFile(uintptr(descriptor), "polaris-artifact-pin")
	if file == nil {
		_ = syscall.Close(descriptor)
		return artifactIdentity{}, errors.New("runtime artifact ownership is unavailable")
	}
	if pins.files == nil {
		pins.files = make(map[[2]uint64]*os.File)
	}
	pins.files[key] = file
	return expected, nil
}

func (pins *artifactPins) close() {
	pins.mutex.Lock()
	defer pins.mutex.Unlock()
	for _, file := range pins.files {
		_ = file.Close()
	}
	pins.files = nil
	pins.closed = true
}

type artifactIdentity struct {
	dev      uint64
	ino      uint64
	mode     uint32
	uid      uint32
	rejected bool // A detected creation failure must never become partial-cleanup authority.
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
	return !left.rejected && !right.rejected && left.dev == right.dev && left.ino == right.ino &&
		left.mode == right.mode && left.uid == right.uid
}
