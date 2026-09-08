//go:build linux

package seatprovider

import (
	"context"
	"encoding/json"
	"strings"
	"syscall"
	"testing"
	"time"
)

func TestAudioPolicyExpiredStopBudgetStillTerminatesEveryChild(t *testing.T) {
	var children []*managedChild
	for range 3 {
		child, reader := startProviderChildForTest(t, "ignore-term")
		defer reader.Close()
		defer child.stop(time.Second)
		children = append(children, child)
	}
	started := time.Now()
	_ = stopAudioChildren(children, time.Now().Add(-time.Second))
	if time.Since(started) > time.Second {
		t.Fatal("expired budget performed blocking waits")
	}
	for index, child := range children {
		select {
		case <-child.done:
			status, ok := child.command.ProcessState.Sys().(syscall.WaitStatus)
			if !ok || !status.Signaled() || status.Signal() != syscall.SIGKILL {
				t.Fatalf("child %d was not killed: %v", index, status)
			}
		case <-time.After(time.Second):
			t.Fatalf("expired budget skipped termination of child %d", index)
		}
		if _, err := child.pidFD.Stat(); err == nil {
			t.Fatalf("child %d retained its pidfd", index)
		}
	}
}

func TestAudioPolicySuccessfulProbeCannotOutliveCancellation(t *testing.T) {
	parent, cancel := context.WithCancel(context.Background())
	defer cancel()
	err := waitForProbe(parent, time.Now().Add(time.Second), time.Millisecond, nil, func(time.Duration) error {
		cancel() // The query succeeds after the caller has retired this startup.
		return nil
	})
	if err == nil || !strings.Contains(err.Error(), "canceled") {
		t.Fatalf("canceled probe admitted readiness: %v", err)
	}
	called := false
	if err := waitForProbe(parent, time.Now().Add(time.Second), time.Millisecond, nil, func(time.Duration) error { called = true; return nil }); err == nil || called {
		t.Fatal("already canceled startup invoked its next probe")
	}
}

const allocatedNodeFixture = `[{"id":32,"type":"PipeWire:Interface:Node","info":{"n-input-ports":2,"n-output-ports":2,"props":{"object.id":32,"object.serial":18446744073709551615,"node.name":"seat","media.class":"Audio/Sink","factory.name":"support.null-audio-sink","node.virtual":true}}}]`

func TestAudioPolicyNodeIdentityIsExactAndUnique(t *testing.T) {
	identity, err := parseAllocatedAudioNode([]byte(allocatedNodeFixture), "seat", true)
	if err != nil || identity.id != 32 || identity.serial != ^uint64(0) {
		t.Fatalf("lost integer object identity: %+v, %v", identity, err)
	}
	for name, value := range map[string]string{
		"missing": `[]`, "null": `null`, "trailing": allocatedNodeFixture + `[]`,
		"duplicate":    `[` + allocatedNodeFixture[1:len(allocatedNodeFixture)-1] + `,` + allocatedNodeFixture[1:],
		"other-name":   strings.Replace(allocatedNodeFixture, `"seat"`, `"other"`, 1),
		"object-id":    strings.Replace(allocatedNodeFixture, `"object.id":32`, `"object.id":33`, 1),
		"overflow":     strings.Replace(allocatedNodeFixture, `18446744073709551615`, `18446744073709551616`, 1),
		"float":        strings.Replace(allocatedNodeFixture, `18446744073709551615`, `32.0`, 1),
		"null-serial":  strings.Replace(allocatedNodeFixture, `18446744073709551615`, `null`, 1),
		"hardware":     strings.Replace(allocatedNodeFixture, `"node.virtual":true`, `"node.virtual":false`, 1),
		"unconfigured": strings.Replace(allocatedNodeFixture, `"n-input-ports":2`, `"n-input-ports":0`, 1),
	} {
		t.Run(name, func(t *testing.T) {
			if _, err := parseAllocatedAudioNode([]byte(value), "seat", true); err == nil {
				t.Fatal("invalid allocated node accepted")
			}
		})
	}
	if _, err := parseAllocatedAudioNode([]byte(strings.ReplaceAll(allocatedNodeFixture, `ports":2`, `ports":0`)), "seat", false); err != nil {
		t.Fatal("initial unconfigured sink must retain its identity", err)
	}
}

func TestAudioPolicyReadinessRequiresPlaybackAndMonitorChannels(t *testing.T) {
	var ports []map[string]any
	for index, key := range []string{"in/FL", "in/FR", "out/FL", "out/FR"} {
		parts := strings.Split(key, "/")
		direction := "input"
		props := map[string]any{"node.id": 32, "port.direction": parts[0], "audio.channel": parts[1], "format.dsp": "32 bit float mono audio"}
		if parts[0] == "out" {
			direction = "output"
			props["port.monitor"] = true
		}
		ports = append(ports, map[string]any{"id": 40 + index, "type": "PipeWire:Interface:Port", "info": map[string]any{"direction": direction, "props": props}})
	}
	output, _ := json.Marshal(ports)
	if err := parseAllocatedAudioPorts(output, audioNodeIdentity{id: 32}); err != nil {
		t.Fatal(err)
	}
	for name, mutate := range map[string]func([]map[string]any){
		"wrong-node": func(p []map[string]any) { p[0]["info"].(map[string]any)["props"].(map[string]any)["node.id"] = 33 },
		"missing-monitor": func(p []map[string]any) {
			delete(p[2]["info"].(map[string]any)["props"].(map[string]any), "port.monitor")
		},
		"null-monitor": func(p []map[string]any) {
			p[0]["info"].(map[string]any)["props"].(map[string]any)["port.monitor"] = nil
		},
		"swapped-direction": func(p []map[string]any) { p[0]["info"].(map[string]any)["direction"] = "output" },
		"duplicate-channel": func(p []map[string]any) {
			p[1]["info"].(map[string]any)["props"].(map[string]any)["audio.channel"] = "FL"
		},
		"duplicate-id": func(p []map[string]any) { p[1]["id"] = p[0]["id"] },
	} {
		t.Run(name, func(t *testing.T) {
			var modified []map[string]any
			_ = json.Unmarshal(output, &modified)
			mutate(modified)
			content, _ := json.Marshal(modified)
			if err := parseAllocatedAudioPorts(content, audioNodeIdentity{id: 32}); err == nil {
				t.Fatal("invalid ports accepted")
			}
		})
	}
}

func TestAudioPolicyClientUsesServerCredentials(t *testing.T) {
	const client = `[{"id":33,"type":"PipeWire:Interface:Client","info":{"props":{"application.name":"WirePlumber (polaris)","wireplumber.profile":"polaris","wireplumber.daemon":true,"pipewire.sec.pid":123,"pipewire.sec.uid":1000,"application.process.id":456}}}]`
	if err := parseAudioPolicyClient([]byte(client), 123, 1000); err != nil {
		t.Fatal(err)
	}
	if err := parseAudioPolicyClient([]byte(client), 456, 1000); err == nil {
		t.Fatal("client-reported PID authorized readiness")
	}
	for _, value := range []string{
		strings.Replace(client, `"pipewire.sec.uid":1000`, `"pipewire.sec.uid":1001`, 1),
		strings.Replace(client, `"pipewire.sec.pid":123`, `"pipewire.sec.pid":null`, 1),
		strings.Replace(client, `"wireplumber.profile":"polaris"`, `"wireplumber.profile":"policy"`, 1),
		`[` + client[1:len(client)-1] + `,` + strings.Replace(client[1:], `"id":33`, `"id":34`, 1),
	} {
		if err := parseAudioPolicyClient([]byte(value), 123, 1000); err == nil {
			t.Fatal("invalid policy identity accepted")
		}
	}
}
