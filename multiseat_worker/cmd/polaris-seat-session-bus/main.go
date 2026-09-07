//go:build linux

package main

import (
	"os"

	"github.com/papi-ux/polaris/multiseat_worker/internal/seatprovider"
)

func main() {
	if err := seatprovider.RunSessionBus(os.Args[1:], os.Environ()); err != nil {
		os.Exit(1)
	}
}
