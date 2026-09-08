//go:build linux

package seatinput

import (
	"os"
	"path/filepath"
	"testing"
)

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

func TestInputPropertiesAreLiteralAndExcludeGamepads(t *testing.T) {
	s := &Set{path: Directory}
	for _, name := range []string{"polaris-keyboard", "polaris-mouse-relative", "polaris-mouse-absolute", "polaris-gamepad-0"} {
		r, _ := deviceRole(name)
		s.devices = append(s.devices, Device{role: r})
	}
	args := s.CompositorArguments()
	if len(args) != 3 || args[0] != "keyboard=/dev/input/polaris-keyboard" || args[1] != "mouse=/dev/input/polaris-mouse-relative" || args[2] != "mouse=/dev/input/polaris-mouse-absolute" {
		t.Fatal(args)
	}
}
