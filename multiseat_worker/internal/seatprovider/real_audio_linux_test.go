//go:build linux

package seatprovider

import (
	"encoding/binary"
	"encoding/json"
	"math"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"testing"
	"time"
)

func startRealAudioTone(t *testing.T, options providerOptions, sink string, left, right int) *managedChild {
	t.Helper()
	// A finite stereo WAV gives each channel an independent known signal and
	// exercises 44.1 kHz application audio resampled by the private graph.
	const frames = 20 * 44100
	content := make([]byte, 44+frames*4)
	copy(content, "RIFF")
	binary.LittleEndian.PutUint32(content[4:8], uint32(len(content)-8))
	copy(content[8:16], "WAVEfmt ")
	binary.LittleEndian.PutUint32(content[16:20], 16)
	binary.LittleEndian.PutUint16(content[20:22], 1)
	binary.LittleEndian.PutUint16(content[22:24], 2)
	binary.LittleEndian.PutUint32(content[24:28], 44100)
	binary.LittleEndian.PutUint32(content[28:32], 44100*4)
	binary.LittleEndian.PutUint16(content[32:34], 4)
	binary.LittleEndian.PutUint16(content[34:36], 16)
	copy(content[36:40], "data")
	binary.LittleEndian.PutUint32(content[40:44], frames*4)
	for frame := 0; frame < frames; frame++ {
		for channel, frequency := range []int{left, right} {
			sample := int16(0.015 * 32767 * math.Sin(2*math.Pi*float64(frequency)*float64(frame)/44100))
			binary.LittleEndian.PutUint16(content[44+frame*4+channel*2:], uint16(sample))
		}
	}
	path := filepath.Join(t.TempDir(), "stereo.wav")
	if err := os.WriteFile(path, content, 0o600); err != nil {
		t.Fatal(err)
	}
	arguments := []string{"-q", "filesrc", "location=" + path, "!", "wavparse", "!",
		"pulsesink", "device=" + sink, "server=unix:" + options.runtimeDirectory + "/pulse/native"}
	child, err := startManagedChild(options.gstLaunchPath, options.executableOwnerUID, arguments,
		audioEnvironment(options.runtimeDirectory, sink), nil)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { stopRealAudioTone(t, child, options) })
	return child
}

func stopRealAudioTone(t *testing.T, child *managedChild, options providerOptions) {
	t.Helper()
	if err := child.stop(options.stopTimeout); err != nil {
		t.Fatal(err)
	}
}

func captureRealAudioSamples(options providerOptions, sink string) ([]byte, error) {
	// A finite receiver and bounded stdout avoid writing unbounded sample files.
	// Six 10 ms Pulse buffers fit the production probe's 64 KiB output bound.
	return runTrustedCommand(options.gstLaunchPath, options.executableOwnerUID,
		[]string{"-q", "pulsesrc", "device=" + sink + ".monitor", "server=unix:" + options.runtimeDirectory + "/pulse/native",
			"num-buffers=6", "buffer-time=20000", "latency-time=10000", "!", "audioconvert", "!", "audioresample", "!",
			"audio/x-raw,format=F32LE,layout=interleaved,rate=48000,channels=2", "!", "fdsink", "fd=1", "sync=false"},
		audioEnvironment(options.runtimeDirectory, sink), 3*time.Second)
}

func assertRealAudioSamples(t *testing.T, options providerOptions, sink string, left, right int) {
	t.Helper()
	// Startup may precede the producer's first buffer. Each attempt reconnects
	// through the real protocol; silence cannot pass the waveform assertion.
	deadline := time.Now().Add(4 * time.Second)
	for {
		output, err := captureRealAudioSamples(options, sink)
		if err == nil && audioSamplesMatch(output, left, right) {
			return
		}
		if err != nil || time.Now().After(deadline) {
			t.Fatalf("private audio samples did not match stereo tones %d/%d: bytes=%d, %v", left, right, len(output), err)
		}
	}
}

func audioSamplesMatch(output []byte, left, right int) bool {
	if len(output)%8 != 0 || len(output) < 16000 || len(output) > maximumProviderOutput {
		return false
	}
	frames := len(output) / 8
	for channel, frequency := range []int{left, right} {
		var energy, sine, cosine float64
		for frame := 0; frame < frames; frame++ {
			offset := frame*8 + channel*4
			value := float64(math.Float32frombits(binary.LittleEndian.Uint32(output[offset : offset+4])))
			if math.IsNaN(value) || math.IsInf(value, 0) || math.Abs(value) > 1 {
				return false
			}
			angle := 2 * math.Pi * float64(frequency) * float64(frame) / 48000
			energy += value * value
			sine += value * math.Sin(angle)
			cosine += value * math.Cos(angle)
		}
		rms := math.Sqrt(energy / float64(frames))
		// The requested tone must account for most energy in its own channel.
		// Wrong seats, swapped channels, silence and mixed tones fail separately.
		if rms < 0.005 || rms > 0.025 || 2*(sine*sine+cosine*cosine)/(float64(frames)*energy) < 0.8 {
			return false
		}
	}
	return true
}

func realAudioNodes(t *testing.T, options providerOptions, sink string) []audioGraphObject {
	t.Helper()
	output, err := dumpAudioGraph(options, audioEnvironment(options.runtimeDirectory, sink), sink, options.probeTimeout)
	var nodes []audioGraphObject
	if err != nil || json.Unmarshal(output, &nodes) != nil {
		t.Fatalf("audio node query failed: %v", err)
	}
	return nodes
}

func configuredRealAudioNodes(t *testing.T, options providerOptions, sink string, count int) []audioGraphObject {
	t.Helper()
	deadline := time.Now().Add(2 * time.Second)
	for {
		nodes := realAudioNodes(t, options, sink)
		configured := len(nodes) == count
		for _, node := range nodes {
			configured = configured && node.Info.InputPorts == 2 && node.Info.OutputPorts == 2
		}
		if configured {
			return nodes
		}
		if time.Now().After(deadline) {
			t.Fatal("negative audio fixture did not configure its real playback and monitor ports")
		}
		time.Sleep(10 * time.Millisecond)
	}
}

func mutateRealAudioNode(t *testing.T, options providerOptions, sink string, remove uint32) {
	t.Helper()
	arguments := []string{"-r", "pipewire-0", "create-node", "adapter",
		"{ factory.name=support.null-audio-sink node.name=" + sink + " media.class=Audio/Sink object.linger=true node.virtual=true audio.position=[ FL FR ] }"}
	if remove != 0 {
		arguments = []string{"-r", "pipewire-0", "destroy", strconv.FormatUint(uint64(remove), 10)}
	}
	if _, err := runTrustedCommand(options.pwCLIPath, options.executableOwnerUID, arguments,
		audioEnvironment(options.runtimeDirectory, sink), options.probeTimeout); err != nil {
		t.Fatal(err)
	}
}

func assertRejectedRealAudioTone(t *testing.T, options providerOptions, sink string) {
	t.Helper()
	child := startRealAudioTone(t, options, sink, 440, 660)
	select {
	case <-child.done:
		if child.command.ProcessState.Success() {
			t.Fatal("disallowed audio client exited successfully")
		}
	case <-time.After(3 * time.Second):
		t.Fatal("policy did not reject the disallowed live audio client")
	}
}

func TestRealPrivateAudioPolicyRejectsWrongTargetsAndReplacement(t *testing.T) {
	options := realProviderOptions(t, privateRuntimeDirectoryForTest(t), true)
	const sink = "polaris-policy-original"
	provider := startRealAudio(t, options, audioRequest("real-policy", sink))
	nodes := realAudioNodes(t, options, sink)
	if len(nodes) != 1 {
		t.Fatal("allocated node missing")
	}
	original := nodes[0]
	originalSerial, _ := audioProperty[uint64](original, "object.serial")

	// A real second sink exists and is configured, but cannot attract streams.
	const other = "polaris-policy-other"
	mutateRealAudioNode(t, options, other, 0)
	otherNodes := configuredRealAudioNodes(t, options, other, 1)
	if len(otherNodes) != 1 {
		t.Fatal("wrong-target fixture missing")
	}
	assertRejectedRealAudioTone(t, options, other)
	mutateRealAudioNode(t, options, other, otherNodes[0].ID)

	// Duplicate names cannot select whichever device happens to enumerate first.
	mutateRealAudioNode(t, options, sink, 0)
	nodes = configuredRealAudioNodes(t, options, sink, 2)
	if len(nodes) != 2 {
		t.Fatal("duplicate sink fixture missing")
	}
	assertRejectedRealAudioTone(t, options, sink)
	for _, node := range nodes {
		if node.ID != original.ID {
			mutateRealAudioNode(t, options, sink, node.ID)
		}
	}
	tone := startRealAudioTone(t, options, sink, 440, 660)
	assertRealAudioSamples(t, options, sink, 440, 660)
	stopRealAudioTone(t, tone, options)

	// Reusing the original name (and possibly numeric ID) cannot reuse its serial.
	mutateRealAudioNode(t, options, sink, original.ID)
	mutateRealAudioNode(t, options, sink, 0)
	nodes = configuredRealAudioNodes(t, options, sink, 1)
	if len(nodes) != 1 || audioPropertyEquals(nodes[0], "object.serial", originalSerial) {
		t.Fatal("replacement sink fixture missing")
	}
	assertRejectedRealAudioTone(t, options, sink)
	if output, err := captureRealAudioSamples(options, sink); err == nil || len(output) != 0 {
		t.Fatal("replacement monitor admitted a capture client")
	}
	stopRealProvider(t, provider)
	requireEmptyRuntime(t, options.runtimeDirectory)
}

func TestRealPrivateAudioPolicyDeathRetiresProvider(t *testing.T) {
	options := realProviderOptions(t, privateRuntimeDirectoryForTest(t), true)
	const sink = "polaris-policy-death"
	provider := startRealAudio(t, options, audioRequest("real-policy-death", sink))
	query := func() []byte {
		output, err := dumpAudioGraph(options, audioEnvironment(options.runtimeDirectory, sink), "Client", options.probeTimeout)
		if err != nil {
			t.Fatal(err)
		}
		return output
	}
	clients, err := decodeAudioGraph(query())
	if err != nil {
		t.Fatal(err)
	}
	var pid uint32
	for _, client := range clients {
		if audioPropertyEquals(client, "application.name", "WirePlumber (polaris)") {
			if pid != 0 {
				t.Fatal("ambiguous policy fixture")
			}
			pid, _ = audioProperty[uint32](client, "pipewire.sec.pid")
		}
	}
	if pid <= 1 {
		t.Fatal("policy fixture missing")
	}
	life, err := retainProcessLifetime(int(pid), -1, processCookie{})
	if err != nil {
		t.Fatal(err)
	}
	defer life.close()
	if err := parseAudioPolicyClient(query(), pid, options.runtimeOwnerUID); err != nil {
		t.Fatal(err)
	}
	if err := life.verify(); err != nil {
		t.Fatal(err)
	}
	if _, _, errno := syscall.Syscall6(424 /* pidfd_send_signal */, life.pidFD.Fd(), uintptr(syscall.SIGKILL), 0, 0, 0, 0); errno != 0 {
		t.Fatal(errno)
	}
	provider.stopOnce.Do(func() {
		defer close(provider.stopComplete)
		defer provider.cancel()
		defer provider.ready.Close()
		select {
		case err := <-provider.done:
			if err == nil || !strings.Contains(err.Error(), "audio policy exited unexpectedly") {
				t.Fatalf("policy death was not propagated: %v", err)
			}
		case <-time.After(4 * time.Second):
			t.Fatal("policy death left the audio provider alive")
		}
	})
	requireEmptyRuntime(t, options.runtimeDirectory)
}
