//go:build !windows

package main

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
	"time"
)

func TestGameCrop(t *testing.T) {
	cases := []struct {
		name   string
		client rect
		mon    rect
		aspect float64
		want   crop
	}{
		{"16:9 monitor fullscreen", rect{0, 0, 2560, 1440}, rect{0, 0, 2560, 1440}, 16.0 / 9, crop{0, 0, 2560, 1440}},
		{"ultrawide fullscreen (bars left/right)", rect{0, 0, 3440, 1440}, rect{0, 0, 3440, 1440}, 16.0 / 9, crop{440, 0, 2560, 1440}},
		{"16:10 fullscreen (bars top/bottom)", rect{0, 0, 1920, 1200}, rect{0, 0, 1920, 1200}, 16.0 / 9, crop{0, 60, 1920, 1080}},
		{"second monitor, ultrawide", rect{2560, 0, 6000, 1440}, rect{2560, 0, 6000, 1440}, 16.0 / 9, crop{440, 0, 2560, 1440}},
		{"monitor left of primary", rect{-1920, 0, 0, 1080}, rect{-1920, 0, 0, 1080}, 16.0 / 9, crop{0, 0, 1920, 1080}},
		{"windowed: whole window", rect{100, 50, 1381, 771}, rect{0, 0, 2560, 1440}, 16.0 / 9, crop{100, 50, 1280, 720}},
		{"stretched: no crop", rect{0, 0, 3440, 1440}, rect{0, 0, 3440, 1440}, 0, crop{0, 0, 3440, 1440}},
		{"4K", rect{0, 0, 3840, 2160}, rect{0, 0, 3840, 2160}, 16.0 / 9, crop{0, 0, 3840, 2160}},
	}
	for _, c := range cases {
		got, err := gameCrop(gameWindow{Client: c.client, Output: c.mon}, c.aspect)
		if err != nil || got != c.want {
			t.Errorf("%s: got %+v err=%v, want %+v", c.name, got, err, c.want)
		}
	}
}

// probe returns "WxH", duration seconds and whether there's an audio stream.
func probe(t *testing.T, ff, file string) (string, float64, bool) {
	out, _ := exec.Command(ff, "-hide_banner", "-i", file).CombinedOutput()
	s := string(out)
	size := regexp.MustCompile(`Video: h264.*?, (\d+x\d+)`).FindStringSubmatch(s)
	dur := regexp.MustCompile(`Duration: (\d+):(\d+):([\d.]+)`).FindStringSubmatch(s)
	if size == nil || dur == nil {
		t.Fatalf("couldn't read %s:\n%s", file, s)
	}
	var h, m int
	var sec float64
	fmt.Sscan(dur[1], &h)
	fmt.Sscan(dur[2], &m)
	fmt.Sscan(dur[3], &sec)
	return size[1], float64(h*3600+m*60) + sec, strings.Contains(s, "Audio: aac")
}

func setupRecTest(t *testing.T) string {
	ff := os.Getenv("RETRO_FFMPEG")
	if ff == "" {
		t.Skip("set RETRO_FFMPEG")
	}
	dir := t.TempDir()
	os.Setenv("RETRO_REC_DIR", dir)
	videoSource = func(win gameWindow, c crop, fps int) string {
		return fmt.Sprintf("testsrc2=size=%dx%d:rate=%d,realtime,format=bgra", c.W, c.H, fps)
	}
	return ff
}

func TestRecordingWithAudio(t *testing.T) {
	ff := setupRecTest(t)
	os.Unsetenv("RETRO_TEST_NOAUDIO")
	win := gameWindow{Client: rect{0, 0, 1720, 720}, Output: rect{0, 0, 1720, 720}} // 21.5:9 → bars
	s, err := startRecording("Mario Kart 8 Deluxe", 1, win, 16.0/9)
	if err != nil {
		t.Fatal(err)
	}
	t.Logf("encoder=%s audio=%s crop=%+v", s.Encoder, s.Audio, s.Crop)
	time.Sleep(4 * time.Second)
	s.stop()
	size, dur, hasAudio := probe(t, ff, s.File)
	t.Logf("file=%s size=%s dur=%.2f audio=%v", filepath.Base(s.File), size, dur, hasAudio)
	if size != "1280x720" || !hasAudio || dur < 3.5 || dur > 6 {
		t.Fatalf("unexpected result size=%s dur=%.2f audio=%v", size, dur, hasAudio)
	}
}

func TestRecordingWithoutAudio(t *testing.T) {
	ff := setupRecTest(t)
	os.Setenv("RETRO_TEST_NOAUDIO", "1")
	defer os.Unsetenv("RETRO_TEST_NOAUDIO")
	win := gameWindow{Client: rect{0, 0, 1280, 800}, Output: rect{0, 0, 1280, 800}}
	s, err := startRecording("Cuphead", 1, win, 16.0/9)
	if err != nil {
		t.Fatal(err)
	}
	time.Sleep(2 * time.Second)
	s.stop()
	size, dur, hasAudio := probe(t, ff, s.File)
	t.Logf("size=%s dur=%.2f audio=%v", size, dur, hasAudio)
	if size != "1280x720" || hasAudio || dur < 1.5 {
		t.Fatalf("unexpected result")
	}
}

// The full automatic flow: the emulator's window appears, the game is
// recorded, the window closes, a second session starts, then the emulator quits.
func TestAutoRecordFlow(t *testing.T) {
	ff := setupRecTest(t)
	winFile := filepath.Join(t.TempDir(), "win")
	os.Setenv("RETRO_TEST_WIN", winFile)
	set := func(title string) {
		if title == "" {
			os.WriteFile(winFile, nil, 0o644) // no window
			return
		}
		os.WriteFile(winFile, []byte("0,0,1920,1200;0,0,1920,1200;"+title), 0o644)
	}
	set("")
	done := make(chan struct{})
	finished := make(chan struct{})
	go func() {
		autoRecord("Super Mario Sunshine", `C:\x\Dolphin.exe`, 1, done, recOpts{Aspect: -1})
		close(finished)
	}()

	time.Sleep(1500 * time.Millisecond)
	if st := recStatus(); st["active"] != nil {
		t.Fatal("recorded before the game window appeared")
	}
	set("Dolphin 2407 | Super Mario Sunshine (GMSE01)")
	time.Sleep(6 * time.Second)
	if recStatus()["active"] == nil {
		t.Fatalf("not recording during game: %v", recStatus())
	}
	set("") // game window closed
	time.Sleep(3 * time.Second)
	if recStatus()["active"] != nil {
		t.Fatal("still recording after the game window closed")
	}
	set("Dolphin 2407 | Super Mario Sunshine (GMSE01)")
	time.Sleep(6 * time.Second)
	close(done) // emulator quits
	<-finished
	files, _ := filepath.Glob(filepath.Join(recordingsDir(), "*.mp4"))
	if len(files) != 2 {
		t.Fatalf("want 2 recordings, got %v", files)
	}
	for _, f := range files {
		size, dur, a := probe(t, ff, f)
		t.Logf("%s %s %.1fs audio=%v", filepath.Base(f), size, dur, a)
		if size != "1920x1080" || !a || !strings.HasPrefix(filepath.Base(f), "Super Mario Sunshine") {
			t.Fatalf("bad recording %s", f)
		}
	}
}

// A game docked in the RetroPixels window is letterboxed like fullscreen, so
// the side bars are cropped even though it doesn't cover the monitor.
func TestGameCropEmbedded(t *testing.T) {
	win := gameWindow{Client: rect{100, 148, 1700, 1048}, Output: rect{0, 0, 1920, 1080}, Embedded: true}
	c, err := gameCrop(win, 4.0/3)
	if err != nil {
		t.Fatal(err)
	}
	if c != (crop{X: 300, Y: 148, W: 1200, H: 900}) {
		t.Fatalf("crop = %+v", c)
	}
	win.Embedded = false
	if c, _ := gameCrop(win, 4.0/3); c.W != 1600 {
		t.Fatalf("windowed crop = %+v", c)
	}
}
