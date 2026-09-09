//go:build linux && amd64

package main

import (
	"encoding/binary"
	"testing"
	"time"
)

func TestOnlyExpectedSyntheticMarkersMatch(t *testing.T) {
	for index, tuple := range [][3]int32{{1, 30, 1}, {2, 0, 7}, {3, 0, 120}, {1, 304, 1}} {
		if !marker(index, uint16(tuple[0]), uint16(tuple[1]), tuple[2]) {
			t.Fatal("expected marker missing", index)
		}
		if marker(index, uint16(tuple[0]), uint16(tuple[1]), 0) {
			t.Fatal("neutral state matched", index)
		}
		if marker(index, 0, 0, 0) {
			t.Fatal("SYN event matched", index)
		}
	}
}
func TestProbeTokensCannotEscapePrivateRuntime(t *testing.T) {
	for _, token := range []string{"", "../other", "0123456789abcdef0123456789abcdefff/other"} {
		if _, err := readyPath(token); err == nil {
			t.Fatal("unsafe token accepted")
		}
	}
	if _, err := readyPath("0123456789abcdef0123456789abcdef"); err != nil {
		t.Fatal(err)
	}
}

func TestDelayedObserverCannotSucceedBeforeHostFinish(t *testing.T) {
	start := time.Unix(0, 0)
	window := observationWindow{deadline: start.Add(10 * time.Second)}
	for _, elapsed := range []time.Duration{0, 3 * time.Second, 5 * time.Second} {
		if done, err := window.advance(start.Add(elapsed), false); done || err != nil {
			t.Fatal("observer ended before finish", elapsed, done, err)
		}
	}
	if done, err := window.advance(start.Add(6*time.Second), true); done || err != nil {
		t.Fatal("finish did not leave drain window", done, err)
	}
	if done, err := window.advance(start.Add(6250*time.Millisecond), true); !done || err != nil {
		t.Fatal("completed drain was not accepted", done, err)
	}
}
func TestTimeoutWithoutHostFinishFailsIsolationEvidence(t *testing.T) {
	start := time.Unix(0, 0)
	window := observationWindow{deadline: start.Add(10 * time.Second)}
	if done, err := window.advance(start.Add(10*time.Second), false); done || err == nil {
		t.Fatal("timeout became successful isolation evidence")
	}
}
func TestDroppedKernelEventsInvalidateObservation(t *testing.T) {
	event := make([]byte, 24)
	binary.LittleEndian.PutUint16(event[18:20], 3)
	var result observation
	if err := recordEvent(&result, 0, event); err == nil {
		t.Fatal("SYN_DROPPED was ignored")
	}
}
