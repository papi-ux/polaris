//go:build linux

package main

import (
	"context"
	"errors"
	"fmt"
	"net"
	"path/filepath"
	"sync"
	"syscall"
	"time"

	"github.com/papi-ux/polaris/multiseat_worker/internal/seatmedia"
	"github.com/papi-ux/polaris/multiseat_worker/internal/seatruntime"
)

// One source owns one connection to one generation's encoder. Cancellation
// retires that connection; a late read can never consume a replacement seat.
type encoderMediaSource struct {
	config       workerConfig
	directory    string
	uid          uint32
	readMutex    sync.Mutex
	writeMutex   sync.Mutex
	mutex        sync.Mutex
	connection   *net.UnixConn
	closed       bool
	started      bool
	contract     *mediaConfig
	indices      [2]uint64
	seen         [2]bool
	videoStarted bool
}

func newEncoderMediaSource(config workerConfig, directory string, uid uint32) *encoderMediaSource {
	return &encoderMediaSource{config: config, directory: directory, uid: uid}
}

func (source *encoderMediaSource) Close() error {
	source.mutex.Lock()
	defer source.mutex.Unlock()
	source.closed = true
	if source.connection != nil {
		return source.connection.Close()
	}
	return nil
}

func (source *encoderMediaSource) connectionFor(ctx context.Context) (*net.UnixConn, error) {
	source.mutex.Lock()
	defer source.mutex.Unlock()
	if source.closed || ctx.Err() != nil {
		return nil, errors.New("seat encoder connection retired")
	}
	if source.connection != nil {
		return source.connection, nil
	}
	if err := privateDirectory(source.directory, source.uid); err != nil {
		return nil, err
	}
	name, err := seatruntime.EncodedMediaSocketName(source.config.RuntimeNamespace)
	if err != nil {
		return nil, err
	}
	path := filepath.Join(source.directory, name)
	// Dial through a retained dentry. Replacement-and-restore of the name cannot
	// redirect this connection to a different encoder.
	fd, err := syscall.Open(path, 0x200000|syscall.O_NOFOLLOW|syscall.O_CLOEXEC, 0)
	if err != nil {
		return nil, errors.New("seat encoder endpoint unavailable")
	}
	defer syscall.Close(fd)
	var status syscall.Stat_t
	if syscall.Fstat(fd, &status) != nil || status.Mode&syscall.S_IFMT != syscall.S_IFSOCK || status.Uid != source.uid || status.Mode&0o7777 != 0o600 {
		return nil, errors.New("seat encoder endpoint ownership is invalid")
	}
	dialer := net.Dialer{Timeout: 2 * time.Second}
	connection, err := dialer.DialContext(ctx, "unix", fmt.Sprintf("/proc/self/fd/%d", fd))
	if err != nil {
		return nil, errors.New("seat encoder connection failed")
	}
	unix := connection.(*net.UnixConn)
	uid, err := peerUID(unix)
	if err != nil || uid != source.uid {
		unix.Close()
		return nil, errors.New("seat encoder peer rejected")
	}
	source.connection = unix
	return unix, nil
}

func (source *encoderMediaSource) read(ctx context.Context) (byte, []byte, error) {
	connection, err := source.connectionFor(ctx)
	if err != nil {
		return 0, nil, err
	}
	stop := context.AfterFunc(ctx, func() { _ = source.Close() })
	defer stop()
	// A dead producer cannot hold a seat indefinitely. The independent control
	// writer can request an IDR while this read waits for a frame.
	deadline := time.Now().Add(10 * time.Second)
	if limit, ok := ctx.Deadline(); ok && limit.Before(deadline) {
		deadline = limit
	}
	if err := connection.SetReadDeadline(deadline); err != nil {
		return 0, nil, err
	}
	kind, body, err := seatmedia.Read(connection)
	if err != nil {
		_ = source.Close()
	}
	return kind, body, err
}

func (source *encoderMediaSource) send(ctx context.Context, command byte) error {
	source.writeMutex.Lock()
	defer source.writeMutex.Unlock()
	if ctx.Err() != nil {
		return ctx.Err()
	}
	source.mutex.Lock()
	closed := source.closed
	source.mutex.Unlock()
	if closed {
		return errors.New("seat encoder connection retired")
	}
	if (command == seatmedia.Start && source.started) || (command == seatmedia.RequestIDR && !source.started) {
		return nil
	}
	connection, err := source.connectionFor(ctx)
	if err != nil {
		return err
	}
	stop := context.AfterFunc(ctx, func() { _ = source.Close() })
	defer stop()
	deadline := time.Now().Add(2 * time.Second)
	if limit, ok := ctx.Deadline(); ok && limit.Before(deadline) {
		deadline = limit
	}
	if err = connection.SetWriteDeadline(deadline); err == nil {
		var n int
		n, err = connection.Write([]byte{command})
		if err == nil && n != 1 {
			err = errors.New("encoder control write incomplete")
		}
	}
	if err != nil {
		_ = source.Close()
	} else if command == seatmedia.Start {
		source.started = true
	}
	return err
}

func (source *encoderMediaSource) Contract(ctx context.Context) (mediaConfig, error) {
	source.readMutex.Lock()
	defer source.readMutex.Unlock()
	source.mutex.Lock()
	retired := source.closed
	source.mutex.Unlock()
	if retired || ctx.Err() != nil {
		return mediaConfig{}, errors.New("seat encoder connection retired")
	}
	if source.contract != nil {
		return *source.contract, nil
	}
	kind, body, err := source.read(ctx)
	if err != nil {
		return mediaConfig{}, err
	}
	contract, err := parseMediaConfig(body)
	if kind != seatmedia.Config || err != nil || source.config.DisplayHDR ||
		uint32(contract.Width) != source.config.DisplayWidth || uint32(contract.Height) != source.config.DisplayHeight ||
		uint64(contract.FPSNumerator)*1000 != uint64(source.config.RefreshMillihz)*uint64(contract.FPSDenominator) ||
		contract.AudioChannels != 2 || contract.AudioFrameDurationUS != 5000 {
		_ = source.Close()
		return mediaConfig{}, errors.New("seat encoder contract differs from allocation")
	}
	source.contract = &contract
	return contract, nil
}

func (source *encoderMediaSource) Next(ctx context.Context) (message, mediaFrame, []byte, error) {
	source.readMutex.Lock()
	defer source.readMutex.Unlock()
	failure := func(err error) (message, mediaFrame, []byte, error) {
		_ = source.Close()
		return 0, mediaFrame{}, nil, err
	}
	if source.contract == nil {
		return failure(errors.New("encoder contract was not read"))
	}
	if err := source.send(ctx, seatmedia.Start); err != nil {
		return failure(err)
	}
	kind, body, err := source.read(ctx)
	if err != nil {
		return failure(err)
	}
	frame, data, err := parseMediaFrame(body)
	if err != nil || (kind != seatmedia.Video && kind != seatmedia.Audio) {
		return failure(errors.New("invalid encoded frame"))
	}
	index := int(kind - seatmedia.Video)
	if (source.seen[index] && frame.FrameIndex <= source.indices[index]) ||
		frame.EncodeTimestampNS == 0 || (frame.CaptureTimestampNS != 0 && frame.EncodeTimestampNS < frame.CaptureTimestampNS) ||
		(kind == seatmedia.Audio && frame.IDR) {
		return failure(errors.New("encoded frame sequence or timestamp is invalid"))
	}
	source.indices[index], source.seen[index] = frame.FrameIndex, true
	if kind == seatmedia.Video {
		if !source.videoStarted && !frame.IDR {
			return failure(errors.New("seat stream must begin with an IDR"))
		}
		source.videoStarted = true
		return messageVideo, frame, data, nil
	}
	return messageAudio, frame, data, nil
}

func (source *encoderMediaSource) Keyframe(ctx context.Context) error {
	return source.send(ctx, seatmedia.RequestIDR)
}
func (source *encoderMediaSource) Invalidate(ctx context.Context, span frameRange) error {
	if span.First > span.Last {
		return errors.New("invalid encoder reference range")
	}
	// Rebuild all references with an IDR. This is conservative recovery for an
	// encoder which does not expose selective reference invalidation.
	return source.Keyframe(ctx)
}

var _ mediaSource = (*encoderMediaSource)(nil)
