//go:build linux

package seatprovider

import (
	"context"
	"encoding/binary"
	"errors"
	"io"
	"net"
	"os"
	"path/filepath"
	"strconv"
	"sync"
	"time"

	"github.com/papi-ux/polaris/multiseat_worker/internal/seatmedia"
	"github.com/papi-ux/polaris/multiseat_worker/internal/seatruntime"
)

const encoderStartupTimeout = 20 * time.Second

func RunEncoder(arguments []string, environment []string) error {
	request, err := parseProviderInvocation(seatruntime.StageEncoder, arguments, environment)
	if err != nil {
		return err
	}
	ready, err := openReadinessWriter()
	if err != nil {
		return err
	}
	options := defaultProviderOptions()
	options.startupTimeout = encoderStartupTimeout
	ctx, cancel := signalContext()
	defer cancel()
	return runEncoder(ctx, request, ready, options)
}

func encoderArguments(request seatruntime.Request, captureSocket string, software bool) []string {
	return []string{captureSocket, request.RenderNode, request.AudioSink,
		strconv.FormatUint(uint64(request.DisplayWidth), 10), strconv.FormatUint(uint64(request.DisplayHeight), 10),
		strconv.FormatUint(uint64(request.DisplayRefreshMillihertz), 10), strconv.FormatBool(software)}
}

// Readiness binds the native producer's contract to this allocation. The
// worker independently checks it again before announcing to the controller.
func validEncoderContract(body []byte, request seatruntime.Request) bool {
	if len(body) != 32 || body[0] != 1 || body[1] != 1 || body[2] != 66 ||
		body[3] < 10 || body[3] > 62 || binary.BigEndian.Uint32(body[28:32]) != 0 {
		return false
	}
	numerator, denominator := binary.BigEndian.Uint32(body[8:12]), binary.BigEndian.Uint32(body[12:16])
	return uint32(binary.BigEndian.Uint16(body[4:6])) == request.DisplayWidth &&
		uint32(binary.BigEndian.Uint16(body[6:8])) == request.DisplayHeight &&
		numerator > 0 && denominator > 0 && uint64(numerator)*1000 == uint64(request.DisplayRefreshMillihertz)*uint64(denominator) &&
		binary.BigEndian.Uint32(body[16:20]) == 8000 &&
		body[20] == 1 && body[21] == 2 && binary.BigEndian.Uint16(body[22:24]) == 5000 &&
		binary.BigEndian.Uint32(body[24:28]) == 48000
}

// The native child owns both pipelines. Its bounded binary output goes to one
// local consumer; independent controls can still recover a backpressured stream.
func runEncoder(parent context.Context, request seatruntime.Request, ready io.WriteCloser, options providerOptions) (result error) {
	if ready != nil {
		defer func() {
			if ready != nil {
				_ = ready.Close()
			}
		}()
	}
	if parent == nil || ready == nil || request.Stage != seatruntime.StageEncoder {
		return errors.New("runtime encoder provider is invalid")
	}
	if _, err := seatruntime.Arguments(request); err != nil {
		return err
	}
	options, err := normalizeProviderOptions(options)
	if err != nil {
		return err
	}
	runtime, err := openRuntimeDirectory(options.runtimeDirectory, options.runtimeOwnerUID)
	if err != nil {
		return err
	}
	defer runtime.close()
	captureName, err := seatruntime.CaptureMediaSocketName(request.RuntimeNamespace)
	if err != nil {
		return err
	}
	encodedName, err := seatruntime.EncodedMediaSocketName(request.RuntimeNamespace)
	if err != nil {
		return err
	}
	capturePath, encodedPath := filepath.Join(runtime.path, captureName), filepath.Join(runtime.path, encodedName)
	if !validUnixSocketPath(capturePath) || !validUnixSocketPath(encodedPath) {
		return errors.New("encoder endpoint path is too long")
	}
	if _, err := os.Lstat(encodedPath); !errors.Is(err, os.ErrNotExist) {
		return errors.New("encoder endpoint already exists or is unavailable")
	}
	listener, err := net.ListenUnix("unix", &net.UnixAddr{Name: encodedPath, Net: "unix"})
	if err != nil {
		return err
	}
	listener.SetUnlinkOnClose(false)
	defer listener.Close()
	if err := os.Chmod(encodedPath, 0o600); err != nil {
		return err
	}
	identity, err := runtime.pins.capture(encodedPath)
	if err != nil {
		return err
	}
	defer func() {
		after, err := lstatIdentity(encodedPath)
		if runtime.verify() != nil || err != nil || !sameIdentity(identity, after) {
			result = errors.Join(result, errors.New("encoder endpoint ownership changed during cleanup"))
			return
		}
		result = errors.Join(result, os.Remove(encodedPath))
	}()
	mediaRead, mediaWrite, err := os.Pipe()
	if err != nil {
		return err
	}
	defer mediaRead.Close()
	defer mediaWrite.Close()
	controlRead, controlWrite, err := os.Pipe()
	if err != nil {
		return err
	}
	defer controlRead.Close()
	defer controlWrite.Close()
	child, err := startManagedChildWithUmask(options.encoderPath, options.executableOwnerUID,
		encoderArguments(request, capturePath, options.softwareDisplay), displayEnvironment(options),
		[]*os.File{mediaWrite, controlRead}, 0o077)
	if err != nil {
		return err
	}
	mediaWrite.Close()
	controlRead.Close()
	stopChild := sync.OnceValue(func() error { return child.stop(options.stopTimeout) })
	defer func() { result = errors.Join(result, stopChild()) }()
	type packet struct {
		kind byte
		body []byte
		err  error
	}
	first := make(chan packet, 1)
	packets := seatmedia.NewReader(mediaRead)
	go func() { kind, body, err := packets.Read(); first <- packet{kind, body, err} }()
	startup := time.NewTimer(options.startupTimeout)
	defer startup.Stop()
	var contract packet
	select {
	case contract = <-first:
		if contract.err != nil || contract.kind != seatmedia.Config || !validEncoderContract(contract.body, request) {
			return errors.New("encoder did not produce a media contract")
		}
	case <-startup.C:
		return errors.New("encoder media readiness timed out")
	case <-parent.Done():
		return parent.Err()
	case <-child.done:
		return child.exitError("encoder exited before media readiness")
	}
	// The child emits this contract only after inspecting actual video and audio
	// samples. It emits no frames before Start follows the controller's ack.
	if child.exited() {
		return child.exitError("encoder exited before readiness")
	}
	if err := publishReadiness(ready); err != nil {
		return err
	}
	ready = nil
	stopAccept := context.AfterFunc(parent, func() { listener.Close() })
	defer stopAccept()
	acceptFinished := make(chan struct{})
	defer close(acceptFinished)
	go func() {
		select {
		case <-child.done:
			listener.Close()
		case <-acceptFinished:
		}
	}()
	connection, err := listener.AcceptUnix()
	if parent.Err() != nil {
		if connection != nil {
			connection.Close()
		}
		return nil
	}
	if err != nil {
		return errors.New("encoder consumer accept failed")
	}
	defer connection.Close()
	listener.Close() // No reconnect or second reader for this generation.
	if err := verifyUnixPeer(connection, 0, options.runtimeOwnerUID); err != nil {
		return err
	}
	ctx, cancel := context.WithCancel(parent)
	defer cancel()
	stopIO := context.AfterFunc(ctx, func() { connection.Close() })
	defer stopIO()
	failures := make(chan error, 2)
	var pumps sync.WaitGroup
	defer func() {
		cancel()
		connection.Close()
		// Stop the producer before closing its pipe ends so cancellation cannot
		// race a normal SIGTERM into an unexpected EOF or broken pipe failure.
		_ = stopChild()
		mediaRead.Close()
		controlWrite.Close()
		pumps.Wait()
	}()
	pumps.Add(2)
	go func() {
		defer pumps.Done()
		next := contract
		for {
			if next.err != nil {
				failures <- next.err
				return
			}
			if err := connection.SetWriteDeadline(time.Now().Add(5 * time.Second)); err != nil {
				failures <- err
				return
			}
			if err := seatmedia.Write(connection, next.kind, next.body); err != nil {
				failures <- err
				return
			}
			// Write has consumed the borrowed packet before the next Read can
			// reuse its storage. No payload is queued or shared with another seat.
			kind, body, err := packets.Read()
			next = packet{kind, body, err}
			if err == nil && kind == seatmedia.Config {
				failures <- errors.New("encoder changed its contract")
				return
			}
		}
	}()
	go func() {
		defer pumps.Done()
		started := false
		for {
			var command [1]byte
			if _, err := io.ReadFull(connection, command[:]); err != nil {
				failures <- err
				return
			}
			if (!started && command[0] != seatmedia.Start) || (started && command[0] != seatmedia.RequestIDR) {
				failures <- errors.New("invalid encoder control transition")
				return
			}
			started = true
			if err := controlWrite.SetWriteDeadline(time.Now().Add(2 * time.Second)); err != nil {
				failures <- err
				return
			}
			if _, err := controlWrite.Write(command[:]); err != nil {
				failures <- err
				return
			}
		}
	}()
	select {
	case <-parent.Done():
		return nil
	case <-child.done:
		if parent.Err() != nil {
			return nil
		}
		return child.exitError("encoder process exited")
	case <-failures:
		if parent.Err() != nil {
			return nil
		}
		return errors.New("encoder media or control connection failed")
	}
}
