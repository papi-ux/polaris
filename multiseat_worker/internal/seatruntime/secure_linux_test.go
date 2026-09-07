//go:build linux

package seatruntime

import (
	"encoding/json"
	"os"
	"path/filepath"
	"testing"
)

func writeSecureCatalogFixture(t *testing.T, mode os.FileMode) string {
	t.Helper()
	content, err := json.Marshal(catalogForTest())
	if err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(t.TempDir(), "providers.json")
	if err := os.WriteFile(path, content, mode); err != nil {
		t.Fatal(err)
	}
	if err := os.Chmod(path, mode); err != nil {
		t.Fatal(err)
	}
	return path
}

func TestLoadCatalogEnforcesOwnerModeAndNoFollow(t *testing.T) {
	path := writeSecureCatalogFixture(t, 0o400)
	if _, err := LoadCatalog(path, uint32(os.Getuid())); err != nil {
		t.Fatalf("trusted fixture was rejected: %v", err)
	}
	if _, err := LoadCatalog(path, uint32(os.Getuid()+1)); err == nil {
		t.Fatal("wrong catalog owner was accepted")
	}
	link := filepath.Join(t.TempDir(), "providers-link.json")
	if err := os.Symlink(path, link); err != nil {
		t.Fatal(err)
	}
	if _, err := LoadCatalog(link, uint32(os.Getuid())); err == nil {
		t.Fatal("catalog symlink was followed")
	}
	if err := os.Chmod(path, 0o600); err != nil {
		t.Fatal(err)
	}
	if _, err := LoadCatalog(path, uint32(os.Getuid())); err == nil {
		t.Fatal("writable catalog was accepted")
	}
}
