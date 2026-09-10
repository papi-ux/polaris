//go:build linux

package main

import (
	"io"
	"strings"
	"testing"
)

func TestGameProbeOutputBoundsCopyFastPaths(t *testing.T) {
	for _, size := range []int{1024, 1025, 64 * 1024} {
		var output gameProbeOutput
		// LimitedReader hides strings.Reader.WriteTo. io.Copy can otherwise
		// select an embedded bytes.Buffer.ReadFrom and bypass Write's limit.
		_, err := io.Copy(&output, io.LimitReader(strings.NewReader(strings.Repeat("x", size)), int64(size)))
		if size == 1024 {
			if err != nil || len(output.Bytes()) != size {
				t.Fatalf("exact bound failed: %v", err)
			}
		} else if err == nil || len(output.Bytes()) != 1024 {
			t.Fatalf("copy bypassed output bound: size=%d retained=%d error=%v", size, len(output.Bytes()), err)
		}
	}
}
