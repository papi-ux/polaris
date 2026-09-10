//go:build linux

package seatprovider

import (
	"context"
	"errors"
	"io"
	"os"
	"path/filepath"
	"strings"
	"syscall"
	"time"

	"github.com/papi-ux/polaris/multiseat_worker/internal/seatruntime"
)

const pipeWireIsolationProperties = "{ support.dbus=false module.jackdbus-detect=false module.portal=false module.x11.bell=false module.raop=false }"

type audioArtifactSpec struct {
	relative string
	mode     uint32
	required bool
}

var audioArtifactSpecs = []audioArtifactSpec{
	{relative: "pulse/native", mode: syscall.S_IFSOCK, required: true},
	{relative: "pulse/pid", mode: syscall.S_IFREG, required: true},
	{relative: "pipewire-0", mode: syscall.S_IFSOCK, required: true},
	{relative: "pipewire-0.lock", mode: syscall.S_IFREG, required: true},
	{relative: "pipewire-0-manager", mode: syscall.S_IFSOCK},
	{relative: "pipewire-0-manager.lock", mode: syscall.S_IFREG},
	{relative: "pulse", mode: syscall.S_IFDIR, required: true},
}

func RunAudio(arguments []string, environment []string) error {
	request, err := parseProviderInvocation(
		seatruntime.StageAudio,
		arguments,
		environment,
	)
	if err != nil {
		return err
	}
	ready, err := openReadinessWriter()
	if err != nil {
		return err
	}
	options, err := normalizeProviderOptions(defaultProviderOptions())
	if err != nil {
		_ = ready.Close()
		return err
	}
	context, cancel := signalContext()
	defer cancel()
	return runAudio(context, request, ready, options)
}

func audioEnvironment(runtimePath string, sink string) []string {
	return []string{
		"LC_ALL=C",
		"HOME=/nonexistent",
		"XDG_CONFIG_HOME=/nonexistent",
		"XDG_RUNTIME_DIR=" + runtimePath,
		"DBUS_SESSION_BUS_ADDRESS=unix:path=" + filepath.Join(runtimePath, "bus"),
		"PIPEWIRE_RUNTIME_DIR=" + runtimePath,
		"PIPEWIRE_NODE=" + sink,
		"PULSE_SERVER=unix:" + filepath.Join(runtimePath, "pulse", "native"),
		"PULSE_SINK=" + sink,
	}
}

func rejectExistingAudioArtifacts(runtime *runtimeDirectory) error {
	if err := runtime.verify(); err != nil {
		return err
	}
	for _, spec := range audioArtifactSpecs {
		if _, err := lstatIdentity(filepath.Join(runtime.path, spec.relative)); !errors.Is(err, os.ErrNotExist) {
			return errors.New("runtime audio artifact already exists")
		}
	}
	return nil
}

func validAudioArtifact(identity artifactIdentity, spec audioArtifactSpec, uid uint32) bool {
	if identity.mode&syscall.S_IFMT != spec.mode || identity.uid != uid {
		return false
	}
	if spec.mode == syscall.S_IFDIR && identity.mode&0o7777 != 0o700 {
		return false
	}
	if spec.mode == syscall.S_IFREG && identity.mode&0o022 != 0 {
		return false
	}
	return true
}

func captureAudioArtifacts(runtime *runtimeDirectory) (map[string]artifactIdentity, error) {
	if err := runtime.verify(); err != nil {
		return nil, err
	}
	captured := make(map[string]artifactIdentity, len(audioArtifactSpecs))
	for _, spec := range audioArtifactSpecs {
		path := filepath.Join(runtime.path, spec.relative)
		identity, err := runtime.pins.capture(path)
		if errors.Is(err, os.ErrNotExist) && !spec.required {
			continue
		}
		if err != nil || !validAudioArtifact(identity, spec, runtime.uid) {
			return nil, errors.New("runtime audio artifact set is invalid")
		}
		captured[spec.relative] = identity
	}
	return captured, nil
}

func cleanupAudioArtifacts(
	runtime *runtimeDirectory,
	known map[string]artifactIdentity,
	allowOwnedCapture bool,
) error {
	if runtime == nil {
		return errors.New("runtime audio cleanup is invalid")
	}
	for _, spec := range audioArtifactSpecs {
		if spec.mode == syscall.S_IFDIR {
			continue
		}
		if err := runtime.verify(); err != nil {
			return err
		}
		path := filepath.Join(runtime.path, spec.relative)
		identity, err := runtime.pins.capture(path)
		if errors.Is(err, os.ErrNotExist) {
			continue
		}
		if err != nil || !validAudioArtifact(identity, spec, runtime.uid) {
			return errors.New("runtime audio cleanup found an unexpected artifact")
		}
		captured, present := known[spec.relative]
		if present {
			if !sameIdentity(identity, captured) {
				return errors.New("runtime audio artifact identity changed")
			}
		} else if !allowOwnedCapture {
			return errors.New("runtime audio cleanup found an unowned artifact")
		}
		if err := os.Remove(path); err != nil {
			return errors.New("runtime audio artifact could not be removed")
		}
	}
	pulseSpec := audioArtifactSpecs[len(audioArtifactSpecs)-1]
	if err := runtime.verify(); err != nil {
		return err
	}
	pulsePath := filepath.Join(runtime.path, pulseSpec.relative)
	identity, err := runtime.pins.capture(pulsePath)
	if errors.Is(err, os.ErrNotExist) {
		return nil
	}
	if err != nil || !validAudioArtifact(identity, pulseSpec, runtime.uid) {
		return errors.New("runtime audio cleanup found an unexpected directory")
	}
	captured, present := known[pulseSpec.relative]
	if present {
		if !sameIdentity(identity, captured) {
			return errors.New("runtime audio directory identity changed")
		}
	} else if !allowOwnedCapture {
		return errors.New("runtime audio cleanup found an unowned directory")
	}
	entries, err := os.ReadDir(pulsePath)
	if err != nil || len(entries) != 0 {
		return errors.New("runtime audio cleanup found unexpected directory content")
	}
	if err := runtime.verify(); err != nil {
		return err
	}
	if err := os.Remove(pulsePath); err != nil {
		return errors.New("runtime audio directory could not be removed")
	}
	return nil
}

func waitForProbe(
	parent context.Context,
	deadline time.Time,
	interval time.Duration,
	children []*managedChild,
	probe func(time.Duration) error,
) error {
	for {
		if parent == nil {
			return errors.New("runtime audio probe context is missing")
		}
		for _, child := range children {
			if child == nil || child.exited() {
				return errors.New("runtime audio child exited before readiness")
			}
		}
		remaining := time.Until(deadline)
		if remaining <= 0 {
			return errors.New("runtime audio readiness timed out")
		}
		if err := probe(remaining); err == nil {
			return nil
		}
		timer := time.NewTimer(interval)
		select {
		case <-parent.Done():
			if !timer.Stop() {
				<-timer.C
			}
			return errors.New("runtime audio startup was canceled")
		case <-timer.C:
		}
	}
}

func boundedProbeTimeout(remaining time.Duration, configured time.Duration) time.Duration {
	if remaining < configured {
		return remaining
	}
	return configured
}

func parseExactPactlName(output []byte, expected string) error {
	lines := strings.Split(strings.TrimSpace(string(output)), "\n")
	if len(lines) != 1 {
		return errors.New("runtime audio graph is not isolated")
	}
	fields := strings.Split(lines[0], "\t")
	if len(fields) < 2 || fields[1] != expected {
		return errors.New("runtime audio graph is not isolated")
	}
	return nil
}

func probePipeWireCore(options providerOptions, environment []string, remaining time.Duration) error {
	output, err := runTrustedCommand(
		options.pwCLIPath,
		options.executableOwnerUID,
		[]string{"-r", "pipewire-0", "info", "0"},
		environment,
		boundedProbeTimeout(remaining, options.probeTimeout),
	)
	if err != nil || !strings.Contains(string(output), "PipeWire:Interface:Core") {
		return errors.New("runtime PipeWire core is unavailable")
	}
	return nil
}

func probeAudioGraph(
	options providerOptions,
	environment []string,
	sink string,
	remaining time.Duration,
) error {
	deadline := time.Now().Add(remaining)
	nextTimeout := func() (time.Duration, error) {
		remaining := time.Until(deadline)
		if remaining <= 0 {
			return 0, errors.New("runtime audio protocol probe timed out")
		}
		return boundedProbeTimeout(remaining, options.probeTimeout), nil
	}
	server := "--server=unix:" + filepath.Join(options.runtimeDirectory, "pulse", "native")
	timeout, err := nextTimeout()
	if err != nil {
		return err
	}
	info, err := runTrustedCommand(
		options.pactlPath,
		options.executableOwnerUID,
		[]string{server, "info"},
		environment,
		timeout,
	)
	if err != nil || strings.TrimSpace(string(info)) == "" {
		return errors.New("runtime Pulse protocol is unavailable")
	}
	timeout, err = nextTimeout()
	if err != nil {
		return err
	}
	sinks, err := runTrustedCommand(
		options.pactlPath,
		options.executableOwnerUID,
		[]string{server, "list", "short", "sinks"},
		environment,
		timeout,
	)
	if err != nil || parseExactPactlName(sinks, sink) != nil {
		return errors.New("runtime audio sink is unavailable")
	}
	timeout, err = nextTimeout()
	if err != nil {
		return err
	}
	sources, err := runTrustedCommand(
		options.pactlPath,
		options.executableOwnerUID,
		[]string{server, "list", "short", "sources"},
		environment,
		timeout,
	)
	if err != nil || parseExactPactlName(sources, sink+".monitor") != nil {
		return errors.New("runtime audio monitor is unavailable")
	}
	return nil
}

func runAudio(
	parent context.Context,
	request seatruntime.Request,
	ready io.WriteCloser,
	options providerOptions,
) (result error) {
	if parent == nil || ready == nil || request.Stage != seatruntime.StageAudio {
		if ready != nil {
			_ = ready.Close()
		}
		return errors.New("runtime audio provider is invalid")
	}
	defer func() {
		if ready != nil {
			_ = ready.Close()
		}
	}()
	if _, err := seatruntime.Arguments(request); err != nil {
		return errors.New("runtime audio request is invalid")
	}
	options, err := normalizeProviderOptions(options)
	if err != nil {
		return err
	}
	select {
	case <-parent.Done():
		return errors.New("runtime audio startup was canceled")
	default:
	}
	runtime, err := openRuntimeDirectory(options.runtimeDirectory, options.runtimeOwnerUID)
	if err != nil {
		return err
	}
	defer runtime.close()
	if err := rejectExistingAudioArtifacts(runtime); err != nil {
		return err
	}
	environment := audioEnvironment(runtime.path, request.AudioSink)
	pipeWire, err := startManagedChild(
		options.pipeWirePath,
		options.executableOwnerUID,
		[]string{"-c", "pipewire.conf", "-P", pipeWireIsolationProperties},
		environment,
		nil,
	)
	if err != nil {
		return err
	}
	var pulse *managedChild
	var artifacts map[string]artifactIdentity
	readyPublished := false
	defer func() {
		var pulseStopError error
		if pulse != nil {
			pulseStopError = pulse.stop(options.stopTimeout)
		}
		pipeWireStopError := pipeWire.stop(options.stopTimeout)
		cleanupError := cleanupAudioArtifacts(runtime, artifacts, !readyPublished)
		result = errors.Join(result, pulseStopError, pipeWireStopError, cleanupError)
	}()
	coreDeadline := time.Now().Add(options.startupTimeout)
	if err := waitForProbe(
		parent,
		coreDeadline,
		options.probeInterval,
		[]*managedChild{pipeWire},
		func(remaining time.Duration) error {
			return probePipeWireCore(options, environment, remaining)
		},
	); err != nil {
		return err
	}
	nodeProperties := "{ factory.name=support.null-audio-sink node.name=" +
		request.AudioSink +
		" node.description=\"Polaris isolated stream\" media.class=Audio/Sink" +
		" object.linger=true node.virtual=true node.pause-on-idle=false" +
		" audio.position=[ FL FR ] }"
	if _, err := runTrustedCommand(
		options.pwCLIPath,
		options.executableOwnerUID,
		[]string{"-r", "pipewire-0", "create-node", "adapter", nodeProperties},
		environment,
		options.probeTimeout,
	); err != nil || pipeWire.exited() {
		return errors.New("runtime audio sink could not be created")
	}
	pulse, err = startManagedChild(
		options.pipeWirePulsePath,
		options.executableOwnerUID,
		[]string{"-c", "pipewire-pulse.conf", "-P", pipeWireIsolationProperties},
		environment,
		nil,
	)
	if err != nil {
		return err
	}
	audioDeadline := time.Now().Add(options.startupTimeout)
	if err := waitForProbe(
		parent,
		audioDeadline,
		options.probeInterval,
		[]*managedChild{pipeWire, pulse},
		func(remaining time.Duration) error {
			return probeAudioGraph(options, environment, request.AudioSink, remaining)
		},
	); err != nil {
		return err
	}
	if err := runtime.verify(); err != nil {
		return err
	}
	artifacts, err = captureAudioArtifacts(runtime)
	if err != nil {
		return err
	}
	if err := publishReadiness(ready); err != nil {
		return err
	}
	readyPublished = true
	ready = nil
	select {
	case <-parent.Done():
		return nil
	case <-pipeWire.done:
		return errors.New("runtime PipeWire core exited unexpectedly")
	case <-pulse.done:
		return errors.New("runtime Pulse service exited unexpectedly")
	}
}
