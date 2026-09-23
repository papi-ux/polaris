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
	"testing"
	"time"
)

func steamTestPacket(sequence uint32) []byte {
	p := make([]byte, steamInputPacketSize)
	copy(p, "PSI1")
	binary.LittleEndian.PutUint32(p[4:], sequence)
	binary.LittleEndian.PutUint16(p[8:], 1)
	p[10] = 255
	binary.LittleEndian.PutUint16(p[12:], 24000)
	p[20] = 255 // left D-pad
	return p
}
func TestSteamInputRejectsMalformedAndReplayedReports(t *testing.T) {
	valid := steamTestPacket(1)
	for _, corrupt := range []func([]byte) []byte{
		func(p []byte) []byte { return p[:23] },
		func(p []byte) []byte { return append(p, 0) },
		func(p []byte) []byte { p[0] = 'X'; return p },
		func(p []byte) []byte { p[4] = 2; return p },
		func(p []byte) []byte { p[9] = 128; return p },
		func(p []byte) []byte { p[20] = 2; return p },
		func(p []byte) []byte { p[21] = 128; return p },
		func(p []byte) []byte { p[23] = 1; return p },
	} {
		if _, err := decodeSteamInput(corrupt(append([]byte(nil), valid...)), 1); err == nil {
			t.Fatal("invalid packet accepted")
		}
	}
	state, err := decodeSteamInput(valid, 1)
	if err != nil || state.buttons != 1 || state.axes[0] != 24000 || state.hats[0] != -1 || state.triggers[0] != 255 {
		t.Fatal(state, err)
	}
	if _, err := decodeSteamInput(valid, 0); err == nil {
		t.Fatal("accepted exhausted sequence")
	}
	events := steamInputEvents(state)
	if len(events) != 20*24 || binary.LittleEndian.Uint16(events[18:]) != 304 ||
		binary.LittleEndian.Uint32(events[20:]) != 1 || binary.LittleEndian.Uint32(events[len(events)-4:]) != 0 {
		t.Fatal("invalid evdev report")
	}
}

type steamTestSink struct {
	mu     sync.Mutex
	states []steamInputState
}

func (s *steamTestSink) put(v steamInputState) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.states = append(s.states, v)
	return nil
}
func (s *steamTestSink) snapshot() []steamInputState {
	s.mu.Lock()
	defer s.mu.Unlock()
	return append([]steamInputState(nil), s.states...)
}
func steamWait(t *testing.T, predicate func() bool) {
	t.Helper()
	until := time.Now().Add(3 * time.Second)
	for !predicate() {
		if time.Now().After(until) {
			t.Fatal("Steam input broker did not reach expected state")
		}
		time.Sleep(time.Millisecond)
	}
}
func steamConnect(t *testing.T, path string) *net.UnixConn {
	t.Helper()
	c, err := net.DialUnix("unixpacket", nil, &net.UnixAddr{Name: path, Net: "unixpacket"})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = c.Close() })
	return c
}

// steamSocketDir is a directory for a broker socket, short enough for the broker to accept.
//
// An AF_UNIX address holds 108 bytes, and t.TempDir() names its directory after the test, so the
// long-named tests here hand the broker a path it rejects as invalid configuration. That reads as a
// broken broker rather than as a path the test built too long, and it is how this suite came to be
// treated as a known local failure. The directory carries no test name and goes with the test.
func steamSocketDir(t *testing.T) string {
	t.Helper()
	dir, err := os.MkdirTemp("", "polaris-steam-")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		os.RemoveAll(dir)
	})
	return dir
}

func TestSteamInputIndependentSeatsNeutralizeOnDisconnectAndCancel(t *testing.T) {
	var a, b steamTestSink
	dir := steamSocketDir(t)
	first, err := startSteamInputBroker(context.Background(), filepath.Join(dir, "a"), uint32(os.Getuid()), a.put)
	if err != nil {
		t.Fatal(err)
	}
	defer first.close()
	second, err := startSteamInputBroker(context.Background(), filepath.Join(dir, "b"), uint32(os.Getuid()), b.put)
	if err != nil {
		t.Fatal(err)
	}
	defer second.close()
	ca, cb := steamConnect(t, first.path), steamConnect(t, second.path)
	if _, err := ca.Write(steamTestPacket(1)); err != nil {
		t.Fatal(err)
	}
	if _, err := cb.Write(steamTestPacket(1)); err != nil {
		t.Fatal(err)
	}
	steamWait(t, func() bool { return len(a.snapshot()) == 2 && len(b.snapshot()) == 2 })
	_ = ca.Close()
	steamWait(t, func() bool { return len(a.snapshot()) == 3 && !first.active.Load() })
	if a.snapshot()[2] != (steamInputState{}) || len(b.snapshot()) != 2 {
		t.Fatal("disconnect crossed seat boundary or retained input")
	}
	if _, err := cb.Write(steamTestPacket(2)); err != nil {
		t.Fatal(err)
	}
	steamWait(t, func() bool { return len(b.snapshot()) == 3 })
	if err := first.close(); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Lstat(first.path); !os.IsNotExist(err) {
		t.Fatal("owned socket survived")
	}
	if err := second.close(); err != nil {
		t.Fatal(err)
	}
	if states := b.snapshot(); states[len(states)-1] != (steamInputState{}) {
		t.Fatal("cancel did not release input")
	}
}
func TestSteamInputBadPeerCannotLeaveHeldControls(t *testing.T) {
	for _, oversized := range []bool{false, true} {
		var sink steamTestSink
		broker, err := startSteamInputBroker(context.Background(), filepath.Join(steamSocketDir(t), "s"), uint32(os.Getuid()), sink.put)
		if err != nil {
			t.Fatal(err)
		}
		c := steamConnect(t, broker.path)
		if _, err := c.Write(steamTestPacket(1)); err != nil {
			t.Fatal(err)
		}
		steamWait(t, func() bool { return len(sink.snapshot()) == 2 })
		bad := steamTestPacket(1) // replay
		if oversized {
			bad = append(bad, make([]byte, 100)...)
		}
		if _, err := c.Write(bad); err != nil {
			t.Fatal(err)
		}
		steamWait(t, func() bool { return len(sink.snapshot()) == 3 })
		if err := broker.close(); err == nil {
			t.Fatal("bad peer did not fail closed")
		}
		if sink.snapshot()[2] != (steamInputState{}) {
			t.Fatal("bad peer left held input")
		}
	}
}
func TestSteamInputDoesNotRemoveReplacementSocket(t *testing.T) {
	path := filepath.Join(steamSocketDir(t), "s")
	broker, err := startSteamInputBroker(context.Background(), path, uint32(os.Getuid()), func(steamInputState) error { return nil })
	if err != nil {
		t.Fatal(err)
	}
	if err := os.Remove(path); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, []byte("replacement"), 0600); err != nil {
		t.Fatal(err)
	}
	if err := broker.verify(); err == nil {
		t.Fatal("replacement accepted")
	}
	_ = broker.close()
	if data, err := os.ReadFile(path); err != nil || string(data) != "replacement" {
		t.Fatal("removed unowned replacement")
	}
}

func TestSteamInputRejectsWrongUIDAndCompetingController(t *testing.T) {
	for _, wrongUID := range []bool{true, false} {
		t.Run(map[bool]string{true: "uid", false: "competing"}[wrongUID], func(t *testing.T) {
			var sink steamTestSink
			uid := uint32(os.Getuid())
			if wrongUID {
				uid++
			}
			broker, err := startSteamInputBroker(context.Background(), filepath.Join(steamSocketDir(t), "s"), uid, sink.put)
			if err != nil {
				t.Fatal(err)
			}
			defer broker.close()
			var admitted *net.UnixConn
			if !wrongUID {
				admitted = steamConnect(t, broker.path)
				if _, err := admitted.Write(steamTestPacket(1)); err != nil {
					t.Fatal(err)
				}
				steamWait(t, func() bool { return len(sink.snapshot()) == 2 })
			}
			rejected := steamConnect(t, broker.path)
			_ = rejected.SetReadDeadline(time.Now().Add(time.Second))
			var data [1]byte
			_, err = rejected.Read(data[:])
			if !errors.Is(err, io.EOF) {
				t.Fatalf("unauthorized controller was not closed: %v", err)
			}
			if wrongUID {
				if len(sink.snapshot()) != 0 {
					t.Fatal("wrong UID changed output state")
				}
			} else {
				if _, err := admitted.Write(steamTestPacket(2)); err != nil {
					t.Fatal(err)
				}
				steamWait(t, func() bool { return len(sink.snapshot()) == 3 })
				if sink.snapshot()[2].buttons != 1 {
					t.Fatal("competing controller displaced admitted controller")
				}
			}
		})
	}
}

func TestSteamInputOutputFailureRetiresBrokerAndAttemptsNeutral(t *testing.T) {
	var sink steamTestSink
	failure := errors.New("device retired")
	broker, err := startSteamInputBroker(context.Background(), filepath.Join(steamSocketDir(t), "s"), uint32(os.Getuid()),
		func(state steamInputState) error {
			_ = sink.put(state)
			if state.buttons != 0 {
				return failure
			}
			return nil
		})
	if err != nil {
		t.Fatal(err)
	}
	defer broker.close()
	client := steamConnect(t, broker.path)
	if _, err := client.Write(steamTestPacket(1)); err != nil {
		t.Fatal(err)
	}
	select {
	case <-broker.done:
	case <-time.After(3 * time.Second):
		t.Fatal("failed output left broker running")
	}
	if !errors.Is(broker.close(), failure) {
		t.Fatal("output failure was not retained")
	}
	states := sink.snapshot()
	if len(states) != 3 || states[len(states)-1] != (steamInputState{}) {
		t.Fatal("output failure did not attempt to release held input")
	}
}
