package main

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"

	"retropixels/internal/mame"
)

func TestPixelUSaveArgs(t *testing.T) {
	v := []mame.Clip{{Path: `T\Frames\pixelu_framedump0.mkv`, W: 1920, H: 1080, Dur: 60}}
	got := strings.Join(pixeluSaveArgs(v, []string{`T\Audio\pixelu_dspdump.wav`}, 60, 60.5, "libx264", "out.mp4"), " ")
	for _, want := range []string{
		`-i T\Frames\pixelu_framedump0.mkv -ignore_length 1 -i T\Audio\pixelu_dspdump.wav`,
		"[0:v:0]setsar=1[v0];[v0]format=yuv420p[v]",
		"atrim=start=0.500,asetpts=PTS-STARTPTS,apad=whole_dur=60.000,atrim=end=60.000[a]", // the sound started half a second early
		"-map [v] -map [a] -c:v libx264", "-crf 18", "-c:a aac", "+faststart out.mp4",
	} {
		if !strings.Contains(got, want) {
			t.Errorf("missing %q in\n%s", want, got)
		}
	}
	// two picture sizes, no sound
	v = append(v, mame.Clip{Path: "b.mkv", W: 1280, H: 720, Dur: 10})
	got = strings.Join(pixeluSaveArgs(v, nil, 70, 0, "h264_qsv", "out.mp4"), " ")
	if !strings.Contains(got, "[1:v:0]scale=1920:1080:force_original_aspect_ratio=decrease,pad=1920:1080") ||
		!strings.Contains(got, "[v0][v1]concat=n=2:v=1:a=0,format=nv12[v]") || strings.Contains(got, "[a]") {
		t.Errorf("two parts:\n%s", got)
	}
}

func TestPixelUDumps(t *testing.T) {
	dir, err := pixeluRecDir(t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	for _, f := range []string{"Frames/pixelu_framedump10.mkv", "Frames/pixelu_framedump2.mkv", "Frames/x.txt", "Audio/pixelu_dspdump.wav"} {
		os.WriteFile(filepath.Join(dir, f), []byte("x"), 0o644)
	}
	v, s := pixeluDumps(dir)
	if len(v) != 2 || filepath.Base(v[0]) != "pixelu_framedump2.mkv" || len(s) != 1 {
		t.Errorf("video %q sound %q", v, s)
	}
}

// With a real ffmpeg: PixelU's files become one .mp4; a game closed
// straight away leaves nothing.
func TestPixelUSave(t *testing.T) {
	ff, err := exec.LookPath("ffmpeg")
	if err != nil {
		t.Skip("no ffmpeg")
	}
	dir, _ := pixeluRecDir(t.TempDir())
	run := func(args ...string) {
		if out, err := exec.Command(ff, append([]string{"-loglevel", "error", "-y"}, args...)...).CombinedOutput(); err != nil {
			t.Fatalf("%v %s", err, out)
		}
	}
	run("-f", "lavfi", "-i", "testsrc=s=320x180:r=60:d=6", "-c:v", "libx264", "-preset", "ultrafast", filepath.Join(dir, "Frames", "pixelu_framedump0.mkv"))
	run("-f", "lavfi", "-i", "sine=r=48000:d=6.3", "-ac", "2", filepath.Join(dir, "Audio", "pixelu_dspdump.wav"))
	out := filepath.Join(t.TempDir(), "Game.mp4")
	if err := pixeluSave(ff, "libx264", dir, out, 10, nil); err != nil {
		t.Fatal(err)
	}
	b, _ := exec.Command(ff, "-hide_banner", "-i", out).CombinedOutput()
	if c, err := mame.ParseProbe(out, string(b)); err != nil || c.W != 320 || !c.Sound || c.Dur < 5.9 || c.Dur > 6.1 {
		t.Errorf("made %+v %v\n%s", c, err, b)
	}

	short, _ := pixeluRecDir(t.TempDir())
	run("-f", "lavfi", "-i", "testsrc=s=320x180:r=60:d=2", "-c:v", "libx264", "-preset", "ultrafast", filepath.Join(short, "Frames", "pixelu_framedump0.mkv"))
	if err := pixeluSave(ff, "libx264", short, out+"2.mp4", 3, nil); err != errRecTooShort {
		t.Errorf("a 2 second game: %v", err)
	}
	empty, _ := pixeluRecDir(t.TempDir())
	if err := pixeluSave(ff, "libx264", empty, out+"3.mp4", 60, nil); err != errNoPicture {
		t.Errorf("no picture after a minute: %v", err)
	}
}
