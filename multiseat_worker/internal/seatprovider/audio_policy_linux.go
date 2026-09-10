//go:build linux

package seatprovider

import (
	"encoding/json"
	"errors"
	"strconv"
	"time"
)

// pw-dump is queried before launcher admission. Keep integer identities exact;
// decoding properties through float64 would alias large object serials.
type audioGraphObject struct {
	ID   uint32 `json:"id"`
	Type string `json:"type"`
	Info struct {
		InputPorts  uint32                     `json:"n-input-ports"`
		OutputPorts uint32                     `json:"n-output-ports"`
		Direction   string                     `json:"direction"`
		Properties  map[string]json.RawMessage `json:"props"`
	} `json:"info"`
}

type audioNodeIdentity struct {
	id     uint32
	serial uint64
}

func audioProperty[T string | bool | uint64 | uint32](object audioGraphObject, key string) (T, bool) {
	var value T
	raw, present := object.Info.Properties[key]
	if !present || string(raw) == "null" || json.Unmarshal(raw, &value) != nil {
		return value, false
	}
	return value, true
}

func audioPropertyEquals[T string | bool | uint64 | uint32](object audioGraphObject, key string, expected T) bool {
	value, valid := audioProperty[T](object, key)
	return valid && value == expected
}

func decodeAudioGraph(output []byte) ([]audioGraphObject, error) {
	var objects []audioGraphObject
	if len(output) == 0 || len(output) > maximumProviderOutput ||
		json.Unmarshal(output, &objects) != nil || len(objects) == 0 || len(objects) > 128 {
		return nil, errors.New("runtime audio graph response is invalid")
	}
	seen := make(map[uint32]bool, len(objects))
	for _, object := range objects {
		if object.ID == 0 || seen[object.ID] || object.Info.Properties == nil {
			return nil, errors.New("runtime audio graph identity is invalid")
		}
		seen[object.ID] = true
	}
	return objects, nil
}

func parseAllocatedAudioNode(output []byte, sink string, configured bool) (audioNodeIdentity, error) {
	objects, err := decodeAudioGraph(output)
	if err != nil || len(objects) != 1 {
		return audioNodeIdentity{}, errors.New("runtime allocated audio node is ambiguous")
	}
	node := objects[0]
	serial, validSerial := audioProperty[uint64](node, "object.serial")
	if node.Type != "PipeWire:Interface:Node" || !validSerial || serial == 0 ||
		!audioPropertyEquals(node, "object.id", node.ID) ||
		!audioPropertyEquals(node, "node.name", sink) ||
		!audioPropertyEquals(node, "media.class", "Audio/Sink") ||
		!audioPropertyEquals(node, "factory.name", "support.null-audio-sink") ||
		!audioPropertyEquals(node, "node.virtual", true) ||
		(configured && (node.Info.InputPorts != 2 || node.Info.OutputPorts != 2)) {
		return audioNodeIdentity{}, errors.New("runtime allocated audio node is invalid")
	}
	return audioNodeIdentity{id: node.ID, serial: serial}, nil
}

func parseAllocatedAudioPorts(output []byte, node audioNodeIdentity) error {
	objects, err := decodeAudioGraph(output)
	if err != nil {
		return err
	}
	seen := make(map[string]bool, 4)
	for _, port := range objects {
		if port.Type != "PipeWire:Interface:Port" {
			return errors.New("runtime audio port type is invalid")
		}
		if !audioPropertyEquals(port, "node.id", node.id) {
			continue
		}
		channel, ok := audioProperty[string](port, "audio.channel")
		if !ok || (channel != "FL" && channel != "FR") ||
			!audioPropertyEquals(port, "format.dsp", "32 bit float mono audio") {
			return errors.New("runtime allocated audio port format is invalid")
		}
		direction, ok := audioProperty[string](port, "port.direction")
		monitor, hasMonitor := audioProperty[bool](port, "port.monitor")
		if _, present := port.Info.Properties["port.monitor"]; present && !hasMonitor {
			return errors.New("runtime allocated audio monitor property is invalid")
		}
		if !ok || !((direction == "in" && port.Info.Direction == "input" && !monitor) ||
			(direction == "out" && port.Info.Direction == "output" && hasMonitor && monitor)) {
			return errors.New("runtime allocated audio port direction is invalid")
		}
		key := direction + "/" + channel
		if seen[key] {
			return errors.New("runtime allocated audio port is ambiguous")
		}
		seen[key] = true
	}
	if len(seen) != 4 {
		return errors.New("runtime allocated audio ports are unavailable")
	}
	return nil
}

func parseAudioPolicyClient(output []byte, pid uint32, uid uint32) error {
	objects, err := decodeAudioGraph(output)
	if err != nil || pid == 0 {
		return errors.New("runtime audio policy client is invalid")
	}
	matches := 0
	for _, client := range objects {
		if client.Type != "PipeWire:Interface:Client" {
			return errors.New("runtime audio client type is invalid")
		}
		if !audioPropertyEquals(client, "pipewire.sec.pid", pid) {
			continue
		}
		// These credentials are supplied by the private PipeWire server, unlike
		// client-controlled application.process.id or an executable name alone.
		if !audioPropertyEquals(client, "pipewire.sec.uid", uid) ||
			!audioPropertyEquals(client, "application.name", "WirePlumber (polaris)") ||
			!audioPropertyEquals(client, "wireplumber.profile", "polaris") ||
			!audioPropertyEquals(client, "wireplumber.daemon", true) {
			return errors.New("runtime audio policy client identity changed")
		}
		matches++
	}
	if matches != 1 {
		return errors.New("runtime audio policy is not attached")
	}
	return nil
}

func dumpAudioGraph(options providerOptions, environment []string, pattern string, remaining time.Duration) ([]byte, error) {
	return runTrustedCommand(options.pwDumpPath, options.executableOwnerUID,
		[]string{"-r", "pipewire-0", pattern}, environment,
		boundedProbeTimeout(remaining, options.probeTimeout))
}

func audioPolicyEnvironment(runtimePath string, sink string, node audioNodeIdentity) []string {
	return append(audioEnvironment(runtimePath, sink),
		"XDG_STATE_HOME=/nonexistent",
		"WIREPLUMBER_CONFIG_DIR=/usr/share/wireplumber",
		"WIREPLUMBER_DATA_DIR=/usr/share/wireplumber",
		"WIREPLUMBER_MODULE_DIR=/usr/lib/x86_64-linux-gnu/wireplumber-0.5",
		"POLARIS_AUDIO_SINK="+sink,
		"POLARIS_AUDIO_NODE_SERIAL="+strconv.FormatUint(node.serial, 10))
}

func probeAudioPolicy(options providerOptions, environment []string, sink string,
	expected audioNodeIdentity, policy *managedChild, remaining time.Duration) error {
	deadline := time.Now().Add(remaining)
	for index, pattern := range []string{sink, "Port", "Client", sink} {
		if policy == nil || policy.exited() {
			return errors.New("runtime audio policy exited before readiness")
		}
		output, err := dumpAudioGraph(options, environment, pattern, time.Until(deadline))
		if err != nil {
			return err
		}
		switch index {
		case 1:
			err = parseAllocatedAudioPorts(output, expected)
		case 2:
			err = parseAudioPolicyClient(output, uint32(policy.command.Process.Pid), options.runtimeOwnerUID)
		default:
			var node audioNodeIdentity
			node, err = parseAllocatedAudioNode(output, sink, true)
			if err == nil && node != expected {
				err = errors.New("runtime allocated audio node was replaced")
			}
		}
		if err != nil {
			return err
		}
	}
	return nil
}
