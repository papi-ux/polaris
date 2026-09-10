//go:build linux

package main

import (
	"fmt"
	"os"

	"github.com/papi-ux/polaris/multiseat_worker/internal/seatprovider"
)

func main() {
	if err := seatprovider.RunSessionBus(os.Args[1:], os.Environ()); err != nil {
		fmt.Fprintln(os.Stderr, "runtime provider failed:", err)
		os.Exit(1)
	}
}
