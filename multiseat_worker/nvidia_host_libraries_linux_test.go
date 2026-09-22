//go:build linux

package main

import (
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestPrepareHostGraphicsIgnoresAnImageWithItsOwnDriver(t *testing.T) {
	if _, err := os.Stat(nvidiaHostContractPath); err == nil {
		t.Skip("this machine has a contract at the production path")
	}
	if err := prepareHostGraphics(context.Background(), os.Stderr); err != nil {
		t.Fatalf("an image without a contract must start: %v", err)
	}
}

func TestNvidiaHostContractRejectsAnUnreviewedShape(t *testing.T) {
	for name, payload := range map[string]string{
		"another contract": `{"contract":2,"architectures":[{"name":"amd64","directory":"/usr/lib","required":["libcuda.so.1"]}]}`,
		"no architectures": `{"contract":1,"architectures":[]}`,
		"not json":         `{`,
	} {
		t.Run(name, func(t *testing.T) {
			directory := t.TempDir()
			path := filepath.Join(directory, "nvidia-host-contract.json")
			if err := os.WriteFile(path, []byte(payload), 0o644); err != nil {
				t.Fatal(err)
			}
			var contract nvidiaHostContract
			if err := decodeContractFile(path, &contract); err == nil && contract.Contract == 1 && len(contract.Architectures) > 0 {
				t.Fatalf("%s was accepted", name)
			}
		})
	}
}

func TestPrepareHostGraphicsNamesTheMissingSonameAndAbi(t *testing.T) {
	directory := t.TempDir()
	contract := `{"contract":1,"architectures":[{"name":"i386","directory":"` + directory +
		`","required":["libcuda.so.1"]}]}`
	path := filepath.Join(directory, "nvidia-host-contract.json")
	if err := os.WriteFile(path, []byte(contract), 0o644); err != nil {
		t.Fatal(err)
	}

	err := prepareHostGraphicsFrom(context.Background(), os.Stderr, path)

	if err == nil {
		t.Fatal("a missing library must stop the worker before gamescope starts")
	}
	if !strings.Contains(err.Error(), "i386") || !strings.Contains(err.Error(), "libcuda.so.1") {
		t.Fatalf("the error must name the ABI and the library: %v", err)
	}
}
