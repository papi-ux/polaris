//go:build linux

package seatinput

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"syscall"
	"testing"
)

func TestOnlyAbsentPhysicalFieldPreservesHostCompatibility(t *testing.T) {
	for _, test := range []struct {
		value   string
		err     error
		allowed bool
	}{
		{"", nil, true}, {"expected", nil, true}, {"", syscall.ENOENT, true},
		{"other-seat", nil, false}, {"other-seat", syscall.ENOENT, false},
		{"", syscall.EACCES, false}, {"", syscall.EPERM, false}, {"", syscall.ENODEV, false},
	} {
		if validPhysicalIdentity(test.value, test.err, "expected") != test.allowed {
			t.Fatalf("unexpected physical identity admission: %+v", test)
		}
	}
}

func TestConsumerRejectsPathAndWriteOnlyDescriptors(t *testing.T) {
	for _, flags := range []int{syscall.O_RDONLY, syscall.O_WRONLY, syscall.O_RDWR, 0x200000} {
		fd, err := syscall.Open("/dev/null", flags|syscall.O_CLOEXEC, 0)
		if err != nil {
			t.Fatal(err)
		}
		readable := readableConsumerFD(fmt.Sprintf("/proc/self/fdinfo/%d", fd))
		_ = syscall.Close(fd)
		if readable != (flags == syscall.O_RDONLY) {
			t.Fatalf("incorrect read admission for flags %#x", flags)
		}
	}
}

func TestConsumerLifetimeRemainsRetiredAfterReap(t *testing.T) {
	pidFD := -1
	command := exec.Command("/bin/sleep", "30")
	command.SysProcAttr = &syscall.SysProcAttr{PidFD: &pidFD}
	if err := command.Start(); err != nil {
		t.Fatal(err)
	}
	defer func() { _ = command.Process.Kill(); _ = command.Wait() }()
	if pidFD < 0 {
		t.Fatal("pidfd is required for input consumer proof")
	}
	defer syscall.Close(pidFD)
	if !consumerAlive(pidFD) {
		t.Fatal("running consumer was not alive")
	}
	if err := command.Process.Kill(); err != nil {
		t.Fatal(err)
	}
	_ = command.Wait()
	for i := 0; i < 100; i++ {
		if consumerAlive(pidFD) {
			t.Fatal("reaped consumer became live")
		}
	}
	if consumerAlive(-1) {
		t.Fatal("accepted missing process lifetime")
	}
}

func TestRolesRejectAmbientDevicesAndNoncanonicalSlots(t *testing.T) {
	for _, name := range []string{"event0", "js0", "mouse0", "polaris-gamepad-00", "polaris-gamepad-16", "polaris-gamepad-+1", "../polaris-keyboard", "polaris-gamepad-0/child"} {
		if _, ok := deviceRole(name); ok {
			t.Fatalf("accepted %q", name)
		}
	}
	for _, name := range []string{"polaris-keyboard", "polaris-mouse-relative", "polaris-mouse-absolute", "polaris-touch", "polaris-pen", "polaris-gamepad-0", "polaris-gamepad-15"} {
		if _, ok := deviceRole(name); !ok {
			t.Fatalf("rejected %q", name)
		}
	}
}

func TestIdentityMatchesHostContractAndSeparatesGenerations(t *testing.T) {
	for _, name := range []string{"polaris-keyboard", "polaris-mouse-relative", "polaris-mouse-absolute", "polaris-touch", "polaris-pen", "polaris-gamepad-0", "polaris-gamepad-15"} {
		r, _ := deviceRole(name)
		kernel, phys := expectedIdentity("input-seat-test", r)
		otherKernel, otherPhys := expectedIdentity("input-seat-test-next", r)
		if kernel == otherKernel || phys == otherPhys || len(kernel) > 79 {
			t.Fatal("input generations alias")
		}
		if name == "polaris-gamepad-15" && phys != "polaris/client-gamepad-seat-isolated/input-seat-test/gamepad/15" {
			t.Fatal(phys)
		}
	}
	r, _ := deviceRole("polaris-mouse-absolute")
	name, phys := expectedIdentity("input-seat-test", r)
	if name != "Polaris multiseat 5753eb36944ebc0919a26d97732be17e mouse (absolute)" || phys != "polaris/client-input-seat-isolated/input-seat-test/mouse" {
		t.Fatalf("host contract drift: %q %q", name, phys)
	}
}

func TestOpenRejectsUnsafeNamespaceAndNonDevicesWithoutLeaking(t *testing.T) {
	directory := t.TempDir()
	for _, name := range []string{"polaris-keyboard", "polaris-mouse-relative", "polaris-mouse-absolute"} {
		if err := os.WriteFile(filepath.Join(directory, name), nil, 0600); err != nil {
			t.Fatal(err)
		}
	}
	before, err := boundedNames("/proc/self/fd", 4096)
	if err != nil {
		t.Fatal(err)
	}
	for i := 0; i < 100; i++ {
		if set, err := Open(directory, "input-seat-test"); err == nil || set != nil {
			t.Fatal("accepted regular input file")
		}
	}
	after, err := boundedNames("/proc/self/fd", 4096)
	if err != nil || len(before) != len(after) {
		t.Fatal("failed admission leaked descriptors")
	}
	link := filepath.Join(t.TempDir(), "input")
	if err := os.Symlink(directory, link); err != nil {
		t.Fatal(err)
	}
	if _, err := Open(link, "input-seat-test"); err == nil {
		t.Fatal("accepted symlink namespace")
	}
	for _, seat := range []string{"", "-bad", "../seat", "seat\n", "seat/other"} {
		if _, err := Open(directory, seat); err == nil {
			t.Fatal("accepted invalid seat")
		}
	}
	if err := os.Chmod(directory, 0777); err != nil {
		t.Fatal(err)
	}
	if _, err := Open(directory, "input-seat-test"); err == nil {
		t.Fatal("accepted writable namespace")
	}
}

func TestCompositorDescriptorRolesAreOrderedAndUnsupportedRolesRejected(t *testing.T) {
	s := &Set{path: Directory}
	for _, name := range []string{"polaris-mouse-absolute", "polaris-gamepad-0", "polaris-keyboard", "polaris-mouse-relative"} {
		r, _ := deviceRole(name)
		s.devices = append(s.devices, Device{role: r})
	}
	devices, err := s.compositorDevices()
	if err != nil || len(devices) != 3 || devices[0].role.name != "polaris-keyboard" || devices[1].role.name != "polaris-mouse-relative" || devices[2].role.name != "polaris-mouse-absolute" {
		t.Fatal(devices, err)
	}
	for _, name := range []string{"polaris-touch", "polaris-pen"} {
		r, _ := deviceRole(name)
		s.devices = append(s.devices, Device{role: r})
		if _, err := s.compositorDevices(); err == nil {
			t.Fatal("unsupported role accepted")
		}
		s.devices = s.devices[:len(s.devices)-1]
	}
}
