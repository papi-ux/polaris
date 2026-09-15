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
	"sync"
	"sync/atomic"
	"syscall"
	"time"
)

const steamInputPacketSize = 24

type steamInputState struct {
	buttons  uint16
	triggers [2]byte
	axes     [4]int16
	hats     [2]int8
}

func decodeSteamInput(packet []byte, expected uint32) (steamInputState, error) {
	var state steamInputState
	if len(packet) != steamInputPacketSize || string(packet[:4]) != "PSI1" ||
		expected == 0 || binary.LittleEndian.Uint32(packet[4:8]) != expected ||
		packet[22] != 0 || packet[23] != 0 {
		return state, errors.New("Steam input packet or sequence is invalid")
	}
	state.buttons = binary.LittleEndian.Uint16(packet[8:10])
	if state.buttons & ^uint16(0x7ff) != 0 {
		return state, errors.New("Steam input buttons are invalid")
	}
	copy(state.triggers[:], packet[10:12])
	for i := range state.axes {
		state.axes[i] = int16(binary.LittleEndian.Uint16(packet[12+i*2:]))
	}
	for i := range state.hats {
		state.hats[i] = int8(packet[20+i])
		if state.hats[i] < -1 || state.hats[i] > 1 {
			return state, errors.New("Steam input hat is invalid")
		}
	}
	return state, nil
}

// The image and host contract are Linux/amd64. Each write is one complete evdev
// report for the already allocated gamepad, never a device creation request.
func steamInputEvents(state steamInputState) []byte {
	result := make([]byte, 0, 20*24)
	event := func(kind, code uint16, value int32) {
		data := make([]byte, 24)
		binary.LittleEndian.PutUint16(data[16:], kind)
		binary.LittleEndian.PutUint16(data[18:], code)
		binary.LittleEndian.PutUint32(data[20:], uint32(value))
		result = append(result, data...)
	}
	for i, code := range []uint16{304, 305, 307, 308, 310, 311, 314, 315, 316, 317, 318} {
		event(1, code, int32((state.buttons>>i)&1))
	}
	for i, code := range []uint16{0, 1, 3, 4} {
		event(3, code, int32(state.axes[i]))
	}
	for i, code := range []uint16{2, 5} {
		event(3, code, int32(state.triggers[i]))
	}
	for i, code := range []uint16{16, 17} {
		event(3, code, int32(state.hats[i]))
	}
	event(0, 0, 0)
	return result
}

type steamInputBroker struct {
	listener *net.UnixListener
	path     string
	identity os.FileInfo
	uid      uint32
	sink     func(steamInputState) error
	cancel   context.CancelFunc
	done     chan struct{}
	mu       sync.Mutex
	failure  error
	active   atomic.Bool
	clients  sync.WaitGroup
}

func startSteamInputBroker(parent context.Context, path string, uid uint32, sink func(steamInputState) error) (*steamInputBroker, error) {
	if parent == nil || sink == nil || !filepath.IsAbs(path) || filepath.Clean(path) != path || len(path) >= 108 {
		return nil, errors.New("Steam input broker configuration is invalid")
	}
	if _, err := os.Lstat(path); !errors.Is(err, os.ErrNotExist) {
		return nil, errors.New("Steam input socket already exists or cannot be inspected")
	}
	listener, err := net.ListenUnix("unixpacket", &net.UnixAddr{Name: path, Net: "unixpacket"})
	if err != nil {
		return nil, err
	}
	listener.SetUnlinkOnClose(false)
	identity, err := os.Lstat(path)
	if err != nil {
		_ = listener.Close()
		return nil, errors.New("Steam input socket identity unavailable")
	}
	broker := &steamInputBroker{listener: listener, path: path, identity: identity, uid: uid, sink: sink, done: make(chan struct{})}
	ctx, cancel := context.WithCancel(parent)
	broker.cancel = cancel
	if err := os.Chmod(path, 0600); err != nil {
		_ = listener.Close()
		broker.removeOwnedSocket()
		cancel()
		return nil, err
	}
	go broker.run(ctx)
	return broker, nil
}

func (b *steamInputBroker) removeOwnedSocket() {
	if current, err := os.Lstat(b.path); err == nil && os.SameFile(current, b.identity) {
		_ = os.Remove(b.path)
	}
}
func (b *steamInputBroker) verify() error {
	if current, err := os.Lstat(b.path); err != nil || !os.SameFile(current, b.identity) || current.Mode()&os.ModeSocket == 0 || current.Mode().Perm() != 0600 {
		return errors.New("Steam input socket was replaced")
	}
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.failure
}
func (b *steamInputBroker) fail(err error) {
	b.mu.Lock()
	if b.failure == nil {
		b.failure = err
	}
	b.mu.Unlock()
	b.cancel()
}
func (b *steamInputBroker) close() error {
	if b == nil {
		return nil
	}
	b.cancel()
	_ = b.listener.Close()
	<-b.done
	b.removeOwnedSocket()
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.failure
}

func (b *steamInputBroker) run(ctx context.Context) {
	defer close(b.done)
	defer b.clients.Wait()
	for ctx.Err() == nil {
		_ = b.listener.SetDeadline(time.Now().Add(200 * time.Millisecond))
		client, err := b.listener.AcceptUnix()
		if err != nil {
			if ctx.Err() != nil || errors.Is(err, net.ErrClosed) {
				return
			}
			if timeout, ok := err.(net.Error); ok && timeout.Timeout() {
				continue
			}
			b.fail(errors.New("Steam input accept failed"))
			return
		}
		if err := b.verify(); err != nil {
			_ = client.Close()
			b.fail(err)
			return
		}
		peerOK := false
		raw, err := client.SyscallConn()
		if err == nil {
			err = raw.Control(func(fd uintptr) {
				cred, err := syscall.GetsockoptUcred(int(fd), syscall.SOL_SOCKET, syscall.SO_PEERCRED)
				peerOK = err == nil && cred.Uid == b.uid && cred.Pid > 0
			})
		}
		// One translated controller is admitted. Excess connections create no
		// goroutine and cannot queue work or displace the existing controller.
		if err != nil || !peerOK || !b.active.CompareAndSwap(false, true) {
			_ = client.Close()
			continue
		}
		b.clients.Add(1)
		go func() {
			defer b.clients.Done()
			defer b.active.Store(false)
			defer client.Close()
			if err := b.consume(ctx, client); err != nil {
				b.fail(err)
			}
		}()
	}
}
func (b *steamInputBroker) consume(ctx context.Context, client *net.UnixConn) (result error) {
	// Neutralize both before admission and on every exit, including malformed
	// packets, abrupt process death, cancellation and a partial device write.
	defer func() { result = errors.Join(result, b.sink(steamInputState{})) }()
	if err := b.sink(steamInputState{}); err != nil {
		return err
	}
	sequence := uint32(1)
	window, packets := time.Now(), 0
	var buffer [steamInputPacketSize + 1]byte
	for ctx.Err() == nil {
		_ = client.SetReadDeadline(time.Now().Add(200 * time.Millisecond))
		n, _, flags, _, err := client.ReadMsgUnix(buffer[:], nil)
		if err != nil {
			if ctx.Err() != nil || errors.Is(err, io.EOF) || errors.Is(err, net.ErrClosed) {
				return nil
			}
			if timeout, ok := err.(net.Error); ok && timeout.Timeout() {
				continue
			}
			return errors.New("Steam input channel failed")
		}
		if n == 0 {
			return nil
		}
		if flags&(syscall.MSG_TRUNC|syscall.MSG_CTRUNC) != 0 {
			return errors.New("Steam input packet was truncated")
		}
		state, err := decodeSteamInput(buffer[:n], sequence)
		if err != nil {
			return err
		}
		now := time.Now()
		if now.Sub(window) >= time.Second {
			window, packets = now, 0
		}
		packets++
		if packets > 2048 {
			return errors.New("Steam input exceeded its bounded event rate")
		}
		if err := b.verify(); err != nil {
			return err
		}
		if err := b.sink(state); err != nil {
			return err
		}
		sequence++
	}
	return nil
}
