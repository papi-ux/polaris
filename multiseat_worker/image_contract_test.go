package main

import (
	"encoding/json"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
)

type lockedImage struct {
	ID        string `json:"id"`
	Launcher  string `json:"launcher"`
	Reference string `json:"reference"`
	Source    string `json:"source"`
}

type imageLock struct {
	Schema          int           `json:"schema"`
	Platform        string        `json:"platform"`
	Builder         lockedImage   `json:"builder"`
	RuntimeProfiles []lockedImage `json:"runtime_profiles"`
}

func repositoryFile(t *testing.T, elements ...string) []byte {
	t.Helper()
	path := filepath.Join(append([]string{".."}, elements...)...)
	content, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	return content
}

func TestImageInputsAreDigestPinnedAndComplete(t *testing.T) {
	var lock imageLock
	if err := json.Unmarshal(
		repositoryFile(t, "containers", "multiseat", "images.lock.json"),
		&lock,
	); err != nil {
		t.Fatal(err)
	}
	if lock.Schema != 1 || lock.Platform != "linux/amd64" {
		t.Fatalf("unsupported image lock: %+v", lock)
	}
	digestReference := regexp.MustCompile(`^[a-z0-9][a-z0-9._/-]*(?::[a-z0-9._-]+)?@sha256:[0-9a-f]{64}$`)
	all := append([]lockedImage{lock.Builder}, lock.RuntimeProfiles...)
	seenReferences := map[string]bool{}
	seenIDs := map[string]bool{}
	for _, image := range all {
		if image.ID == "" || seenIDs[image.ID] || image.Source == "" ||
			!digestReference.MatchString(image.Reference) || seenReferences[image.Reference] {
			t.Fatalf("invalid or duplicate locked image: %+v", image)
		}
		seenIDs[image.ID] = true
		seenReferences[image.Reference] = true
	}
	for _, required := range []string{"go", "gamescope", "steam", "heroic", "lutris"} {
		if !seenIDs[required] {
			t.Fatalf("required image profile %q is missing", required)
		}
	}
}

func TestContainerfileUsesLockedOfflineBuildInputs(t *testing.T) {
	containerfile := string(repositoryFile(t, "containers", "multiseat", "Containerfile"))
	lockContent := string(repositoryFile(t, "containers", "multiseat", "images.lock.json"))
	for _, required := range []string{
		"docker.io/library/golang@sha256:e8c859f5632dcfde7b32d2012b4351728f6437930887c2f6a91ea242459e5514",
		"ghcr.io/games-on-whales/base-app@sha256:1d7b61da242e767bc5c80c5fe897392b6a9e6854345d3dea6d2f799e7ea98a14",
		"GOPROXY=off",
		"CGO_ENABLED=0",
		"go test -trimpath ./...",
		"-o /out/polaris-seat-runtime ./cmd/polaris-seat-runtime",
		"-o /out/polaris-seat-session-bus ./cmd/polaris-seat-session-bus",
		"-o /out/polaris-seat-audio ./cmd/polaris-seat-audio",
		"COPY --from=worker-build --chmod=0555 /out/polaris-seat-runtime /usr/bin/polaris-seat-runtime",
		"COPY --from=worker-build --chmod=0555 /out/polaris-seat-session-bus /usr/libexec/polaris-seat/session-bus",
		"COPY --from=worker-build --chmod=0555 /out/polaris-seat-audio /usr/libexec/polaris-seat/audio",
		"test -x /usr/bin/dbus-daemon",
		"test -x /usr/bin/pipewire",
		"test -x /usr/bin/pw-cli",
		"test -x /usr/bin/pactl",
		"test -r /usr/share/pipewire/pipewire.conf",
		"test -r /usr/share/pipewire/pipewire-pulse.conf",
		"COPY containers/multiseat/Containerfile /containers/multiseat/Containerfile",
		"COPY containers/multiseat/images.lock.json /containers/multiseat/images.lock.json",
		"ENTRYPOINT [\"/usr/bin/polaris-seat-worker\"]",
	} {
		if !strings.Contains(containerfile, required) {
			t.Fatalf("Containerfile is missing %q", required)
		}
	}
	if strings.Contains(containerfile, ":latest") || strings.Contains(containerfile, ":edge") ||
		strings.Contains(containerfile, "apt-get") || strings.Contains(containerfile, "curl ") ||
		strings.Contains(containerfile, "git clone") {
		t.Fatal("Containerfile reintroduced a moving or network-fetched build input")
	}
	if !strings.Contains(lockContent, "\"steam\"") ||
		!strings.Contains(lockContent, "\"heroic\"") ||
		!strings.Contains(lockContent, "\"lutris\"") {
		t.Fatal("launcher image lock is incomplete")
	}
}

func TestRuntimeProvidersArePackagedButProcessAdaptersRemainUnwired(t *testing.T) {
	server := string(repositoryFile(t, "multiseat_worker", "server.go"))
	main := string(repositoryFile(t, "multiseat_worker", "main.go"))
	containerfile := string(repositoryFile(t, "containers", "multiseat", "Containerfile"))
	if !strings.Contains(server, "expectedUID,\n\t\tnil,\n\t\truntimeOptions{}") {
		t.Fatal("production worker no longer injects an explicit nil runtime adapter set")
	}
	for _, productionSource := range []string{main, server} {
		if strings.Contains(productionSource, "newProcessRuntimeAdapters") ||
			strings.Contains(productionSource, "polaris-seat-runtime") {
			t.Fatal("process-backed runtime helper was activated by the production worker")
		}
	}
	if !strings.Contains(containerfile,
		"COPY --from=worker-build --chmod=0555 /out/polaris-seat-runtime /usr/bin/polaris-seat-runtime") {
		t.Fatal("inert runtime helper is absent from the worker image")
	}
	for _, provider := range []string{
		"/usr/libexec/polaris-seat/session-bus",
		"/usr/libexec/polaris-seat/audio",
	} {
		if !strings.Contains(containerfile, provider) {
			t.Fatalf("implemented but inert provider %q is absent from the worker image", provider)
		}
	}
	if strings.Contains(containerfile, "multiseat-runtime-providers.json") {
		t.Fatal("worker image installed an unimplemented provider catalog")
	}
}
