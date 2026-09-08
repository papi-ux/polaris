//go:build linux

package main

import (
	"fmt"
	"github.com/papi-ux/polaris/multiseat_worker/internal/seatprovider"
	"os"
)

func main() {
	if err := seatprovider.RunLauncher(os.Args[1:], os.Environ()); err != nil {
		fmt.Fprintln(os.Stderr, "runtime provider failed:", err)
		os.Exit(1)
	}
}
