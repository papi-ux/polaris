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
	// mediaConfigSize is the fixed big-endian body of a media_config message.
	mediaConfigSize = 32
	// mediaFramePrefixSize is the fixed prefix inside every video/audio payload.
	mediaFramePrefixSize = 32
	// frameRangeSize is the fixed body of an invalidate_ref_frames message.
	frameRangeSize       = 16
	mediaContractVersion = 1
	mediaFrameFlagIDR    = 0x01
	mediaFrameKnownFlags = mediaFrameFlagIDR
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
	// messageMediaConfig travels worker to controller on the media channel as
	// the first frame there, ahead of every frame it describes.
	messageMediaConfig message = 25
	// messageMediaConfigAck travels controller to worker; no media may be
	// sent before it arrives.
	messageMediaConfigAck message = 26
	// messageRequestIDR asks for the next produced frame to be an IDR.
	messageRequestIDR message = 27
	// messageInvalidateReferenceFrames carries an inclusive frameRange body.
	messageInvalidateReferenceFrames message = 28
	// messageMediaControlAck is the one acknowledgement the worker returns for
	// the three controller to worker contract messages above.
	messageMediaControlAck message = 29

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
	case messageMediaConfig:
		// The contract rides the channel it describes, ahead of the frames it
		// describes, so it can never be read out of order with them.
		return selectedChannel == channelMedia && payloadSize == mediaConfigSize
	case messageMediaConfigAck, messageRequestIDR, messageMediaControlAck:
		return selectedChannel == channelControl && payloadSize == 0
	case messageInvalidateReferenceFrames:
		return selectedChannel == channelControl && payloadSize == frameRangeSize
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

type videoCodec uint8

const videoCodecH264 videoCodec = 1

type audioCodec uint8

const audioCodecOpus audioCodec = 1

// mediaConfig is the media contract one worker produces for the lifetime of
// its data plane. It mirrors media_config_t in src/multiseat_worker_protocol.h
// byte for byte; only H.264 video and 48 kHz Opus audio are representable.
type mediaConfig struct {
	VideoCodec           videoCodec
	ProfileIDC           uint8
	LevelIDC             uint8
	Width                uint16
	Height               uint16
	FPSNumerator         uint32
	FPSDenominator       uint32
	BitrateCeilingKbps   uint32
	AudioCodec           audioCodec
	AudioChannels        uint8
	AudioFrameDurationUS uint16
	AudioSampleRate      uint32
}

// mediaFrame is the prefix carried inside every video and audio payload.
type mediaFrame struct {
	FrameIndex uint64
	IDR        bool
	// CLOCK_MONOTONIC nanoseconds on the shared kernel; zero means unknown.
	CaptureTimestampNS uint64
	EncodeTimestampNS  uint64
}

// frameRange is an inclusive frame index range whose references must not be
// used again.
type frameRange struct {
	First uint64
	Last  uint64
}

func legalOpusFrameDuration(microseconds uint16) bool {
	switch microseconds {
	case 2500, 5000, 10000, 20000, 40000, 60000:
		return true
	default:
		return false
	}
}

func legalH264Profile(profileIDC uint8) bool {
	switch profileIDC {
	case 66, 77, 88, 100:
		return true
	default:
		return false
	}
}

// validMediaConfig mirrors valid_media_config: known codecs, even geometry
// between 16 and 16384 pixels, a positive frame rate of at most 1000 Hz, a
// bitrate ceiling of at most 1 Gbit/s, 48 kHz Opus with one to eight channels
// and a legal Opus frame duration.
func validMediaConfig(config mediaConfig) bool {
	const minimumDimension, maximumDimension = 16, 16384
	const maximumBitrateKbps = 1000000
	const maximumFPS = 1000
	if config.VideoCodec != videoCodecH264 || !legalH264Profile(config.ProfileIDC) ||
		config.LevelIDC < 10 || config.LevelIDC > 62 {
		return false
	}
	if config.Width < minimumDimension || config.Width > maximumDimension ||
		config.Height < minimumDimension || config.Height > maximumDimension ||
		config.Width%2 != 0 || config.Height%2 != 0 {
		return false
	}
	if config.FPSNumerator == 0 || config.FPSDenominator == 0 ||
		config.FPSNumerator < config.FPSDenominator ||
		uint64(config.FPSNumerator) > maximumFPS*uint64(config.FPSDenominator) {
		return false
	}
	if config.BitrateCeilingKbps == 0 || config.BitrateCeilingKbps > maximumBitrateKbps {
		return false
	}
	return config.AudioCodec == audioCodecOpus &&
		config.AudioChannels >= 1 && config.AudioChannels <= 8 &&
		config.AudioSampleRate == 48000 &&
		legalOpusFrameDuration(config.AudioFrameDurationUS)
}

func encodeMediaConfig(config mediaConfig) ([]byte, error) {
	if !validMediaConfig(config) {
		return nil, errors.New("invalid worker media configuration")
	}
	body := make([]byte, mediaConfigSize)
	body[0] = mediaContractVersion
	body[1] = byte(config.VideoCodec)
	body[2] = config.ProfileIDC
	body[3] = config.LevelIDC
	binary.BigEndian.PutUint16(body[4:6], config.Width)
	binary.BigEndian.PutUint16(body[6:8], config.Height)
	binary.BigEndian.PutUint32(body[8:12], config.FPSNumerator)
	binary.BigEndian.PutUint32(body[12:16], config.FPSDenominator)
	binary.BigEndian.PutUint32(body[16:20], config.BitrateCeilingKbps)
	body[20] = byte(config.AudioCodec)
	body[21] = config.AudioChannels
	binary.BigEndian.PutUint16(body[22:24], config.AudioFrameDurationUS)
	binary.BigEndian.PutUint32(body[24:28], config.AudioSampleRate)
	// bytes 28-31 are reserved and stay zero.
	return body, nil
}

func parseMediaConfig(body []byte) (mediaConfig, error) {
	if len(body) != mediaConfigSize || body[0] != mediaContractVersion ||
		binary.BigEndian.Uint32(body[28:32]) != 0 {
		return mediaConfig{}, errors.New("invalid worker media configuration body")
	}
	config := mediaConfig{
		VideoCodec:           videoCodec(body[1]),
		ProfileIDC:           body[2],
		LevelIDC:             body[3],
		Width:                binary.BigEndian.Uint16(body[4:6]),
		Height:               binary.BigEndian.Uint16(body[6:8]),
		FPSNumerator:         binary.BigEndian.Uint32(body[8:12]),
		FPSDenominator:       binary.BigEndian.Uint32(body[12:16]),
		BitrateCeilingKbps:   binary.BigEndian.Uint32(body[16:20]),
		AudioCodec:           audioCodec(body[20]),
		AudioChannels:        body[21],
		AudioFrameDurationUS: binary.BigEndian.Uint16(body[22:24]),
		AudioSampleRate:      binary.BigEndian.Uint32(body[24:28]),
	}
	if !validMediaConfig(config) {
		return mediaConfig{}, errors.New("invalid worker media configuration")
	}
	return config, nil
}

// encodeMediaFrame builds one media payload: the fixed prefix followed by
// the encoded bytes. An empty frame or one over the limit is rejected.
func encodeMediaFrame(frame mediaFrame, encoded []byte) ([]byte, error) {
	if len(encoded) == 0 || len(encoded) > maxMediaPayload-mediaFramePrefixSize {
		return nil, errors.New("invalid worker media frame")
	}
	payload := make([]byte, mediaFramePrefixSize+len(encoded))
	payload[0] = mediaContractVersion
	if frame.IDR {
		payload[1] = mediaFrameFlagIDR
	}
	// bytes 2-7 are reserved and stay zero.
	binary.BigEndian.PutUint64(payload[8:16], frame.FrameIndex)
	binary.BigEndian.PutUint64(payload[16:24], frame.CaptureTimestampNS)
	binary.BigEndian.PutUint64(payload[24:32], frame.EncodeTimestampNS)
	copy(payload[mediaFramePrefixSize:], encoded)
	return payload, nil
}

// parseMediaFrame splits one media payload into its prefix and encoded
// bytes. It rejects an unknown version, unknown flags, non-zero reserved
// bytes and a payload without at least one encoded byte after the prefix.
func parseMediaFrame(payload []byte) (mediaFrame, []byte, error) {
	if len(payload) <= mediaFramePrefixSize || len(payload) > maxMediaPayload ||
		payload[0] != mediaContractVersion ||
		payload[1]&^mediaFrameKnownFlags != 0 ||
		binary.BigEndian.Uint16(payload[2:4]) != 0 ||
		binary.BigEndian.Uint32(payload[4:8]) != 0 {
		return mediaFrame{}, nil, errors.New("invalid worker media frame prefix")
	}
	return mediaFrame{
		FrameIndex:         binary.BigEndian.Uint64(payload[8:16]),
		IDR:                payload[1]&mediaFrameFlagIDR != 0,
		CaptureTimestampNS: binary.BigEndian.Uint64(payload[16:24]),
		EncodeTimestampNS:  binary.BigEndian.Uint64(payload[24:32]),
	}, payload[mediaFramePrefixSize:], nil
}

func encodeFrameRange(value frameRange) ([]byte, error) {
	if value.First > value.Last {
		return nil, errors.New("invalid worker frame range")
	}
	body := make([]byte, frameRangeSize)
	binary.BigEndian.PutUint64(body[0:8], value.First)
	binary.BigEndian.PutUint64(body[8:16], value.Last)
	return body, nil
}

func parseFrameRange(body []byte) (frameRange, error) {
	if len(body) != frameRangeSize {
		return frameRange{}, errors.New("invalid worker frame range body")
	}
	value := frameRange{
		First: binary.BigEndian.Uint64(body[0:8]),
		Last:  binary.BigEndian.Uint64(body[8:16]),
	}
	if value.First > value.Last {
		return frameRange{}, errors.New("invalid worker frame range")
	}
	return value, nil
}
