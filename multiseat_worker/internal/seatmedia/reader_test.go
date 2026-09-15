package seatmedia

import (
	"bytes"
	"encoding/binary"
	"errors"
	"io"
	"testing"
	"testing/iotest"
)

func TestReaderRelaysMixedPacketsWithoutKeepingBorrowedPayloads(t *testing.T) {
	var input bytes.Buffer
	for index := range 40 {
		kind, size := Video, FramePrefixSize+32*1024
		if index == 0 {
			kind, size = Config, ConfigSize
		} else if index%3 != 0 {
			kind, size = Audio, FramePrefixSize+80
		}
		if err := Write(&input, kind, bytes.Repeat([]byte{byte(index)}, size)); err != nil {
			t.Fatal(err)
		}
	}
	want := append([]byte(nil), input.Bytes()...)
	reader := NewReader(iotest.OneByteReader(&input))
	var output bytes.Buffer
	for range 40 {
		kind, payload, err := reader.Read()
		if err != nil {
			t.Fatal(err)
		}
		// This is the relay's ownership rule: finish the synchronous write
		// before reading another packet into the borrowed storage.
		if err := Write(&output, kind, payload); err != nil {
			t.Fatal(err)
		}
	}
	if !bytes.Equal(output.Bytes(), want) {
		t.Fatal("relay changed a packet or leaked bytes from a larger packet")
	}
	if kind, payload, err := reader.Read(); kind != 0 || payload != nil || !errors.Is(err, io.EOF) {
		t.Fatal("EOF exposed a previous packet", kind, err)
	}
}

func TestReadersKeepSeparateSeatStorage(t *testing.T) {
	var first, second bytes.Buffer
	for _, value := range []byte{0x31, 0x32} {
		if err := Write(&first, Video, bytes.Repeat([]byte{value}, 80)); err != nil {
			t.Fatal(err)
		}
	}
	if err := Write(&second, Video, bytes.Repeat([]byte{0x42}, 80)); err != nil {
		t.Fatal(err)
	}
	left, right := NewReader(&first), NewReader(&second)
	_, oldLeft, err := left.Read()
	if err != nil {
		t.Fatal(err)
	}
	_, heldRight, err := right.Read()
	if err != nil {
		t.Fatal(err)
	}
	_, nextLeft, err := left.Read()
	if err != nil || !bytes.Equal(nextLeft, bytes.Repeat([]byte{0x32}, 80)) {
		t.Fatal("next packet was not read", err)
	}
	if &oldLeft[0] != &nextLeft[0] {
		t.Fatal("same-sized packet allocated new storage")
	}
	if !bytes.Equal(heldRight, bytes.Repeat([]byte{0x42}, 80)) {
		t.Fatal("reading one seat changed another seat's packet")
	}
}

func TestReaderRejectsMalformedHeadersBeforeReadingOrGrowingStorage(t *testing.T) {
	for _, entry := range []struct {
		name   string
		kind   byte
		size   uint32
		mutate func([]byte)
	}{
		{"config size", Config, ConfigSize + 1, nil},
		{"empty video", Video, FramePrefixSize, nil},
		{"oversized video", Video, MaxPayload + 1, nil},
		{"oversized audio", Audio, MaxAudioPayload + 1, nil},
		{"unknown kind", 99, 80, nil},
		{"magic", Video, 80, func(h []byte) { h[0] ^= 1 }},
		{"reserved 5", Video, 80, func(h []byte) { h[5] = 1 }},
		{"reserved 6", Video, 80, func(h []byte) { h[6] = 1 }},
		{"reserved 7", Video, 80, func(h []byte) { h[7] = 1 }},
	} {
		t.Run(entry.name, func(t *testing.T) {
			var input bytes.Buffer
			if err := Write(&input, Config, make([]byte, ConfigSize)); err != nil {
				t.Fatal(err)
			}
			header := make([]byte, HeaderSize)
			copy(header, "PME1")
			header[4] = entry.kind
			binary.BigEndian.PutUint32(header[8:], entry.size)
			if entry.mutate != nil {
				entry.mutate(header)
			}
			input.Write(header)
			input.WriteString("body must remain unread")
			reader := NewReader(&input)
			if _, _, err := reader.Read(); err != nil {
				t.Fatal(err)
			}
			capacity := cap(reader.payload)
			if kind, payload, err := reader.Read(); kind != 0 || payload != nil || err == nil {
				t.Fatal("malformed packet exposed reused storage", kind, err)
			}
			if input.String() != "body must remain unread" || cap(reader.payload) != capacity {
				t.Fatal("rejected packet consumed its body or grew storage")
			}
		})
	}
}

func TestReaderDoesNotExposeTruncatedPackets(t *testing.T) {
	var wire bytes.Buffer
	if err := Write(&wire, Video, bytes.Repeat([]byte{0x42}, 80)); err != nil {
		t.Fatal(err)
	}
	for _, length := range []int{0, 1, HeaderSize - 1, HeaderSize, HeaderSize + 1, wire.Len() - 1} {
		var input bytes.Buffer
		input.Write(wire.Bytes())
		input.Write(wire.Bytes()[:length])
		reader := NewReader(&input)
		if _, _, err := reader.Read(); err != nil {
			t.Fatal(err)
		}
		if kind, payload, err := reader.Read(); kind != 0 || payload != nil || err == nil {
			t.Fatalf("truncated packet exposed storage at length %d: kind %d, error %v", length, kind, err)
		}
	}
}

func TestReadRetainsCallerOwnedPackets(t *testing.T) {
	var input bytes.Buffer
	for _, value := range []byte{0x31, 0x32} {
		if err := Write(&input, Video, bytes.Repeat([]byte{value}, 80)); err != nil {
			t.Fatal(err)
		}
	}
	_, first, err := Read(&input)
	if err != nil {
		t.Fatal(err)
	}
	if _, _, err := Read(&input); err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(first, bytes.Repeat([]byte{0x31}, 80)) {
		t.Fatal("owned packet changed across reads")
	}
}

func BenchmarkRead(b *testing.B)   { benchmarkRead(b, false) }
func BenchmarkReader(b *testing.B) { benchmarkRead(b, true) }

func benchmarkRead(b *testing.B, reuse bool) {
	for _, entry := range []struct {
		name string
		kind byte
		size int
	}{
		{"audio", Audio, FramePrefixSize + 80},
		{"video", Video, FramePrefixSize + 16*1024},
		{"keyframe", Video, FramePrefixSize + 256*1024},
	} {
		b.Run(entry.name, func(b *testing.B) {
			var wire bytes.Buffer
			if err := Write(&wire, entry.kind, make([]byte, entry.size)); err != nil {
				b.Fatal(err)
			}
			encoded := wire.Bytes()
			reader := bytes.NewReader(encoded)
			read := func() (byte, []byte, error) { return Read(reader) }
			if reuse {
				read = NewReader(reader).Read
			}
			// Measure the steady relay after its first packet has allocated
			// storage; growing the bounded buffer is intentionally not free.
			if _, _, err := read(); err != nil {
				b.Fatal(err)
			}
			b.ReportAllocs()
			b.SetBytes(int64(len(encoded)))
			b.ResetTimer()
			for b.Loop() {
				reader.Reset(encoded)
				kind, payload, err := read()
				if err != nil || kind != entry.kind || len(payload) != entry.size {
					b.Fatal("packet read failed", err)
				}
			}
		})
	}
}
