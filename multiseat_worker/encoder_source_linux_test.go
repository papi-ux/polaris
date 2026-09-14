//go:build linux

package main

import (
	"context"
	"encoding/binary"
	"errors"
	"io"
	"net"
	"os"
	"path/filepath"
	"sync"
	"syscall"
	"testing"
	"time"

	"github.com/papi-ux/polaris/multiseat_worker/internal/seatmedia"
	"github.com/papi-ux/polaris/multiseat_worker/internal/seatruntime"
)

func encoderFixture(t *testing.T, serve func(*net.UnixConn, mediaConfig)) (*encoderMediaSource, mediaConfig) {
	t.Helper()
	root, err := os.MkdirTemp("/tmp", "em-")
	if err != nil {
		t.Fatal(err)
	}
	config := runtimeTestConfig("encoder-source", 41, 2)
	contract := goldenMediaConfig()
	contract.FPSNumerator, contract.FPSDenominator = 60000, 1000
	contract.AudioChannels, contract.AudioFrameDurationUS = 2, 5000
	config.DisplayWidth, config.DisplayHeight = uint32(contract.Width), uint32(contract.Height)
	config.RefreshMillihz, config.DisplayHDR = 60000, false
	name, err := seatruntime.EncodedMediaSocketName(config.RuntimeNamespace)
	if err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(root, name)
	listener, err := net.ListenUnix("unix", &net.UnixAddr{Name: path, Net: "unix"})
	if err != nil {
		os.Remove(root)
		t.Fatal(err)
	}
	if err := os.Chmod(path, 0o600); err != nil {
		t.Fatal(err)
	}
	source := newEncoderMediaSource(config, root, uint32(os.Geteuid()))
	var group sync.WaitGroup
	group.Add(1)
	go func() {
		defer group.Done()
		connection, err := listener.AcceptUnix()
		if err == nil {
			defer connection.Close()
			connection.SetDeadline(time.Now().Add(3 * time.Second))
			serve(connection, contract)
		}
	}()
	t.Cleanup(func() { source.Close(); listener.Close(); group.Wait(); os.Remove(root) })
	return source, contract
}

func writeEncoderContract(t *testing.T, connection *net.UnixConn, contract mediaConfig) {
	t.Helper()
	body, err := encodeMediaConfig(contract)
	if err == nil {
		err = seatmedia.Write(connection, seatmedia.Config, body)
	}
	if err != nil {
		t.Error(err)
	}
}

func TestEncoderSourceStartsAfterAckAndControlsRemainIndependentOfReads(t *testing.T) {
	commands := make(chan byte, 4)
	source, _ := encoderFixture(t, func(connection *net.UnixConn, expected mediaConfig) {
		writeEncoderContract(t, connection, expected)
		for index := 0; index < 3; index++ {
			var command [1]byte
			if _, err := io.ReadFull(connection, command[:]); err != nil {
				return
			}
			commands <- command[0]
			if index == 1 {
				body, _ := encodeMediaFrame(mediaFrame{FrameIndex: 1, IDR: true, EncodeTimestampNS: 42}, []byte{0, 0, 0, 1, 0x65, 1})
				_ = seatmedia.Write(connection, seatmedia.Video, body)
			}
		}
	})
	plane := newSeatDataPlane(source.config.Identity, source, nil)
	ctx, cancel := context.WithTimeout(t.Context(), 2*time.Second)
	defer cancel()
	if _, err := plane.NextMedia(ctx); err != nil {
		t.Fatal(err)
	}
	select {
	case command := <-commands:
		t.Fatalf("command before ack: %d", command)
	default:
	}
	if err := plane.RouteMediaControl(ctx, routedMediaControl{Identity: source.config.Identity, Message: messageMediaConfigAck}); err != nil {
		t.Fatal(err)
	}
	result := make(chan error, 1)
	go func() {
		output, err := plane.NextMedia(ctx)
		if err == nil && output.Message != messageVideo {
			err = errors.New("not video")
		}
		result <- err
	}()
	if command := <-commands; command != seatmedia.Start {
		t.Fatal(command)
	}
	if err := source.Keyframe(ctx); err != nil {
		t.Fatal(err)
	}
	if command := <-commands; command != seatmedia.RequestIDR {
		t.Fatal(command)
	}
	if err := <-result; err != nil {
		t.Fatal(err)
	}
	if err := source.Invalidate(ctx, frameRange{First: 1, Last: 1}); err != nil {
		t.Fatal(err)
	}
	if command := <-commands; command != seatmedia.RequestIDR {
		t.Fatal(command)
	}
}

func TestEncoderSourceRefusesContractAndFrameDrift(t *testing.T) {
	for _, mutation := range []string{"dimensions", "rate", "codec", "duplicate-contract", "first-delta", "audio-idr", "frame-regression"} {
		t.Run(mutation, func(t *testing.T) {
			source, _ := encoderFixture(t, func(connection *net.UnixConn, contract mediaConfig) {
				selected := contract
				if mutation == "dimensions" {
					selected.Width += 2
				}
				if mutation == "rate" {
					selected.FPSNumerator = 30000
				}
				body, _ := encodeMediaConfig(selected)
				if mutation == "codec" {
					body[1] = 99
				}
				if seatmedia.Write(connection, seatmedia.Config, body) != nil {
					return
				}
				var start [1]byte
				if _, err := io.ReadFull(connection, start[:]); err != nil {
					return
				}
				if mutation == "duplicate-contract" {
					_ = seatmedia.Write(connection, seatmedia.Config, body)
					return
				}
				frame := mediaFrame{FrameIndex: 2, IDR: true, EncodeTimestampNS: 100}
				kind := seatmedia.Video
				if mutation == "first-delta" {
					frame.IDR = false
				}
				if mutation == "audio-idr" {
					kind = seatmedia.Audio
				}
				body, _ = encodeMediaFrame(frame, []byte{1})
				_ = seatmedia.Write(connection, kind, body)
				if mutation == "frame-regression" {
					_ = seatmedia.Write(connection, kind, body)
				}
			})
			ctx, cancel := context.WithTimeout(t.Context(), 2*time.Second)
			defer cancel()
			_, err := source.Contract(ctx)
			if mutation == "dimensions" || mutation == "rate" || mutation == "codec" {
				if err == nil {
					t.Fatal("contract drift accepted")
				}
				return
			}
			if err != nil {
				t.Fatal(err)
			}
			_, _, _, err = source.Next(ctx)
			if mutation == "frame-regression" {
				if err != nil {
					t.Fatal(err)
				}
				_, _, _, err = source.Next(ctx)
			}
			if err == nil {
				t.Fatal("frame drift accepted")
			}
		})
	}
}

func TestEncoderSourceCancellationRetiresBlockedConnection(t *testing.T) {
	entered := make(chan struct{})
	source, _ := encoderFixture(t, func(connection *net.UnixConn, _ mediaConfig) {
		close(entered)
		var body [1]byte
		_, _ = connection.Read(body[:])
	})
	ctx, cancel := context.WithCancel(t.Context())
	result := make(chan error, 1)
	go func() { _, err := source.Contract(ctx); result <- err }()
	<-entered
	cancel()
	select {
	case err := <-result:
		if err == nil {
			t.Fatal("canceled read succeeded")
		}
	case <-time.After(time.Second):
		t.Fatal("canceled read blocked")
	}
	if _, err := source.Contract(t.Context()); err == nil {
		t.Fatal("retired connection reopened")
	}
}

func TestEncoderSourceRefusesSymlinksAndBroadSocketPermissions(t *testing.T) {
	for _, symlink := range []bool{false, true} {
		source, _ := encoderFixture(t, func(*net.UnixConn, mediaConfig) { t.Error("unsafe endpoint was connected") })
		name, _ := seatruntime.EncodedMediaSocketName(source.config.RuntimeNamespace)
		path := filepath.Join(source.directory, name)
		if symlink {
			original := path + "-old"
			if err := os.Rename(path, original); err != nil {
				t.Fatal(err)
			}
			if err := os.Symlink(original, path); err != nil {
				t.Fatal(err)
			}
			t.Cleanup(func() { os.Remove(original); os.Remove(path) })
		} else {
			if err := os.Chmod(path, 0o666); err != nil {
				t.Fatal(err)
			}
		}
		if _, err := source.Contract(t.Context()); err == nil {
			t.Fatal("unsafe encoder endpoint accepted")
		}
	}
}

func TestWorkerMediaModeRequiresTheExactFinalOption(t *testing.T) {
	base := []string{"--workload-kind=gamescope", "--workload-id=input-pong-v1"}
	if _, enabled, err := parseWorkerRunMode(base); err != nil || enabled {
		t.Fatal("default worker activated media", err)
	}
	if _, enabled, err := parseWorkerRunMode(append(base, "--media=enabled")); err != nil || !enabled {
		t.Fatal("explicit media selection failed", err)
	}
	for _, arguments := range [][]string{append(base, "--media=true"), append(base, "--media=enabled", "--media=enabled"), append([]string{"--media=enabled"}, base...)} {
		if _, _, err := parseWorkerRunMode(arguments); err == nil {
			t.Fatal("ambiguous media selection accepted")
		}
	}
}

// Cancellation must unblock the caller without disconnecting a still-live
// provider. The runtime stops the launcher before the encoder; drain discarded
// media until that ordered shutdown closes the source.
func TestEncoderSourceCancellationDrainsUntilOwnerCloses(t *testing.T) {
	for _, stage := range []string{"contract", "frame"} {
		t.Run(stage, func(t *testing.T) {
			entered := make(chan struct{})
			probe := make(chan struct{})
			result := make(chan error, 1)
			peerClosed := make(chan error, 1)
			source, _ := encoderFixture(t, func(connection *net.UnixConn, contract mediaConfig) {
				if stage == "frame" {
					writeEncoderContract(t, connection, contract)
					var command [1]byte
					if _, err := io.ReadFull(connection, command[:]); err != nil {
						result <- err
						return
					}
				}
				close(entered)
				<-probe
				// A full socket buffer must not turn requested shutdown into
				// the provider's five-second backpressure failure.
				_, err := connection.Write(make([]byte, 2*1024*1024))
				result <- err
				var b [1]byte
				_, err = connection.Read(b[:])
				peerClosed <- err
			})
			if stage == "frame" {
				if _, err := source.Contract(t.Context()); err != nil {
					t.Fatal(err)
				}
			}
			ctx, cancel := context.WithCancel(t.Context())
			defer cancel()
			canceled := make(chan error, 1)
			go func() {
				if stage == "contract" {
					_, err := source.Contract(ctx)
					canceled <- err
				} else {
					_, _, _, err := source.Next(ctx)
					canceled <- err
				}
			}()
			<-entered
			cancel()
			select {
			case err := <-canceled:
				if err == nil {
					t.Fatal("canceled operation succeeded")
				}
			case <-time.After(time.Second):
				t.Fatal("canceled operation blocked")
			}
			close(probe)
			if err := <-result; err != nil {
				t.Fatalf("provider lost its connection before ordered shutdown: %v", err)
			}
			if _, err := source.Contract(t.Context()); err == nil {
				t.Fatal("retired connection reused")
			}
			select {
			case err := <-peerClosed:
				t.Fatalf("provider observed early close: %v", err)
			default:
			}
			if err := source.Close(); err != nil {
				t.Fatal(err)
			}
			select {
			case err := <-peerClosed:
				// Closing with bytes still queued in the drain can report reset
				// on Linux. Both results prove closure after the owner's Close;
				// the checks above still reject any earlier disconnection.
				if !errors.Is(err, io.EOF) && !errors.Is(err, syscall.ECONNRESET) {
					t.Fatalf("owner cleanup did not close provider connection: %v", err)
				}
			case <-time.After(2 * time.Second):
				t.Fatal("owner cleanup left provider connection open")
			}
		})
	}
}

func TestEncoderSourceUnexpectedDisconnectStillFails(t *testing.T) {
	source, _ := encoderFixture(t, func(connection *net.UnixConn, contract mediaConfig) {
		writeEncoderContract(t, connection, contract)
	})
	if _, err := source.Contract(t.Context()); err != nil {
		t.Fatal(err)
	}
	if _, _, _, err := source.Next(t.Context()); err == nil {
		t.Fatal("unexpected provider disconnect accepted")
	}
}

func TestEncoderBitrateWaitsForNativeConfirmationAndRetiresFailures(t *testing.T) {
	for _, scenario := range []string{"confirmed", "wrong-rate", "frame-instead", "cancel"} {
		t.Run(scenario, func(t *testing.T) {
			received, respond := make(chan struct{}), make(chan struct{})
			source, _ := encoderFixture(t, func(connection *net.UnixConn, contract mediaConfig) {
				writeEncoderContract(t, connection, contract)
				var command [5]byte
				if _, err := io.ReadFull(connection, command[:]); err != nil {
					t.Error(err)
					return
				}
				if command[0] != seatmedia.SelectBitrate || binary.BigEndian.Uint32(command[1:]) != 4000 {
					t.Error("wrong selected bitrate")
				}
				close(received)
				<-respond
				if scenario == "cancel" {
					return
				}
				body := []byte{0, 0, 0x0f, 0xa0}
				kind := seatmedia.BitrateSelected
				if scenario == "wrong-rate" {
					body[3]++
				}
				if scenario == "frame-instead" {
					kind = seatmedia.Video
					body, _ = encodeMediaFrame(mediaFrame{IDR: true, EncodeTimestampNS: 42}, []byte("frame"))
				}
				_ = seatmedia.Write(connection, kind, body)
				if scenario == "confirmed" {
					var start [1]byte
					if _, err := io.ReadFull(connection, start[:]); err != nil || start[0] != seatmedia.Start {
						t.Error("missing Start")
						return
					}
					body, _ = encodeMediaFrame(mediaFrame{IDR: true, EncodeTimestampNS: 42}, []byte("frame"))
					_ = seatmedia.Write(connection, seatmedia.Video, body)
				}
			})
			if _, err := source.Contract(t.Context()); err != nil {
				t.Fatal(err)
			}
			ctx, cancel := context.WithCancel(t.Context())
			defer cancel()
			result := make(chan error, 1)
			go func() { result <- source.SelectBitrate(ctx, 4000) }()
			<-received
			select {
			case err := <-result:
				t.Fatalf("selection acknowledged before encoder confirmation: %v", err)
			default:
			}
			if scenario == "cancel" {
				cancel()
			}
			close(respond)
			err := <-result
			if scenario == "confirmed" {
				if err != nil {
					t.Fatal(err)
				}
				if err := source.SelectBitrate(t.Context(), 4000); err == nil {
					t.Fatal("duplicate selection accepted")
				}
				if kind, frame, _, err := source.Next(t.Context()); err != nil || kind != messageVideo || !frame.IDR {
					t.Fatal("selected encoder did not start with an IDR", err)
				}
			} else {
				if err == nil {
					t.Fatal("unconfirmed selection accepted")
				}
				if _, err := source.Contract(t.Context()); err == nil {
					t.Fatal("failed selection reused its connection")
				}
			}
		})
	}
}
