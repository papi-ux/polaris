package seatmedia

import "io"

// Reader reuses one seat's packet storage for a synchronous media relay. It
// must not be shared between goroutines or seats. Each payload is borrowed
// until the next Read call; a caller that retains or queues it must copy it.
type Reader struct {
	source  io.Reader
	header  [HeaderSize]byte
	payload []byte
}

func NewReader(source io.Reader) *Reader {
	return &Reader{source: source}
}

// Read validates the header before growing storage or consuming a body. The
// retained buffer is bounded by MaxPayload and lasts only as long as Reader.
// An incomplete packet never exposes partially overwritten storage.
func (reader *Reader) Read() (byte, []byte, error) {
	kind, size, err := readHeader(reader.source, &reader.header)
	if err != nil {
		return 0, nil, err
	}
	if cap(reader.payload) < int(size) {
		reader.payload = make([]byte, size)
	}
	reader.payload = reader.payload[:size]
	if _, err := io.ReadFull(reader.source, reader.payload); err != nil {
		return 0, nil, err
	}
	return kind, reader.payload, nil
}
