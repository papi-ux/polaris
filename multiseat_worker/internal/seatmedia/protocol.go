// Package seatmedia frames the private encoder connection. Raw video never
// crosses this boundary. Bodies use the existing worker media wire format.
package seatmedia

import (
	"encoding/binary"
	"errors"
	"io"
)

const (
	Config          byte = 1
	Video           byte = 2
	Audio           byte = 3
	Start           byte = 1
	RequestIDR      byte = 2
	HeaderSize           = 12
	ConfigSize           = 32
	FramePrefixSize      = 32
	MaxPayload           = 16 * 1024 * 1024
	MaxAudioPayload      = FramePrefixSize + 1400
)

func validSize(kind byte, size uint32) bool {
	switch kind {
	case Config:
		return size == ConfigSize
	case Video:
		return size > FramePrefixSize && size <= MaxPayload
	case Audio:
		return size > FramePrefixSize && size <= MaxAudioPayload
	default:
		return false
	}
}

// Read rejects the advertised length before allocating or waiting for a body.
func Read(reader io.Reader) (byte, []byte, error) {
	var header [HeaderSize]byte
	if _, err := io.ReadFull(reader, header[:]); err != nil {
		return 0, nil, err
	}
	size := binary.BigEndian.Uint32(header[8:])
	if string(header[:4]) != "PME1" || header[5] != 0 || header[6] != 0 || header[7] != 0 || !validSize(header[4], size) {
		return 0, nil, errors.New("invalid private encoder packet")
	}
	payload := make([]byte, size)
	_, err := io.ReadFull(reader, payload)
	return header[4], payload, err
}

func Write(writer io.Writer, kind byte, payload []byte) error {
	if len(payload) > MaxPayload || !validSize(kind, uint32(len(payload))) {
		return errors.New("invalid private encoder packet")
	}
	var header [HeaderSize]byte
	copy(header[:4], "PME1")
	header[4] = kind
	binary.BigEndian.PutUint32(header[8:], uint32(len(payload)))
	for _, part := range [][]byte{header[:], payload} {
		for len(part) != 0 {
			n, err := writer.Write(part)
			if err != nil {
				return err
			}
			if n <= 0 || n > len(part) {
				return io.ErrShortWrite
			}
			part = part[n:]
		}
	}
	return nil
}
