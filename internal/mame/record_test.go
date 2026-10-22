package mame

import (
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"
)

func TestRecordArgs(t *testing.T) {
	dir := `C:\Videos\RetroPixels\temp\MAME-1`
	want := []string{"-snapshot_directory", dir, "-aviwrite", AVIName}
	if got := RecordArgs(dir, nil); !reflect.DeepEqual(got, want) {
		t.Errorf("unknown options: got %q", got)
	}
	opts := ParseShowConfig(showConfig + "snapshot_directory snap\naviwrite\nmngwrite\n")
	if got := RecordArgs(dir, opts); !reflect.DeepEqual(got, want) {
		t.Errorf("got %q", got)
	}
	if got := RecordArgs(dir, ParseShowConfig(showConfig)); got != nil {
		t.Errorf("a MAME without -aviwrite: got %q", got)
	}
}

const probeAVI = `Input #0, avi, from 'game.avi':
  Duration: 00:01:02.50, start: 0.000000, bitrate: 39000 kb/s
  Stream #0:0: Video: rawvideo, bgr24, 384x224, 37000 kb/s, 59.64 fps, 59.64 tbr, 59.64 tbn
  Stream #0:1: Audio: pcm_s16le ([1][0][0][0] / 0x0001), 48000 Hz, stereo, s16, 1536 kb/s
At least one output file must be specified
`

func TestParseProbe(t *testing.T) {
	c, err := ParseProbe("game.avi", probeAVI)
	if err != nil {
		t.Fatal(err)
	}
	if c.W != 384 || c.H != 224 || c.Dur != 62.5 || !c.Sound {
		t.Errorf("got %+v", c)
	}
	if _, err := ParseProbe("x.avi", "x.avi: Invalid data found when processing input"); err == nil {
		t.Error("no picture should be an error")
	}
	c, _ = ParseProbe("v.avi", strings.Replace(probeAVI, "Audio", "Data", 1))
	if c.Sound {
		t.Error("no sound stream")
	}
}

func TestOutputSize(t *testing.T) {
	for _, tc := range []struct{ w, h, ow, oh int }{
		{384, 224, 1194, 896}, // CPS: wide pixels, shown 4:3
		{320, 240, 1280, 960},
		{224, 288, 648, 864}, // upright screen: 3:4
		{288, 224, 1194, 896},
		{864, 224, 3456, 896}, // three screens side by side keep their shape
		{640, 480, 1280, 960},
		{1280, 1100, 1466, 1100}, // already big: as it is
	} {
		if ow, oh := OutputSize(tc.w, tc.h); ow != tc.ow || oh != tc.oh {
			t.Errorf("OutputSize(%d, %d) = %d×%d, want %d×%d", tc.w, tc.h, ow, oh, tc.ow, tc.oh)
		}
	}
}

func TestConvertArgs(t *testing.T) {
	one := []Clip{{Path: "a.avi", W: 384, H: 224, Dur: 60, Sound: true}}
	got := strings.Join(ConvertArgs(one, "out.mp4"), " ")
	for _, want := range []string{
		"-i a.avi -map 0:v:0 -map 0:a:0? -vf scale=1536:896:flags=neighbor,scale=1194:896:flags=lanczos,setsar=1,format=yuv420p",
		"-c:v libx264", "-crf 17", "-c:a aac", "-movflags +faststart out.mp4",
	} {
		if !strings.Contains(got, want) {
			t.Errorf("missing %q in\n%s", want, got)
		}
	}
	two := append(one, Clip{Path: "b.avi", W: 384, H: 224, Dur: 3, Sound: true})
	got = strings.Join(ConvertArgs(two, "out.mp4"), " ")
	if !strings.Contains(got, "[v0][0:a:0][v1][1:a:0]concat=n=2:v=1:a=1[cv][a];[cv]format=yuv420p[v]") ||
		!strings.Contains(got, "-map [v] -map [a]") {
		t.Errorf("two parts:\n%s", got)
	}
	two[1].Sound = false
	got = strings.Join(ConvertArgs(two, "out.mp4"), " ")
	if !strings.Contains(got, "[v0][v1]concat=n=2:v=1:a=0[cv]") || strings.Contains(got, "-map [a]") {
		t.Errorf("two parts, one without sound:\n%s", got)
	}
}

func TestAVIsAndSnapshots(t *testing.T) {
	dir := t.TempDir()
	exeDir := t.TempDir()
	os.MkdirAll(filepath.Join(dir, "sf2"), 0o755)
	for _, f := range []string{"game.avi", "game_1.avi", filepath.Join("sf2", "0000.png"), "notes.txt"} {
		os.WriteFile(filepath.Join(dir, f), []byte("x"), 0o644)
	}
	got := AVIs(dir)
	if len(got) != 2 || filepath.Base(got[0]) != "game.avi" || filepath.Base(got[1]) != "game_1.avi" {
		t.Errorf("AVIs = %q", got)
	}
	KeepSnapshots(dir, filepath.Join(exeDir, "mame.exe"))
	if _, err := os.Stat(filepath.Join(exeDir, "snap", "sf2", "0000.png")); err != nil {
		t.Error("the screenshot wasn't moved to MAME's snap folder")
	}
}

func TestConvertNothing(t *testing.T) {
	// no AVI (MAME never showed a picture): nothing is made
	if err := Convert("ffmpeg-that-isnt-there", t.TempDir(), filepath.Join(t.TempDir(), "x.mp4"), nil); err != ErrTooShort {
		t.Errorf("got %v", err)
	}
}
