//go:build linux

package seatprovider

import (
	"context"
	"errors"
	"io"
	"os"
	"path/filepath"
	"strconv"
	"time"

	"github.com/papi-ux/polaris/multiseat_worker/internal/seatruntime"
)

// The encoder is the second half of the worker-local media path. Display
// capture publishes raw frames on the capture endpoint; this provider consumes
// them, encodes, and publishes encoded packets on its own endpoint for the
// worker's data plane to read. One process per seat, because sharing an encoder
// instance across seats is forbidden and a process makes that structural rather
// than something a reviewer has to keep checking.
const (
	encoderKeyframeInterval = 60
	encoderStartupTimeout   = 20 * time.Second
)

// RunEncoder is the provider entrypoint for seatruntime.StageEncoder.
func RunEncoder(arguments []string, environment []string) error {
	request, err := parseProviderInvocation(
		seatruntime.StageEncoder,
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
	options := defaultProviderOptions()
	options.startupTimeout = encoderStartupTimeout
	options, err = normalizeProviderOptions(options)
	if err != nil {
		_ = ready.Close()
		return err
	}
	context, cancel := signalContext()
	defer cancel()
	return runEncoder(context, request, ready, options)
}

// encoderArguments builds the worker-local encode pipeline.
//
// Only a software H.264 encoder is reachable today: the locked worker roots
// register openh264 and nothing else, because NVIDIA exposes no VA-API encode
// and the nvcodec plugin is absent from the image. The element is chosen here
// rather than inline so that adding a hardware encoder is a branch plus an
// image change, not a rewrite of the provider.
func encoderArguments(
	request seatruntime.Request,
	captureSocket string,
	encodedSocket string,
	software bool,
) []string {
	return []string{
		"-q",
		"unixfdsrc", "socket-path=" + captureSocket, "!",
		displayCaps(request, software), "!",
		"videoconvert", "!",
		"video/x-raw,format=I420", "!",
		"openh264enc",
		"gop-size=" + strconv.Itoa(encoderKeyframeInterval),
		"usage-type=screen",
		"complexity=low",
		"!",
		"video/x-h264,stream-format=byte-stream,alignment=au", "!",
		"unixfdsink",
		"socket-path=" + encodedSocket,
		"sync=false", "async=false", "enable-last-sample=false", "wait-for-connection=false",
	}
}

// encoderProbeArguments proves an encoded packet actually crosses the endpoint
// before readiness is published, the way display capture proves a raw frame
// does. A socket that exists is not a socket that carries anything.
func encoderProbeArguments(encodedSocket string) []string {
	return []string{
		"-q",
		"unixfdsrc",
		"socket-path=" + encodedSocket,
		"num-buffers=1",
		"!",
		"video/x-h264,stream-format=byte-stream,alignment=au",
		"!",
		"fakesink",
		"sync=false",
		"async=false",
		"enable-last-sample=false",
	}
}

func runEncoder(
	parent context.Context,
	request seatruntime.Request,
	ready io.WriteCloser,
	options providerOptions,
) (result error) {
	if parent == nil || ready == nil || request.Stage != seatruntime.StageEncoder {
		if ready != nil {
			_ = ready.Close()
		}
		return errors.New("runtime encoder provider is invalid")
	}
	defer func() {
		if ready != nil {
			_ = ready.Close()
		}
	}()
	if _, err := seatruntime.Arguments(request); err != nil {
		return errors.New("runtime encoder request is invalid")
	}
	if request.MediaPipeline != seatruntime.MediaPipelineWorkerLocal {
		return errors.New("runtime encoder media pipeline is unsupported")
	}
	if request.EncoderSessions == 0 {
		return errors.New("runtime encoder session budget is invalid")
	}
	options, err := normalizeProviderOptions(options)
	if err != nil {
		return err
	}
	select {
	case <-parent.Done():
		return errors.New("runtime encoder startup was canceled")
	default:
	}
	runtime, err := openRuntimeDirectory(options.runtimeDirectory, options.runtimeOwnerUID)
	if err != nil {
		return err
	}
	defer runtime.close()

	captureName, err := seatruntime.CaptureMediaSocketName(request.RuntimeNamespace)
	if err != nil {
		return errors.New("runtime encoder capture endpoint is invalid")
	}
	encodedName, err := seatruntime.EncodedMediaSocketName(request.RuntimeNamespace)
	if err != nil {
		return errors.New("runtime encoder media endpoint is invalid")
	}
	captureSocket := filepath.Join(runtime.path, captureName)
	encodedSocket := filepath.Join(runtime.path, encodedName)
	if !validUnixSocketPath(captureSocket) || !validUnixSocketPath(encodedSocket) {
		return errors.New("runtime encoder socket path is too long")
	}
	if !options.softwareDisplay {
		if err := validateDisplayRenderNode(request.RenderNode); err != nil {
			return err
		}
	}
	if err := rejectExistingEncodedArtifact(runtime, encodedSocket); err != nil {
		return err
	}

	child, err := startManagedChildWithUmask(
		options.gstLaunchPath,
		options.executableOwnerUID,
		encoderArguments(request, captureSocket, encodedSocket, options.softwareDisplay),
		displayEnvironment(options),
		nil,
		0o077,
	)
	if err != nil {
		return err
	}
	published := false
	defer func() {
		stopError := child.stop(options.stopTimeout)
		cleanupError := cleanupEncodedArtifact(runtime, encodedSocket, !published)
		result = errors.Join(result, stopError, cleanupError)
	}()

	deadline := time.Now().Add(options.startupTimeout)
	if err := waitForEncodedEndpoint(
		parent,
		child,
		runtime,
		encodedSocket,
		deadline,
		options.probeInterval,
	); err != nil {
		return err
	}
	remaining := time.Until(deadline)
	if remaining <= 0 {
		return errors.New("runtime encoder packet readiness timed out")
	}
	// An endpoint that exists is not an endpoint that carries anything, so
	// readiness waits for one real encoded packet the way capture waits for one
	// real frame.
	if _, err := runTrustedCommand(
		options.gstLaunchPath,
		options.executableOwnerUID,
		encoderProbeArguments(encodedSocket),
		displayEnvironment(options),
		remaining,
	); err != nil {
		return errors.New("runtime encoder packet transport is unavailable")
	}
	if child.exited() {
		return errors.New(
			"runtime encoder exited before readiness with " + describeChildExit(child),
		)
	}
	if err := publishReadiness(ready); err != nil {
		return err
	}
	published = true
	ready = nil
	select {
	case <-parent.Done():
		return nil
	case <-child.done:
		return errors.New(
			"runtime encoder exited unexpectedly with " + describeChildExit(child),
		)
	}
}

func rejectExistingEncodedArtifact(runtime *runtimeDirectory, encodedSocket string) error {
	if _, err := os.Lstat(encodedSocket); err == nil {
		return errors.New("runtime encoder artifact already exists")
	} else if !errors.Is(err, os.ErrNotExist) {
		return errors.New("runtime encoder artifact could not be inspected")
	}
	return nil
}

func cleanupEncodedArtifact(runtime *runtimeDirectory, encodedSocket string, remove bool) error {
	if !remove {
		return nil
	}
	if err := os.Remove(encodedSocket); err != nil && !errors.Is(err, os.ErrNotExist) {
		return errors.New("runtime encoder artifact could not be removed")
	}
	return nil
}

func waitForEncodedEndpoint(
	parent context.Context,
	child *managedChild,
	runtime *runtimeDirectory,
	encodedSocket string,
	deadline time.Time,
	interval time.Duration,
) error {
	ticker := time.NewTicker(interval)
	defer ticker.Stop()
	for {
		info, err := os.Lstat(encodedSocket)
		if err == nil {
			if info.Mode()&os.ModeSocket == 0 {
				return errors.New("runtime encoder artifact is not a socket")
			}
			return nil
		}
		if !errors.Is(err, os.ErrNotExist) {
			return errors.New("runtime encoder artifact could not be inspected")
		}
		if time.Now().After(deadline) {
			return errors.New("runtime encoder endpoint did not appear")
		}
		select {
		case <-parent.Done():
			return errors.New("runtime encoder startup was canceled")
		case <-child.done:
			return errors.New(
				"runtime encoder exited before publishing its endpoint with " +
					describeChildExit(child),
			)
		case <-ticker.C:
		}
	}
}
