//go:build !windows

package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

// fakePixelU is a stand-in PixelU.exe: it notes its arguments and, when
// asked to record, writes what PixelU does: the TV picture as .mkv files
// (a new one when the picture size changes; the first has dark edges the
// game drew itself, which must not be cropped off) and the sound as a .wav.
const fakePixelU = `#!/bin/sh
echo "$@" >> "$FAKE_ARGS"
D=""; FFX=""
while [ $# -gt 0 ]; do
  case "$1" in --rp-record) D="$2"; shift ;; --rp-ffmpeg) FFX="$2"; shift ;; esac
  shift
done
[ -n "$D" ] || { sleep 1; exit 0; }
FF="$FFX -hide_banner -loglevel error -y"
$FF -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=4.5" -ac 2 -c:a pcm_s16le "$D/Audio/pixelu_dspdump.wav"
$FF -f lavfi -i "testsrc2=size=1120x720:rate=60:duration=2" -vf "pad=1280:720:80:0:black" -c:v libx264 -crf 14 -pix_fmt yuv420p "$D/Frames/pixelu_framedump0.mkv"
sleep 1
$FF -f lavfi -i "testsrc2=size=1280x720:rate=60:duration=2" -c:v libx264 -crf 14 -pix_fmt yuv420p "$D/Frames/pixelu_framedump1.mkv"
sleep "${FAKE_PLAY:-1}"
`

func setupFakePixelU(t *testing.T) (argsFile string, game string) {
	t.Helper()
	games := filepath.Join(t.TempDir(), "Nintendo Wii - Wii U games")
	game = filepath.Join(games, "WII  U -Hyrule Warriors (US).wua")
	os.MkdirAll(games, 0o755)
	os.WriteFile(game, []byte("WUA0"), 0o644)
	os.WriteFile(filepath.Join(games, "Sonic Heroes.iso"), append(make([]byte, 0x1C), 0xC2, 0x33, 0x9F, 0x3D), 0o644) // GameCube: not listed
	exe := filepath.Join(t.TempDir(), "PixelU", "PixelU.exe")
	os.MkdirAll(filepath.Dir(exe), 0o755)
	os.WriteFile(exe, []byte(fakePixelU), 0o755)
	argsFile = filepath.Join(t.TempDir(), "args")
	for k, v := range map[string]string{"RETRO_REC_DIR": t.TempDir(), "FAKE_ARGS": argsFile, "XDG_CONFIG_HOME": t.TempDir()} {
		t.Setenv(k, v)
	}
	cfgMu.Lock()
	off := false // these tests check PixelU's own window; docking needs Windows
	cfg = config{PixelUExe: exe, PixelUFolders: []string{games}, PixelUEmbed: &off}
	cfgMu.Unlock()
	setManualWanted(false)
	recMu.Lock()
	recLastErr, recLastMsg = "", ""
	recMu.Unlock()
	return argsFile, game
}

func TestPixelULibraryAndLaunch(t *testing.T) {
	argsFile, game := setupFakePixelU(t)
	lib := pixeluLibrary()
	if len(lib) != 1 || lib[0].Name != "Hyrule Warriors" || lib[0].Path != game {
		t.Fatalf("library: %+v", lib)
	}
	if err := launchPixelUGame(filepath.Join(filepath.Dir(game), "elsewhere.wua")); err == nil {
		t.Fatal("started a game that isn't in the library")
	}
	if err := launchPixelUGame(game); err != nil {
		t.Fatal(err)
	}
	if err := launchPixelUGame(game); err == nil {
		t.Error("started a second game while one is running")
	}
	waitIdle(t)
	args, _ := os.ReadFile(argsFile)
	if got := strings.TrimSpace(string(args)); got != "-g "+game+" -f" {
		t.Errorf("args: %q", got)
	}
	cfgMu.Lock()
	recent := cfg.PixelURecent
	off := false
	cfg.PixelUFullscreen = &off
	cfgMu.Unlock()
	if len(recent) != 1 || recent[0].Path != game || recent[0].Name != "Hyrule Warriors" {
		t.Errorf("recent: %+v", recent)
	}
	if lib := pixeluLibrary(); lib[0].Played.IsZero() {
		t.Error("library doesn't show when it was played")
	}
	os.Remove(argsFile)
	if err := launchPixelUGame(game); err != nil {
		t.Fatal(err)
	}
	waitIdle(t)
	if args, _ := os.ReadFile(argsFile); strings.TrimSpace(string(args)) != "-g "+game {
		t.Errorf("fullscreen off: %q", args)
	}
}

func TestPixelUDumps(t *testing.T) {
	dir := t.TempDir()
	since := time.Now()
	for i, rel := range []string{"Frames/pixelu_framedump0.mkv", "Frames/pixelu_framedump1.mkv", "Audio/pixelu_dspdump.wav", "Frames/notes.txt"} {
		p := filepath.Join(dir, rel)
		os.MkdirAll(filepath.Dir(p), 0o755)
		os.WriteFile(p, []byte("x"), 0o644)
		at := since.Add(time.Duration(i) * time.Second)
		os.Chtimes(p, at, at)
	}
	d := findDumps(dir, since)
	if len(d.Video) != 2 || filepath.Base(d.Video[0]) != "pixelu_framedump0.mkv" || filepath.Base(d.Video[1]) != "pixelu_framedump1.mkv" {
		t.Errorf("video: %v", d.Video)
	}
	if len(d.DSP) != 1 || filepath.Base(d.DSP[0]) != "pixelu_dspdump.wav" || len(d.DTK) != 0 {
		t.Errorf("sound: %v %v", d.DSP, d.DTK)
	}
}

// Recording a Wii U game: PixelU is asked to save the game into a folder of
// its own, and the result is one high-quality .mp4 of the whole picture.
func TestPixelURecordEndToEnd(t *testing.T) {
	ff := testFFmpeg(t)
	argsFile, game := setupFakePixelU(t)
	cfgMu.Lock()
	on := true
	cfg.PixelURecord = &on
	cfgMu.Unlock()
	if err := launchPixelUGame(game); err != nil {
		t.Fatal(err)
	}
	if st := recStatus(); !st["dumping"].(bool) {
		t.Fatalf("not recording: %v", st)
	}
	waitIdle(t)
	args, _ := os.ReadFile(argsFile)
	tmp := filepath.Join(recordingsDir(), "temp")
	if !strings.Contains(string(args), "-g "+game+" -f --rp-record "+filepath.Join(tmp, "PixelU-")) ||
		!strings.HasSuffix(strings.TrimSpace(string(args)), "--rp-ffmpeg "+ff) {
		t.Fatalf("args: %s", args)
	}
	st := recStatus()
	if st["error"] != "" || !strings.HasPrefix(st["message"].(string), "Saved Hyrule Warriors") {
		t.Fatalf("status: %v", st)
	}
	files, _ := filepath.Glob(filepath.Join(recordingsDir(), "*.mp4"))
	if len(files) != 1 {
		t.Fatalf("want 1 recording, got %v", files)
	}
	size, dur, audio := probe(t, ff, files[0])
	t.Logf("%s: %s %.2fs audio=%v", filepath.Base(files[0]), size, dur, audio)
	if size != "1280x720" || !audio || dur < 3.8 || dur > 4.3 {
		t.Fatalf("bad recording: %s %.2fs audio=%v", size, dur, audio)
	}
	if left, _ := os.ReadDir(tmp); len(left) != 0 {
		t.Fatalf("raw files left behind: %v", left)
	}
	log, _ := os.ReadFile(recLogPath())
	if !strings.Contains(string(log), "PixelU saves the game to") || strings.Contains(string(log), "bars=&") ||
		!strings.Contains(string(log), "-crf 18") {
		t.Errorf("log:\n%s", log)
	}
}

// Without ffmpeg the game still starts, unrecorded, and ffmpeg is fetched.
func TestPixelURecordNoFFmpeg(t *testing.T) {
	argsFile, game := setupFakePixelU(t)
	t.Setenv("PATH", t.TempDir())
	t.Setenv("RETRO_FFMPEG", "")
	if findFFmpeg() != "" {
		t.Skip("an ffmpeg next to the test binary")
	}
	ffOld := ffInstallF
	ffInstallF = func() error { return nil }
	defer func() { ffInstallF = ffOld; setFF("idle", nil) }()
	setManualWanted(true)
	if err := launchPixelUGame(game); err != nil {
		t.Fatal(err)
	}
	waitIdle(t)
	args, _ := os.ReadFile(argsFile)
	if strings.Contains(string(args), "--rp-record") || strings.Contains(string(args), "--rp-ffmpeg") {
		t.Errorf("asked to record without ffmpeg: %s", args)
	}
	if e, _ := recStatus()["error"].(string); !strings.Contains(e, "needs ffmpeg") {
		t.Errorf("error: %q", e)
	}
}

func TestPixelUEmbedDefaultsOn(t *testing.T) {
	cfgMu.Lock()
	defer cfgMu.Unlock()
	old := cfg
	defer func() { cfg = old }()
	cfg = config{}
	if !pixeluEmbedOn() {
		t.Error("Wii U games should play inside RetroPixels unless turned off")
	}
}
