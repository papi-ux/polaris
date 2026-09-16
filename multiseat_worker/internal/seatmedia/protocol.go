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
	BitrateSelected byte = 4
	Start           byte = 1
	RequestIDR      byte = 2
	SelectBitrate   byte = 3
	HeaderSize           = 12
	ConfigSize           = 32
	FramePrefixSize      = 32
	MaxPayload           = 16 * 1024 * 1024
	MaxAudioPayload      = FramePrefixSize + 1400
)

func validSize(kind byte, size uint32) bool {
	switch kind {
	case BitrateSelected:
		return size == 4
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

func readHeader(reader io.Reader, header *[HeaderSize]byte) (byte, uint32, error) {
	if _, err := io.ReadFull(reader, header[:]); err != nil {
		return 0, 0, err
	}
	size := binary.BigEndian.Uint32(header[8:])
	if string(header[:4]) != "PME1" || header[5] != 0 || header[6] != 0 || header[7] != 0 || !validSize(header[4], size) {
		return 0, 0, errors.New("invalid private encoder packet")
	}
	return header[4], size, nil
}

// Read rejects the advertised length before allocating or waiting for a body.
// Its payload remains owned by the caller across subsequent reads.
func Read(reader io.Reader) (byte, []byte, error) {
	var header [HeaderSize]byte
	kind, size, err := readHeader(reader, &header)
	if err != nil {
		return 0, nil, err
	}
	payload := make([]byte, size)
	_, err = io.ReadFull(reader, payload)
	return kind, payload, err
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
