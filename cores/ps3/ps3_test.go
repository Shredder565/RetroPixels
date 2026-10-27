package ps3

import (
	"os"
	"strconv"
	"testing"
	"time"

	"retropixels/core"
)

// TestPKG opens a game's package as the app does (RETRO_PS3_PKG=<file>):
// it installs it, then runs the game for a while.
func TestPKG(t *testing.T) {
	pkg := os.Getenv("RETRO_PS3_PKG")
	if pkg == "" {
		t.Skip("RETRO_PS3_PKG not set")
	}
	dir := os.Getenv("RETRO_PS3_DATA")
	if dir == "" {
		dir = t.TempDir()
	}
	c, err := New(nil, core.Options{SampleRate: 44100, Path: pkg, DataDir: dir})
	if err != nil {
		t.Fatal(err)
	}
	defer c.(*PS3).Close()
	start := time.Now()
	for c.(*PS3).installing {
		c.RunFrame()
		time.Sleep(50 * time.Millisecond)
		if time.Since(start) > 10*time.Minute {
			t.Fatal("install took too long")
		}
	}
	t.Logf("installed in %v", time.Since(start))
	frames := 600
	if n, err := strconv.Atoi(os.Getenv("RETRO_PS3_FRAMES")); err == nil {
		frames = n
	}
	start = time.Now()
	samples := 0
	for i := 0; i < frames; i++ {
		c.RunFrame()
		samples += len(c.TakeAudio())
		if p := c.(*PS3).fatal; p != "" {
			t.Fatalf("frame %d: %s", i, p)
		}
	}
	t.Logf("%d frames in %v, %d sound samples, picture %dx%d", frames, time.Since(start), samples, c.Width(), c.Height())
}
