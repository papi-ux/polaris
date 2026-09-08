//go:build linux

package main

import (
	"github.com/papi-ux/polaris/multiseat_worker/internal/seatprovider"
	"os"
)

func main() {
	if err := seatprovider.RunLauncher(os.Args[1:], os.Environ()); err != nil {
		os.Exit(1)
	}
}
