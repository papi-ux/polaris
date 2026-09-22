//go:build linux

package main

import (
	"context"
	"os/exec"
	"reflect"
	"strings"
	"syscall"
	"testing"
	"time"
)

// finishingFakeLease is a fake lease that says how it ended, as the real
// process lease does.
type finishingFakeLease struct {
	*fakeRuntimeLease
	finished bool
}

func (lease *finishingFakeLease) Finished() bool { return lease.finished }

// endsWith makes one stage's lease report how it ended.
func endsWith(stage runtimeStage, finished bool) func(*fakeRuntimeSet) {
	return func(set *fakeRuntimeSet) {
		lease := &finishingFakeLease{fakeRuntimeLease: set.leases[stage], finished: finished}
		set.byStage[stage].start = func(context.Context, runtimeAllocation) (runtimeLease, error) {
			return lease, nil
		}
	}
}

// streaming attaches both channels and carries one frame, so the media channel
// is in the state a stream is in when the title ends.
func streaming(t *testing.T, worker testWorker, plane *fakeWorkerDataPlane) *testConnection {
	t.Helper()
	control := connectAndAuthenticate(t, worker, channelControl)
	media := connectAndAuthenticate(t, worker, channelMedia)
	t.Cleanup(func() { _ = control.connection.Close(); _ = media.connection.Close() })
	control.attach(t)
	media.attach(t)
	plane.media <- routedOutput{Identity: worker.config.Identity, Message: messageVideo, Payload: []byte("frame")}
	if frame := media.read(t); frame.Message != messageVideo {
		t.Fatalf("the stream did not carry its frame: %+v", frame)
	}
	return media
}

func workerResult(t *testing.T, worker testWorker, within time.Duration) error {
	t.Helper()
	select {
	case err := <-worker.done:
		return err
	case <-time.After(within):
		t.Fatal("the worker did not stop")
		return nil
	}
}

func TestATitleThatEndsByItselfEndsTheStreamAndTheWorkerCleanly(t *testing.T) {
	plane := newFakeWorkerDataPlane()
	worker, runtime := createRoutedTestWorkerWith(t, "worker-finished", 91, 0, plane,
		endsWith(runtimeStageLauncherProcessTree, true))
	media := streaming(t, worker, plane)

	runtime.leases[runtimeStageLauncherProcessTree].finish(nil)

	// The controller is told, on the channel it reads media from, before anything closes.
	if frame := media.read(t); frame.Message != messageEndOfStream || len(frame.Payload) != 0 {
		t.Fatalf("the controller was not told the stream ended: %+v", frame)
	}
	if err := workerResult(t, worker, 2*time.Second); err != nil {
		t.Fatalf("a title that ended by itself was reported as a fault: %v", err)
	}
	if events, _ := runtime.recorder.snapshot(); !reflect.DeepEqual(events, completeRuntimeEvents()) {
		t.Fatalf("the rest of the seat was not taken down in order: %#v", events)
	}
}

func TestALauncherThatFailsIsStillAFailureAndSaysNothingOnMedia(t *testing.T) {
	plane := newFakeWorkerDataPlane()
	worker, runtime := createRoutedTestWorkerWith(t, "worker-launcher-failed", 92, 0, plane,
		endsWith(runtimeStageLauncherProcessTree, false))
	media := streaming(t, worker, plane)

	runtime.leases[runtimeStageLauncherProcessTree].finish(nil)

	err := workerResult(t, worker, 2*time.Second)
	if err == nil || !strings.Contains(err.Error(), "launcher-process-tree exited unexpectedly") {
		t.Fatalf("a launcher that did not return success was not a failure: %v", err)
	}
	_ = media.connection.SetReadDeadline(time.Now().Add(time.Second))
	if frame, readError := readFrame(media.connection, channelMedia, worker.config.Identity.Slot,
		worker.config.Identity.Generation); readError == nil {
		t.Fatalf("a failed launcher was announced as an ending: %+v", frame)
	}
}

func TestOnlyTheLauncherMayFinish(t *testing.T) {
	plane := newFakeWorkerDataPlane()
	worker, runtime := createRoutedTestWorkerWith(t, "worker-encoder-ended", 93, 0, plane,
		endsWith(runtimeStageEncoder, true))
	streaming(t, worker, plane)

	// The encoder serves the title for as long as it runs. Status 0 or not, it ending is a fault.
	runtime.leases[runtimeStageEncoder].finish(nil)

	err := workerResult(t, worker, 2*time.Second)
	if err == nil || !strings.Contains(err.Error(), "encoder exited unexpectedly") {
		t.Fatalf("an encoder that ended was not a failure: %v", err)
	}
}

func TestAFinishedTitleDoesNotWaitForAControllerThatIsNotReading(t *testing.T) {
	plane := newFakeWorkerDataPlane()
	worker, runtime := createRoutedTestWorkerWith(t, "worker-finished-unattached", 94, 0, plane,
		endsWith(runtimeStageLauncherProcessTree, true))
	started := time.Now()
	runtime.leases[runtimeStageLauncherProcessTree].finish(nil)
	if err := workerResult(t, worker, 2*time.Second); err != nil {
		t.Fatalf("a finished title with no stream attached was a fault: %v", err)
	}
	if elapsed := time.Since(started); elapsed >= endOfStreamGrace {
		t.Fatalf("the worker waited %v to tell a controller that was never attached", elapsed)
	}
}

func TestAStopThatEndsTheLauncherIsNotReportedAsFinished(t *testing.T) {
	plane := newFakeWorkerDataPlane()
	worker, _ := createRoutedTestWorkerWith(t, "worker-stopped", 95, 0, plane,
		endsWith(runtimeStageLauncherProcessTree, true))
	media := streaming(t, worker, plane)

	// The controller asked for the stop. The launcher ends with status 0 on the way
	// down, and that must not read as a title that ended by itself.
	worker.cancel()
	if err := workerResult(t, worker, 2*time.Second); err != nil {
		t.Fatalf("a requested stop was a fault: %v", err)
	}
	_ = media.connection.SetReadDeadline(time.Now().Add(time.Second))
	if frame, readError := readFrame(media.connection, channelMedia, worker.config.Identity.Slot,
		worker.config.Identity.Generation); readError == nil && frame.Message == messageEndOfStream {
		t.Fatal("a requested stop was announced as the title ending by itself")
	}
}

// The status is read the way exit is observed: without reaping, so the one
// cleanup owner still finds its child waitable.
func TestAHelpersStatusIsReadWithoutReapingIt(t *testing.T) {
	for _, scenario := range []struct {
		name     string
		script   string
		finished bool
	}{
		{"returned success", "exit 0", true},
		{"returned a failure", "exit 3", false},
		{"was killed", "kill -KILL $$", false},
	} {
		t.Run(scenario.name, func(t *testing.T) {
			pidFD := -1
			command := exec.Command("/bin/sh", "-c", scenario.script)
			command.SysProcAttr = &syscall.SysProcAttr{PidFD: &pidFD}
			if err := command.Start(); err != nil {
				t.Fatal(err)
			}
			if pidFD < 0 {
				t.Skip("this kernel has no pidfd")
			}
			defer syscall.Close(pidFD)
			done := make(chan error)
			answers := make(chan bool, 1)
			go observeRuntimeProcessExit(pidFD, done, func(finished bool) { answers <- finished })
			select {
			case <-done:
			case <-time.After(5 * time.Second):
				t.Fatal("the exit was not observed")
			}
			// The answer is there by the time done closes, which is when the runtime asks.
			select {
			case finished := <-answers:
				if finished != scenario.finished {
					t.Fatalf("finished = %v, want %v", finished, scenario.finished)
				}
			default:
				t.Fatal("done closed before the status was recorded")
			}
			// Still waitable: looking did not reap it.
			err := command.Wait()
			if scenario.finished != (err == nil) {
				t.Fatalf("Wait after looking: %v", err)
			}
		})
	}
	if runtimeProcessReturnedSuccess(-1) {
		t.Fatal("a status that cannot be read was taken for success")
	}
}
