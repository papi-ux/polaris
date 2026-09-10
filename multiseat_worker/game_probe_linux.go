//go:build linux

package main

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"os/signal"
	"path/filepath"
	"regexp"
	"syscall"
	"time"
)

// This bounded physical probe composes six real resources. It does not start
// an encoder or attach a media data plane and never publishes worker readiness.
const gameProbeRecord = "POLARIS-PHYSICAL-GAME/1\n"

var gameProbeToken = regexp.MustCompile(`^[0-9a-f]{32}$`)

type gameObservation struct {
	Keyboard uint32 `json:"keyboard"`
	Pointer  uint32 `json:"pointer"`
	Gamepad  uint32 `json:"gamepad"`
	Frames   uint32 `json:"frames"`
	PID      uint32 `json:"pid"`
}

// Do not embed bytes.Buffer: its promoted ReadFrom lets io.Copy bypass Write.
type gameProbeOutput struct{ buffer bytes.Buffer }

func (output *gameProbeOutput) Bytes() []byte  { return output.buffer.Bytes() }
func (output *gameProbeOutput) String() string { return output.buffer.String() }

func (output *gameProbeOutput) Write(data []byte) (int, error) {
	if len(data) > 1024-output.buffer.Len() {
		count, _ := output.buffer.Write(data[:1024-output.buffer.Len()])
		return count, errors.New("game observation exceeds bound")
	}
	return output.buffer.Write(data)
}
func readGameObservation(parent context.Context) (gameObservation, error) {
	var observation gameObservation
	ctx, cancel := context.WithTimeout(parent, time.Second)
	defer cancel()
	command := exec.CommandContext(ctx, "/usr/libexec/polaris-seat/game-status")
	command.Env = []string{"DISPLAY=:0", "XDG_RUNTIME_DIR=/run/polaris", "HOME=/nonexistent", "LC_ALL=C"}
	var output gameProbeOutput
	command.Stdout = &output
	command.WaitDelay = 100 * time.Millisecond
	if err := command.Run(); err != nil {
		return observation, errors.New("private game observation unavailable")
	}
	return parseGameObservation(output.Bytes())
}

func parseGameObservation(content []byte) (gameObservation, error) {
	var observation gameObservation
	var required map[string]json.RawMessage
	if len(content) > 1024 || json.Unmarshal(content, &required) != nil || len(required) != 5 {
		return observation, errors.New("private game observation fields invalid")
	}
	for _, name := range []string{"keyboard", "pointer", "gamepad", "frames", "pid"} {
		if data, exists := required[name]; !exists || string(data) == "null" {
			return observation, errors.New("private game observation field missing")
		}
	}
	decoder := json.NewDecoder(bytes.NewReader(content))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&observation); err != nil || observation.PID <= 1 || observation.Frames == 0 {
		return observation, errors.New("private game observation invalid")
	}
	if decoder.Decode(new(any)) != io.EOF {
		return observation, errors.New("private game observation has trailing bytes")
	}
	return observation, nil
}
func gameProbePath(token string) (string, error) {
	if !gameProbeToken.MatchString(token) {
		return "", errors.New("invalid physical probe token")
	}
	return filepath.Join(containerStatePath, "game-probe-"+token+".ready"), nil
}
func gameProbeSignal(path string) error {
	file, _, err := openGameProbeSignal(path)
	if file != nil {
		file.Close()
	}
	return err
}

func openGameProbeSignal(path string) (*os.File, fileIdentity, error) {
	fd, err := syscall.Open(path, syscall.O_RDONLY|syscall.O_NOFOLLOW|syscall.O_NONBLOCK|syscall.O_CLOEXEC, 0)
	if err != nil {
		return nil, fileIdentity{}, err
	}
	file := os.NewFile(uintptr(fd), "game-probe-signal")
	valid := false
	defer func() {
		if !valid {
			file.Close()
		}
	}()
	var status syscall.Stat_t
	if syscall.Fstat(fd, &status) != nil || status.Mode&syscall.S_IFMT != syscall.S_IFREG || status.Mode&07777 != 0600 || status.Uid != uint32(os.Geteuid()) || status.Size != int64(len(gameProbeRecord)) {
		return nil, fileIdentity{}, errors.New("physical probe signal is invalid")
	}
	content, err := io.ReadAll(io.LimitReader(file, int64(len(gameProbeRecord)+1)))
	if err != nil || string(content) != gameProbeRecord {
		return nil, fileIdentity{}, errors.New("physical probe record is invalid")
	}
	valid = true
	return file, fileIdentity{device: uint64(status.Dev), inode: status.Ino}, nil
}
func createGameProbeSignal(path string) (*os.File, fileIdentity, error) {
	fd, err := syscall.Open(path, syscall.O_WRONLY|syscall.O_CREAT|syscall.O_EXCL|syscall.O_NOFOLLOW|syscall.O_CLOEXEC, 0600)
	if err != nil {
		return nil, fileIdentity{}, err
	}
	file := os.NewFile(uintptr(fd), "game-probe-signal")
	var status syscall.Stat_t
	if err := syscall.Fstat(fd, &status); err != nil {
		file.Close()
		return nil, fileIdentity{}, err
	}
	identity := fileIdentity{device: uint64(status.Dev), inode: status.Ino}
	if count, err := file.WriteString(gameProbeRecord); err != nil || count != len(gameProbeRecord) {
		cleanup := removeGameProbeSignal(path, identity)
		file.Close()
		return nil, fileIdentity{}, errors.Join(errors.New("physical probe signal write failed"), cleanup)
	}
	return file, identity, nil
}

func physicalGameProbe(arguments []string) (result error) {
	if len(arguments) != 2 {
		return errors.New("physical game probe requires operation and token")
	}
	path, err := gameProbePath(arguments[1])
	if err != nil {
		return err
	}
	if err := privateDirectory(containerStatePath, uint32(os.Geteuid())); err != nil {
		return err
	}
	switch arguments[0] {
	case "media":
		return physicalEncodedGameProbe(path)
	case "audio":
		return physicalEncodedAudioProbe(path)
	case "state":
		if err := gameProbeSignal(path); err != nil {
			return err
		}
		observation, err := readGameObservation(context.Background())
		if err != nil {
			return err
		}
		return json.NewEncoder(os.Stdout).Encode(observation)
	case "finish":
		if err := gameProbeSignal(path); err != nil {
			return err
		}
		file, _, err := createGameProbeSignal(path + ".finish")
		if err != nil {
			return err
		}
		return file.Close()
	case "start":
	default:
		return errors.New("unknown physical game probe operation")
	}
	config, err := loadWorkerConfig(os.LookupEnv, workloadPlan{Kind: workloadKindGamescope, TargetID: "input-pong-v1"})
	if err != nil {
		return err
	}
	if config.RuntimeProfile != "gamescope" || config.Compositor != "gamescope" || config.DisplayHDR {
		return errors.New("physical game probe allocation is unsupported")
	}
	if err := checkHealth(config, productionPaths(), uint32(os.Geteuid())); err != nil {
		return err
	}
	if err := privateDirectory("/var/lib/polaris-seat", uint32(os.Geteuid())); err != nil {
		return err
	}
	if _, err := os.Lstat(path); !errors.Is(err, os.ErrNotExist) {
		return errors.New("physical probe already exists")
	}
	if _, err := os.Lstat(path + ".finish"); !errors.Is(err, os.ErrNotExist) {
		return errors.New("physical probe finish already exists")
	}
	// One probe owns these singleton resources within this already isolated worker.
	activePath := filepath.Join(containerStatePath, "physical-game.active")
	active, activeIdentity, err := createGameProbeSignal(activePath)
	if err != nil {
		return errors.New("physical game resources already claimed")
	}
	defer func() {
		result = errors.Join(result, removeGameProbeSignal(activePath, activeIdentity))
		active.Close()
	}()
	parent, stop := signal.NotifyContext(context.Background(), syscall.SIGTERM, syscall.SIGINT)
	defer stop()
	lifetime, cancel := context.WithTimeout(parent, 150*time.Second)
	defer cancel()
	allocation, err := runtimeAllocationFromConfig(config)
	if err != nil {
		return err
	}
	adapters, err := newProcessRuntimeAdapters(osRuntimeProcessHost{diagnostics: os.Stderr}, processRuntimeAdapterOptions{CompositorInput: true})
	if err != nil {
		return err
	}
	var leases []runtimeLease
	failures := make(chan error, 6)
	defer func() {
		for i := len(leases) - 1; i >= 0; i-- {
			stopContext, stopCancel := context.WithTimeout(context.Background(), 5*time.Second)
			result = errors.Join(result, leases[i].Stop(stopContext))
			stopCancel()
		}
	}()
	startup, startupCancel := context.WithTimeout(lifetime, 90*time.Second)
	defer startupCancel()
	for _, component := range adapters.ordered() {
		if component.stage == runtimeStageEncoder {
			continue
		} // Explicitly absent from this probe's claims.
		lease, err := component.adapter.Start(startup, allocation)
		if lease != nil {
			leases = append(leases, lease)
		}
		if err != nil {
			return fmt.Errorf("physical %s: %w", component.stage, err)
		}
		stage := component.stage
		go func(lease runtimeLease) {
			select {
			case <-lease.Done():
				failures <- fmt.Errorf("physical %s exited", stage)
				cancel()
			case <-lifetime.Done():
			}
		}(lease)
	}
	for {
		if _, err := readGameObservation(startup); err == nil {
			break
		}
		select {
		case err := <-failures:
			return err
		case <-startup.Done():
			return errors.New("private game did not produce an observation")
		case <-time.After(50 * time.Millisecond):
		}
	}
	ready, identity, err := createGameProbeSignal(path)
	if err != nil {
		return err
	}
	defer func() { result = errors.Join(result, removeGameProbeSignal(path, identity)); ready.Close() }()
	ticker := time.NewTicker(100 * time.Millisecond)
	defer ticker.Stop()
	for {
		select {
		case err := <-failures:
			return err
		case <-lifetime.Done():
			if parent.Err() != nil {
				return nil
			}
			return errors.New("physical game probe lifetime expired")
		case <-ticker.C:
			if finish, finishIdentity, err := openGameProbeSignal(path + ".finish"); err == nil {
				defer finish.Close()
				return removeGameProbeSignal(path+".finish", finishIdentity)
			} else if !errors.Is(err, os.ErrNotExist) {
				return err
			}
		}
	}
}

func removeGameProbeSignal(path string, identity fileIdentity) error {
	return removeGameProbeSignalWith(path, identity, os.Remove)
}
func removeGameProbeSignalWith(path string, identity fileIdentity, remove func(string) error) error {
	current, err := identityOf(path)
	if err != nil || current != identity {
		return errors.New("physical probe marker ownership changed; cleanup unproven")
	}
	if err := remove(path); err != nil {
		return errors.New("physical probe marker removal failed; cleanup unproven")
	}
	if _, err := os.Lstat(path); !errors.Is(err, os.ErrNotExist) {
		return errors.New("physical probe marker remains; cleanup unproven")
	}
	return nil
}
