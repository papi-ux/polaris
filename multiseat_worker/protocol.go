// Package main implements the isolated Polaris seat-worker entrypoint.
package main

import (
	"crypto/hmac"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
)

const (
	headerSize        = 32
	capabilitySize    = 32
	challengeSize     = 32
	proofSize         = 32
	maxControlPayload = 64 * 1024
	maxMediaPayload   = 16 * 1024 * 1024
)

var (
	protocolMagic   = [4]byte{'P', 'S', 'W', '1'}
	authDomain      = []byte("polaris-seat-worker-ipc-v1\x00")
	errInvalidFrame = errors.New("invalid worker IPC frame")
)

type channel uint8

const (
	channelControl channel = 1
	channelMedia   channel = 2
)

func (selected channel) String() string {
	switch selected {
	case channelControl:
		return "control"
	case channelMedia:
		return "media"
	default:
		return "unknown"
	}
}

type message uint8

const (
	messageChallenge     message = 1
	messageAuthenticate  message = 2
	messageAuthenticated message = 3
	messageHeartbeat     message = 4
	messageHeartbeatAck  message = 5

	messageReady       message = 16
	messageShutdown    message = 17
	messageShutdownAck message = 18
	messageInput       message = 19
	messageFeedback    message = 20
	messageError       message = 21
	messageAttach      message = 22
	messageAttached    message = 23
	messageInputAck    message = 24

	messageVideo         message = 32
	messageAudio         message = 33
	messageEndOfStream   message = 34
	messageDiscontinuity message = 35
)

type proofRole uint8

const (
	proofController proofRole = 1
	proofWorker     proofRole = 2
)

type endpointIdentity struct {
	ControllerEpoch string
	LogicalGPU      string
	Slot            uint32
	Generation      uint64
	WorkerName      string
}

type frame struct {
	Channel    channel
	Message    message
	Slot       uint32
	Generation uint64
	Sequence   uint64
	Payload    []byte
}

func validNameToken(value string, max int) bool {
	if len(value) == 0 || len(value) > max || !asciiAlphaNumeric(value[0]) {
		return false
	}
	for index := 1; index < len(value); index++ {
		character := value[index]
		if !asciiAlphaNumeric(character) && character != '-' && character != '_' && character != '.' {
			return false
		}
	}
	return true
}

func asciiAlphaNumeric(value byte) bool {
	return value >= 'a' && value <= 'z' ||
		value >= 'A' && value <= 'Z' ||
		value >= '0' && value <= '9'
}

func validIdentity(identity endpointIdentity) bool {
	return validNameToken(identity.ControllerEpoch, 64) &&
		validNameToken(identity.LogicalGPU, 128) &&
		identity.Generation != 0 &&
		validNameToken(identity.WorkerName, 128)
}

func parseCapability(encoded string) ([capabilitySize]byte, error) {
	var capability [capabilitySize]byte
	if len(encoded) != capabilitySize*2 {
		return capability, errors.New("capability has the wrong length")
	}
	for _, character := range []byte(encoded) {
		if !(character >= '0' && character <= '9') && !(character >= 'a' && character <= 'f') {
			return capability, errors.New("capability is not canonical lowercase hex")
		}
	}
	decoded, err := hex.DecodeString(encoded)
	if err != nil || len(decoded) != len(capability) {
		return capability, errors.New("capability cannot be decoded")
	}
	copy(capability[:], decoded)
	return capability, nil
}

func appendString(target []byte, value string) []byte {
	var length [2]byte
	binary.BigEndian.PutUint16(length[:], uint16(len(value)))
	target = append(target, length[:]...)
	return append(target, []byte(value)...)
}

func authenticationProof(
	capability [capabilitySize]byte,
	role proofRole,
	selectedChannel channel,
	identity endpointIdentity,
	challenge [challengeSize]byte,
) ([proofSize]byte, error) {
	var proof [proofSize]byte
	if !validIdentity(identity) ||
		(selectedChannel != channelControl && selectedChannel != channelMedia) ||
		(role != proofController && role != proofWorker) {
		return proof, errors.New("authentication context is invalid")
	}

	transcript := make([]byte, 0, 128)
	transcript = append(transcript, authDomain...)
	transcript = append(transcript, byte(role), byte(selectedChannel))
	var numbers [12]byte
	binary.BigEndian.PutUint32(numbers[0:4], identity.Slot)
	binary.BigEndian.PutUint64(numbers[4:12], identity.Generation)
	transcript = append(transcript, numbers[:]...)
	transcript = appendString(transcript, identity.ControllerEpoch)
	transcript = appendString(transcript, identity.LogicalGPU)
	transcript = appendString(transcript, identity.WorkerName)
	transcript = append(transcript, challenge[:]...)

	digest := hmac.New(sha256.New, capability[:])
	_, _ = digest.Write(transcript)
	copy(proof[:], digest.Sum(nil))
	return proof, nil
}

func verifyProof(expected [proofSize]byte, presented []byte) bool {
	return len(presented) == len(expected) && hmac.Equal(expected[:], presented)
}

func payloadLimit(selectedChannel channel) int {
	if selectedChannel == channelControl {
		return maxControlPayload
	}
	return maxMediaPayload
}

func validMessage(selectedChannel channel, selectedMessage message, payloadSize int) bool {
	switch selectedMessage {
	case messageChallenge, messageAuthenticate, messageAuthenticated:
		return payloadSize == proofSize
	case messageHeartbeat, messageHeartbeatAck:
		return payloadSize == 0
	case messageReady:
		return selectedChannel == channelControl && payloadSize > 0 && payloadSize <= 4096
	case messageShutdown, messageShutdownAck:
		return selectedChannel == channelControl && payloadSize == 0
	case messageAttach, messageAttached:
		return payloadSize == 0
	case messageInputAck:
		return selectedChannel == channelControl && payloadSize == 0
	case messageInput, messageFeedback:
		return selectedChannel == channelControl && payloadSize > 0
	case messageError:
		return selectedChannel == channelControl && payloadSize > 0 && payloadSize <= 4096
	case messageVideo, messageAudio:
		return selectedChannel == channelMedia && payloadSize > 0
	case messageEndOfStream, messageDiscontinuity:
		return selectedChannel == channelMedia && payloadSize == 0
	default:
		return false
	}
}

func encodeFrame(value frame) ([]byte, error) {
	if (value.Channel != channelControl && value.Channel != channelMedia) ||
		value.Generation == 0 || value.Sequence == 0 ||
		len(value.Payload) > payloadLimit(value.Channel) ||
		!validMessage(value.Channel, value.Message, len(value.Payload)) {
		return nil, errInvalidFrame
	}
	encoded := make([]byte, headerSize+len(value.Payload))
	copy(encoded[0:4], protocolMagic[:])
	encoded[4] = byte(value.Channel)
	encoded[5] = byte(value.Message)
	// bytes 6-7 are reserved and stay zero.
	binary.BigEndian.PutUint32(encoded[8:12], uint32(len(value.Payload)))
	binary.BigEndian.PutUint32(encoded[12:16], value.Slot)
	binary.BigEndian.PutUint64(encoded[16:24], value.Generation)
	binary.BigEndian.PutUint64(encoded[24:32], value.Sequence)
	copy(encoded[headerSize:], value.Payload)
	return encoded, nil
}

func parseHeader(
	header []byte,
	expectedChannel channel,
	expectedSlot uint32,
	expectedGeneration uint64,
) (frame, int, error) {
	if len(header) != headerSize || expectedGeneration == 0 ||
		(expectedChannel != channelControl && expectedChannel != channelMedia) {
		return frame{}, 0, errInvalidFrame
	}
	if string(header[0:4]) != string(protocolMagic[:]) {
		return frame{}, 0, errInvalidFrame
	}
	selectedChannel := channel(header[4])
	selectedMessage := message(header[5])
	payloadSize := int(binary.BigEndian.Uint32(header[8:12]))
	value := frame{
		Channel:    selectedChannel,
		Message:    selectedMessage,
		Slot:       binary.BigEndian.Uint32(header[12:16]),
		Generation: binary.BigEndian.Uint64(header[16:24]),
		Sequence:   binary.BigEndian.Uint64(header[24:32]),
	}
	if selectedChannel != expectedChannel || value.Slot != expectedSlot ||
		value.Generation != expectedGeneration || value.Sequence == 0 ||
		binary.BigEndian.Uint16(header[6:8]) != 0 ||
		payloadSize > payloadLimit(selectedChannel) ||
		!validMessage(selectedChannel, selectedMessage, payloadSize) {
		return frame{}, 0, errInvalidFrame
	}
	return value, payloadSize, nil
}

func readFrame(
	reader io.Reader,
	expectedChannel channel,
	expectedSlot uint32,
	expectedGeneration uint64,
) (frame, error) {
	header := make([]byte, headerSize)
	if _, err := io.ReadFull(reader, header); err != nil {
		return frame{}, fmt.Errorf("read worker IPC header: %w", err)
	}
	value, payloadSize, err := parseHeader(
		header,
		expectedChannel,
		expectedSlot,
		expectedGeneration,
	)
	if err != nil {
		return frame{}, err
	}
	if payloadSize != 0 {
		value.Payload = make([]byte, payloadSize)
		if _, err := io.ReadFull(reader, value.Payload); err != nil {
			return frame{}, fmt.Errorf("read worker IPC payload: %w", err)
		}
	}
	return value, nil
}

func writeFrame(writer io.Writer, value frame) error {
	encoded, err := encodeFrame(value)
	if err != nil {
		return err
	}
	for len(encoded) != 0 {
		written, err := writer.Write(encoded)
		if err != nil {
			return fmt.Errorf("write worker IPC frame: %w", err)
		}
		if written <= 0 {
			return io.ErrUnexpectedEOF
		}
		encoded = encoded[written:]
	}
	return nil
}

type sequenceGuard struct {
	next      uint64
	exhausted bool
}

func newSequenceGuard() sequenceGuard {
	return sequenceGuard{next: 1}
}

func (guard *sequenceGuard) accept(sequence uint64) bool {
	if guard.exhausted || sequence != guard.next {
		return false
	}
	if guard.next == ^uint64(0) {
		guard.exhausted = true
	} else {
		guard.next++
	}
	return true
}
