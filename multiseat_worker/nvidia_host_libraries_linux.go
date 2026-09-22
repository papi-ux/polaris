//go:build linux

package main

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"time"
)

// The reviewed contract the host and the image were built against. The host
// mounts the files; this side only proves they arrived and that the loader can
// find them.
const (
	nvidiaHostContractPath = "/usr/share/polaris/build/nvidia-host-contract.json"
	nvidiaLoaderCacheDir   = "/etc/polaris-ld"
	nvidiaLoaderCachePath  = nvidiaLoaderCacheDir + "/ld.so.cache"
	nvidiaGraphicsCheck    = "/usr/libexec/polaris-seat/graphics-check"
)

type nvidiaHostContract struct {
	Contract      uint `json:"contract"`
	Architectures []struct {
		Name      string   `json:"name"`
		Directory string   `json:"directory"`
		Required  []string `json:"required"`
	} `json:"architectures"`
}

// prepareHostGraphics makes a borrowed NVIDIA driver usable inside the seat.
//
// The rootfs is read only and carries no driver of its own, so the loader cache
// has to be rebuilt over the mounted files. Steam's pressure-vessel copies the
// graphics stack into a game's namespace by matching sonames against that
// cache, and the libraries it only ever dlopens would be missing without it.
func prepareHostGraphics(parent context.Context, diagnostics *os.File) error {
	return prepareHostGraphicsFrom(parent, diagnostics, nvidiaHostContractPath)
}

// decodeContractFile reads and checks one reviewed contract.
func decodeContractFile(path string, contract *nvidiaHostContract) error {
	payload, err := os.ReadFile(path)
	if err != nil {
		return err
	}
	if err := json.Unmarshal(payload, contract); err != nil {
		return fmt.Errorf("nvidia host contract: %w", err)
	}
	if contract.Contract != 1 || len(contract.Architectures) == 0 {
		return errors.New("nvidia host contract is not the reviewed one")
	}
	return nil
}

func prepareHostGraphicsFrom(parent context.Context, diagnostics *os.File, contractPath string) error {
	var contract nvidiaHostContract
	if err := decodeContractFile(contractPath, &contract); errors.Is(err, os.ErrNotExist) {
		return nil // an image that carries its own driver
	} else if err != nil {
		return err
	}

	for _, architecture := range contract.Architectures {
		for _, soname := range architecture.Required {
			path := filepath.Join(architecture.Directory, soname)
			info, err := os.Lstat(path)
			if err != nil || !info.Mode().IsRegular() {
				return fmt.Errorf("nvidia %s library %s did not arrive", architecture.Name, soname)
			}
		}
	}

	if err := os.MkdirAll(nvidiaLoaderCacheDir, 0o700); err != nil {
		return fmt.Errorf("nvidia loader cache: %w", err)
	}
	context, cancel := context.WithTimeout(parent, 30*time.Second)
	defer cancel()
	// -X because every borrowed file is already published under its own SONAME,
	// so the cache needs no links written beside it.
	cache := exec.CommandContext(context, "/sbin/ldconfig", "-X", "-C", nvidiaLoaderCachePath)
	cache.Stdout = diagnostics
	cache.Stderr = diagnostics
	if err := cache.Run(); err != nil {
		return fmt.Errorf("nvidia loader cache: %w", err)
	}

	// Loading each required library the way a game will is the only check that
	// distinguishes "the files are present" from "the driver works".
	for _, probe := range []string{nvidiaGraphicsCheck, nvidiaGraphicsCheck + "-32"} {
		if _, err := os.Stat(probe); errors.Is(err, os.ErrNotExist) {
			continue
		}
		command := exec.CommandContext(context, probe)
		command.Stdout = diagnostics
		command.Stderr = diagnostics
		if err := command.Run(); err != nil {
			return fmt.Errorf("nvidia graphics check %s: %w", filepath.Base(probe), err)
		}
	}
	return nil
}
