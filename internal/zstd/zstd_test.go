package zstd

import (
	"bytes"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// TestDecodeFiles checks .zst files made by the reference zstd against
// their originals (in testdata, or ZSTD_TESTDATA=dir for more).
func TestDecodeFiles(t *testing.T) {
	dir := os.Getenv("ZSTD_TESTDATA")
	if dir == "" {
		dir = "testdata"
	}
	files, _ := filepath.Glob(filepath.Join(dir, "*.zst"))
	for _, f := range files {
		base := filepath.Base(f)
		raw, err := os.ReadFile(filepath.Join(dir, base[:strings.Index(base, ".")]+".raw"))
		if err != nil {
			t.Fatal(err)
		}
		if strings.Contains(base, ".blk.") {
			raw = raw[:min(len(raw), 65536)]
		}
		c, _ := os.ReadFile(f)
		got, err := Decode(c)
		if err != nil {
			t.Errorf("%s: %v", base, err)
			continue
		}
		if !bytes.Equal(got, raw) {
			t.Errorf("%s: %d bytes, want %d", base, len(got), len(raw))
		}
	}
	t.Logf("%d files", len(files))
}

// TestCorrupt: damaged data gives an error, never a crash.
func TestCorrupt(t *testing.T) {
	dir := os.Getenv("ZSTD_TESTDATA")
	if dir == "" {
		dir = "testdata"
	}
	files, _ := filepath.Glob(filepath.Join(dir, "*.zst"))
	seed := uint32(7)
	for _, f := range files {
		c, _ := os.ReadFile(f)
		for k := 0; k < 300 && len(c) > 0; k++ {
			b := append([]byte(nil), c...)
			for j := 0; j < 1+k%4; j++ {
				seed = seed*1664525 + 1013904223
				b[int(seed>>8)%len(b)] ^= byte(seed >> 24)
			}
			func() {
				defer func() {
					if e := recover(); e != nil {
						t.Fatalf("%s: crash on damaged data: %v", f, e)
					}
				}()
				Decode(b)
			}()
		}
	}
}
