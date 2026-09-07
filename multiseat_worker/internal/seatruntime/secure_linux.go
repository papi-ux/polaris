//go:build linux

package seatruntime

import (
	"errors"
	"fmt"
	"os"
	"runtime"
	"slices"
	"syscall"
)

func validateReadyDescriptor() error {
	var status syscall.Stat_t
	if err := syscall.Fstat(3, &status); err != nil ||
		status.Mode&syscall.S_IFMT != syscall.S_IFIFO {
		return errors.New("runtime readiness descriptor is invalid")
	}
	flags, _, errno := syscall.Syscall(
		syscall.SYS_FCNTL,
		uintptr(3),
		uintptr(syscall.F_GETFL),
		0,
	)
	if errno != 0 || int(flags)&syscall.O_ACCMODE != syscall.O_WRONLY {
		return errors.New("runtime readiness descriptor is invalid")
	}
	descriptorFlags, _, errno := syscall.Syscall(
		syscall.SYS_FCNTL,
		uintptr(3),
		uintptr(syscall.F_GETFD),
		0,
	)
	if errno != 0 || int(descriptorFlags)&syscall.FD_CLOEXEC != 0 {
		return errors.New("runtime readiness descriptor is invalid")
	}
	return nil
}

func secureOpenRegular(
	path string,
	expectedOwnerUID uint32,
	requireExecutable bool,
) (*os.File, error) {
	if !validProviderExecutable(path) {
		return nil, errors.New("trusted runtime file path is invalid")
	}
	descriptor, err := syscall.Open(
		path,
		syscall.O_RDONLY|syscall.O_NOFOLLOW|syscall.O_NONBLOCK,
		0,
	)
	if err != nil {
		return nil, errors.New("trusted runtime file could not be opened")
	}
	closeDescriptor := true
	defer func() {
		if closeDescriptor {
			_ = syscall.Close(descriptor)
		}
	}()
	var status syscall.Stat_t
	if err := syscall.Fstat(descriptor, &status); err != nil ||
		status.Mode&syscall.S_IFMT != syscall.S_IFREG ||
		status.Uid != expectedOwnerUID ||
		status.Mode&0o022 != 0 ||
		(!requireExecutable && status.Mode&0o222 != 0) ||
		status.Mode&(syscall.S_ISUID|syscall.S_ISGID) != 0 ||
		(requireExecutable && status.Mode&0o111 == 0) {
		return nil, errors.New("trusted runtime file ownership or mode is invalid")
	}
	if descriptor < 4 {
		duplicated, _, errno := syscall.Syscall(
			syscall.SYS_FCNTL,
			uintptr(descriptor),
			uintptr(syscall.F_DUPFD),
			uintptr(4),
		)
		if errno != 0 {
			return nil, errors.New("trusted runtime file descriptor could not be reserved")
		}
		_ = syscall.Close(descriptor)
		descriptor = int(duplicated)
	}
	file := os.NewFile(uintptr(descriptor), "trusted-runtime-file")
	if file == nil {
		return nil, errors.New("trusted runtime file descriptor is invalid")
	}
	closeDescriptor = false
	return file, nil
}

// LoadCatalog opens the fixed provider catalog without following a final
// symlink and verifies its owner and non-writable trust boundary before JSON
// parsing. The production command requires its own effective UID because the
// controller supplies this exact file through the read-only auth mount.
func LoadCatalog(path string, expectedOwnerUID uint32) (Catalog, error) {
	file, err := secureOpenRegular(path, expectedOwnerUID, false)
	if err != nil {
		return Catalog{}, errors.New("trusted runtime provider catalog is unavailable")
	}
	defer file.Close()
	catalog, err := DecodeCatalog(file)
	if err != nil {
		return Catalog{}, err
	}
	return catalog, nil
}

func validatePlan(plan Plan) error {
	if !validProviderExecutable(plan.Executable) || len(plan.Arguments) < 5 ||
		plan.Arguments[0] != plan.Executable ||
		plan.Arguments[1] != "serve-resource-v1" {
		return errors.New("runtime provider plan is invalid")
	}
	separator := slices.Index(plan.Arguments, "--")
	if separator < 4 {
		return errors.New("runtime provider plan is invalid")
	}
	helperArguments := append([]string(nil), plan.Arguments[1:separator]...)
	helperArguments[0] = "serve"
	if _, err := ParseInvocation(helperArguments, plan.Environment); err != nil {
		return errors.New("runtime provider plan is invalid")
	}
	if len(plan.Arguments)-separator-1 > maximumProviderArguments {
		return errors.New("runtime provider plan is invalid")
	}
	for _, argument := range plan.Arguments[separator+1:] {
		if !validProviderValue(argument, true) {
			return errors.New("runtime provider plan is invalid")
		}
	}
	return nil
}

// ExecuteProvider replaces the dispatcher in place. The worker therefore
// continues to own the exact PID and process group that writes readiness and
// any descendants the provider creates.
func ExecuteProvider(plan Plan, expectedOwnerUID uint32) error {
	if err := validatePlan(plan); err != nil {
		return err
	}
	executable, err := secureOpenRegular(plan.Executable, expectedOwnerUID, true)
	if err != nil {
		return errors.New("trusted runtime provider executable is unavailable")
	}
	defer executable.Close()
	path := fmt.Sprintf("/proc/self/fd/%d", executable.Fd())
	err = syscall.Exec(path, plan.Arguments, plan.Environment)
	runtime.KeepAlive(executable)
	if err != nil {
		return errors.New("trusted runtime provider could not be executed")
	}
	return nil
}

// Run performs the complete fail-closed helper transition. It parses the
// least-authority invocation before touching the trusted catalog, resolves one
// exact provider, and then execs it without a shell or ambient environment.
func Run(
	arguments []string,
	environment []string,
	catalogPath string,
	catalogOwnerUID uint32,
	providerOwnerUID uint32,
) error {
	request, err := ParseInvocation(arguments, environment)
	if err != nil {
		return err
	}
	if err := validateReadyDescriptor(); err != nil {
		return err
	}
	catalog, err := LoadCatalog(catalogPath, catalogOwnerUID)
	if err != nil {
		return err
	}
	plan, err := catalog.Resolve(request)
	if err != nil {
		return err
	}
	return ExecuteProvider(plan, providerOwnerUID)
}
