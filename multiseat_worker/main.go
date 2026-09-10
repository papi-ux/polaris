//go:build linux

package main

import (
	"context"
	"errors"
	"fmt"
	"os"
	"os/signal"
	"syscall"
)

func run(arguments []string) error {
	if len(arguments) == 0 {
		return errors.New("expected run or health")
	}
	uid := uint32(os.Geteuid())
	paths := productionPaths()
	switch arguments[0] {
	case "physical-game-probe":
		return physicalGameProbe(arguments[1:])
	case "health":
		if len(arguments) != 1 {
			return errors.New("health does not accept arguments")
		}
		config, err := loadWorkerConfig(os.LookupEnv, workloadPlan{})
		if err != nil {
			return err
		}
		return checkHealth(config, paths, uid)
	case "run":
		workload, err := parseRunArguments(arguments[1:])
		if err != nil {
			return err
		}
		config, err := loadWorkerConfig(os.LookupEnv, workload)
		if err != nil {
			return err
		}
		syscall.Umask(0o077)
		context, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
		defer stop()
		return runWorker(context, config, paths, uid)
	default:
		return errors.New("unknown worker command")
	}
}

func main() {
	if err := run(os.Args[1:]); err != nil {
		fmt.Fprintln(os.Stderr, "polaris-seat-worker:", err)
		os.Exit(1)
	}
}
