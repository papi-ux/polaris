//go:build linux

package main

import (
	"os"

	"github.com/papi-ux/polaris/multiseat_worker/internal/seatruntime"
)

func main() {
	if err := seatruntime.Run(
		os.Args[1:],
		os.Environ(),
		seatruntime.DefaultCatalogPath,
		uint32(os.Geteuid()),
		0,
	); err != nil {
		os.Exit(1)
	}
}
