//go:build linux

package seatprovider

import (
	"os"
	"os/exec"
	"syscall"
	"testing"
)

func TestProcessCookiePinsOriginalLifetimeAndRejectsRetirement(t *testing.T) {
	pidFD := -1
	child := exec.Command("/bin/sleep", "30")
	child.SysProcAttr = &syscall.SysProcAttr{PidFD: &pidFD}
	if err := child.Start(); err != nil {
		t.Fatal(err)
	}
	defer func() { _ = child.Process.Kill(); _ = child.Wait(); _ = syscall.Close(pidFD) }()
	life, err := retainProcessLifetime(child.Process.Pid, pidFD, processCookie{})
	if err != nil {
		t.Fatal(err)
	}
	defer life.close()
	launcher, err := retainProcessLifetime(child.Process.Pid, -1, life.cookie)
	if err != nil {
		t.Fatal(err)
	}
	defer launcher.close()
	wrong := life.cookie
	wrong.inode++
	if other, err := retainProcessLifetime(child.Process.Pid, -1, wrong); err == nil {
		other.close()
		t.Fatal("stale cookie accepted")
	}
	if other, err := retainProcessLifetime(os.Getpid(), -1, life.cookie); err == nil {
		other.close()
		t.Fatal("different live process accepted")
	}
	if err := child.Process.Kill(); err != nil {
		t.Fatal(err)
	}
	_ = child.Wait()
	if life.verify() == nil || launcher.verify() == nil {
		t.Fatal("dead compositor remained usable")
	}
	if other, err := retainProcessLifetime(child.Process.Pid, pidFD, life.cookie); err == nil {
		other.close()
		t.Fatal("dead process was reacquired")
	}
}
