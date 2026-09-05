//go:build linux

package seatprovider

import (
	"context"
	"encoding/binary"
	"errors"
	"io"
	"net"
	"syscall"
	"time"
)

const (
	maximumWaylandMessageSize = 64 * 1024
	maximumWaylandMessages    = 512
	waylandOutputCurrent      = 0x1
)

type waylandMessage struct {
	objectID uint32
	opcode   uint16
	payload  []byte
}

type waylandGlobal struct {
	name    uint32
	version uint32
}

type waylandProbeExpectation struct {
	width         uint32
	height        uint32
	refresh       uint32
	requireDMABuf bool
	peerPID       int
	peerUID       uint32
}

func appendWaylandUint(buffer []byte, value uint32) []byte {
	var encoded [4]byte
	binary.NativeEndian.PutUint32(encoded[:], value)
	return append(buffer, encoded[:]...)
}

func appendWaylandString(buffer []byte, value string) ([]byte, error) {
	if value == "" || len(value) > 256 {
		return nil, errors.New("runtime Wayland string is invalid")
	}
	length := len(value) + 1
	buffer = appendWaylandUint(buffer, uint32(length))
	buffer = append(buffer, value...)
	buffer = append(buffer, 0)
	for len(buffer)%4 != 0 {
		buffer = append(buffer, 0)
	}
	return buffer, nil
}

func encodeWaylandMessage(objectID uint32, opcode uint16, payload []byte) ([]byte, error) {
	size := len(payload) + 8
	if objectID == 0 || size < 8 || size > maximumWaylandMessageSize || size%4 != 0 {
		return nil, errors.New("runtime Wayland message is invalid")
	}
	message := make([]byte, 8, size)
	binary.NativeEndian.PutUint32(message[0:4], objectID)
	binary.NativeEndian.PutUint32(message[4:8], uint32(size)<<16|uint32(opcode))
	return append(message, payload...), nil
}

func writeWaylandMessage(
	connection net.Conn,
	objectID uint32,
	opcode uint16,
	payload []byte,
) error {
	message, err := encodeWaylandMessage(objectID, opcode, payload)
	if err != nil {
		return err
	}
	for len(message) > 0 {
		written, err := connection.Write(message)
		if err != nil || written <= 0 || written > len(message) {
			return errors.New("runtime Wayland request failed")
		}
		message = message[written:]
	}
	return nil
}

func readWaylandMessage(connection net.Conn) (waylandMessage, error) {
	var header [8]byte
	if _, err := io.ReadFull(connection, header[:]); err != nil {
		return waylandMessage{}, errors.New("runtime Wayland response failed")
	}
	objectID := binary.NativeEndian.Uint32(header[0:4])
	word := binary.NativeEndian.Uint32(header[4:8])
	size := int(word >> 16)
	if objectID == 0 || size < 8 || size > maximumWaylandMessageSize || size%4 != 0 {
		return waylandMessage{}, errors.New("runtime Wayland response is invalid")
	}
	payload := make([]byte, size-8)
	if _, err := io.ReadFull(connection, payload); err != nil {
		return waylandMessage{}, errors.New("runtime Wayland response failed")
	}
	return waylandMessage{
		objectID: objectID,
		opcode:   uint16(word & 0xffff),
		payload:  payload,
	}, nil
}

func parseWaylandString(payload []byte, offset int) (string, int, error) {
	if offset < 0 || offset+4 > len(payload) {
		return "", 0, errors.New("runtime Wayland string is invalid")
	}
	length := int(binary.NativeEndian.Uint32(payload[offset : offset+4]))
	offset += 4
	if length < 2 || length > 257 || offset+length > len(payload) ||
		payload[offset+length-1] != 0 {
		return "", 0, errors.New("runtime Wayland string is invalid")
	}
	for _, character := range payload[offset : offset+length-1] {
		if character < 0x20 || character > 0x7e {
			return "", 0, errors.New("runtime Wayland string is invalid")
		}
	}
	next := offset + length
	for next%4 != 0 {
		if next >= len(payload) || payload[next] != 0 {
			return "", 0, errors.New("runtime Wayland string is invalid")
		}
		next++
	}
	return string(payload[offset : offset+length-1]), next, nil
}

func parseWaylandGlobal(payload []byte) (string, waylandGlobal, error) {
	if len(payload) < 12 {
		return "", waylandGlobal{}, errors.New("runtime Wayland global is invalid")
	}
	global := waylandGlobal{name: binary.NativeEndian.Uint32(payload[0:4])}
	name, next, err := parseWaylandString(payload, 4)
	if err != nil || next+4 != len(payload) {
		return "", waylandGlobal{}, errors.New("runtime Wayland global is invalid")
	}
	global.version = binary.NativeEndian.Uint32(payload[next : next+4])
	if global.name == 0 || global.version == 0 {
		return "", waylandGlobal{}, errors.New("runtime Wayland global is invalid")
	}
	return name, global, nil
}

func waylandRoundTrip(
	connection net.Conn,
	callbackID uint32,
	handle func(waylandMessage) error,
) error {
	for count := 0; count < maximumWaylandMessages; count++ {
		message, err := readWaylandMessage(connection)
		if err != nil {
			return err
		}
		if message.objectID == 1 && message.opcode == 0 {
			return errors.New("runtime Wayland server reported a protocol error")
		}
		if message.objectID == callbackID && message.opcode == 0 {
			if len(message.payload) != 4 {
				return errors.New("runtime Wayland callback is invalid")
			}
			return nil
		}
		if handle != nil {
			if err := handle(message); err != nil {
				return err
			}
		}
	}
	return errors.New("runtime Wayland response exceeded its bound")
}

func verifyWaylandPeer(connection *net.UnixConn, expectedPID int, expectedUID uint32) error {
	if connection == nil || expectedPID <= 0 {
		return errors.New("runtime Wayland peer expectation is invalid")
	}
	raw, err := connection.SyscallConn()
	if err != nil {
		return errors.New("runtime Wayland peer could not be inspected")
	}
	var credential *syscall.Ucred
	var controlError error
	if err := raw.Control(func(descriptor uintptr) {
		credential, controlError = syscall.GetsockoptUcred(
			int(descriptor),
			syscall.SOL_SOCKET,
			syscall.SO_PEERCRED,
		)
	}); err != nil || controlError != nil || credential == nil ||
		int(credential.Pid) != expectedPID || credential.Uid != expectedUID {
		return errors.New("runtime Wayland peer identity is invalid")
	}
	return nil
}

func probeWaylandDisplay(
	parent context.Context,
	socketPath string,
	expectation waylandProbeExpectation,
	timeout time.Duration,
) error {
	if parent == nil || !validUnixSocketPath(socketPath) || timeout <= 0 ||
		expectation.width == 0 || expectation.height == 0 ||
		expectation.refresh == 0 {
		return errors.New("runtime Wayland probe is invalid")
	}
	dialer := net.Dialer{Timeout: timeout}
	connectionValue, err := dialer.DialContext(parent, "unix", socketPath)
	if err != nil {
		return errors.New("runtime Wayland display is unavailable")
	}
	connection, ok := connectionValue.(*net.UnixConn)
	if !ok {
		_ = connectionValue.Close()
		return errors.New("runtime Wayland display is invalid")
	}
	defer connection.Close()
	if err := connection.SetDeadline(time.Now().Add(timeout)); err != nil {
		return errors.New("runtime Wayland probe deadline failed")
	}
	if err := verifyWaylandPeer(
		connection,
		expectation.peerPID,
		expectation.peerUID,
	); err != nil {
		return err
	}

	registryID := uint32(2)
	firstCallbackID := uint32(3)
	if err := writeWaylandMessage(
		connection,
		1,
		1,
		appendWaylandUint(nil, registryID),
	); err != nil {
		return err
	}
	if err := writeWaylandMessage(
		connection,
		1,
		0,
		appendWaylandUint(nil, firstCallbackID),
	); err != nil {
		return err
	}
	globals := make(map[string][]waylandGlobal)
	if err := waylandRoundTrip(connection, firstCallbackID, func(message waylandMessage) error {
		if message.objectID != registryID || message.opcode != 0 {
			return nil
		}
		name, global, err := parseWaylandGlobal(message.payload)
		if err != nil {
			return err
		}
		globals[name] = append(globals[name], global)
		return nil
	}); err != nil {
		return err
	}
	for _, required := range []string{"wl_compositor", "wl_shm", "wl_seat", "xdg_wm_base"} {
		if len(globals[required]) == 0 {
			return errors.New("runtime Wayland display is missing a required global")
		}
	}
	outputs := globals["wl_output"]
	if len(outputs) != 1 || outputs[0].version < 2 {
		return errors.New("runtime Wayland output set is invalid")
	}
	if expectation.requireDMABuf &&
		(len(globals["zwp_linux_dmabuf_v1"]) == 0 ||
			globals["zwp_linux_dmabuf_v1"][0].version < 3) {
		return errors.New("runtime Wayland display is missing DMA-BUF support")
	}

	outputID := uint32(4)
	bindPayload := appendWaylandUint(nil, outputs[0].name)
	bindPayload, err = appendWaylandString(bindPayload, "wl_output")
	if err != nil {
		return err
	}
	bindPayload = appendWaylandUint(bindPayload, 2)
	bindPayload = appendWaylandUint(bindPayload, outputID)
	if err := writeWaylandMessage(connection, registryID, 0, bindPayload); err != nil {
		return err
	}
	secondCallbackID := uint32(5)
	if err := writeWaylandMessage(
		connection,
		1,
		0,
		appendWaylandUint(nil, secondCallbackID),
	); err != nil {
		return err
	}
	modeSeen := false
	doneSeen := false
	if err := waylandRoundTrip(connection, secondCallbackID, func(message waylandMessage) error {
		if message.objectID != outputID {
			return nil
		}
		switch message.opcode {
		case 1:
			if len(message.payload) != 16 {
				return errors.New("runtime Wayland output mode is invalid")
			}
			flags := binary.NativeEndian.Uint32(message.payload[0:4])
			if flags&waylandOutputCurrent == 0 {
				return nil
			}
			width := binary.NativeEndian.Uint32(message.payload[4:8])
			height := binary.NativeEndian.Uint32(message.payload[8:12])
			refresh := binary.NativeEndian.Uint32(message.payload[12:16])
			if width != expectation.width || height != expectation.height ||
				refresh != expectation.refresh {
				return errors.New("runtime Wayland output mode does not match the allocation")
			}
			modeSeen = true
		case 2:
			if len(message.payload) != 0 {
				return errors.New("runtime Wayland output completion is invalid")
			}
			doneSeen = true
		}
		return nil
	}); err != nil {
		return err
	}
	if !modeSeen || !doneSeen {
		return errors.New("runtime Wayland output did not become ready")
	}
	return nil
}
