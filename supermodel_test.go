//go:build !windows

package main

import (
	"archive/zip"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"retropixels/internal/supermodel"
)

// fakeSupermodel is a stand-in supermodel.exe: it notes its arguments and
// SUPERMODEL_HOME and, when asked to record, writes what RetroPixels'
// Supermodel does: the window's picture (a 4:3 game with black bars beside
// it) as .mkv and the sound as a 44.1 kHz .wav. FAKE_FAIL makes it say what
// Supermodel says about a split ROM set and quit.
const fakeSupermodel = `#!/bin/sh
echo "$@" >> "$FAKE_ARGS"
echo "HOME=$SUPERMODEL_HOME" >> "$FAKE_ARGS"
if [ -n "$FAKE_FAIL" ]; then
  echo "Error: Expected to find parent ROM set of 'swtrilgya' at 'roms/swtrilgy.zip'."
  exit 1
fi
D=""; FFX=""
for a in "$@"; do
  case "$a" in -rp-record=*) D="${a#-rp-record=}" ;; -rp-ffmpeg=*) FFX="${a#-rp-ffmpeg=}" ;; esac
done
[ -n "$D" ] || { sleep "${FAKE_PLAY:-1}"; exit 0; }
FF="$FFX -hide_banner -loglevel error -y"
$FF -f lavfi -i "sine=frequency=440:sample_rate=44100:duration=3" -ac 2 -c:a pcm_s16le "$D/Audio/supermodel_dspdump.wav"
$FF -f lavfi -i "testsrc2=size=960x720:rate=60:duration=3" -vf "pad=1280:720:160:0:black" -c:v libx264 -g 10 -crf 14 -pix_fmt yuv420p "$D/Frames/supermodel_framedump0.mkv"
sleep "${FAKE_PLAY:-1}"
`

func writeTestZip(t *testing.T, path string, files []supermodel.ROMFile) {
	t.Helper()
	os.MkdirAll(filepath.Dir(path), 0o755)
	f, err := os.Create(path)
	if err != nil {
		t.Fatal(err)
	}
	zw := zip.NewWriter(f)
	for _, rf := range files {
		w, err := zw.CreateRaw(&zip.FileHeader{Name: rf.Name, Method: zip.Store, CRC32: rf.CRC32, CompressedSize64: 1, UncompressedSize64: 1})
		if err != nil {
			t.Fatal(err)
		}
		w.Write([]byte("x"))
	}
	zw.Close()
	f.Close()
}

// setupFakeSupermodel: RetroPixels' own build next to it, the game in a
// MAME ROM folder, nothing remembered yet.
func setupFakeSupermodel(t *testing.T, patched bool) (exe, rom, argsFile, home string) {
	t.Helper()
	dir := t.TempDir()
	exe = filepath.Join(dir, "Supermodel", supermodel.ExeName)
	os.MkdirAll(filepath.Dir(exe), 0o755)
	os.WriteFile(exe, []byte(fakeSupermodel), 0o755)
	if patched {
		os.WriteFile(filepath.Join(dir, "Supermodel", supermodel.PatchedMarker), []byte("modified"), 0o644)
	}
	roms := filepath.Join(dir, "MAME roms")
	rom = filepath.Join(roms, "swtrilgya.zip")
	writeTestZip(t, rom, supermodel.SetFiles("swtrilgya", true))
	argsFile = filepath.Join(dir, "args")
	home = filepath.Join(dir, "home", "Supermodel")
	mameCfg := filepath.Join(dir, "cfg", "mame.json")
	os.MkdirAll(filepath.Dir(mameCfg), 0o755)
	os.WriteFile(mameCfg, []byte(`{"romFolders":[`+jsonString(roms)+`],"detected":true}`), 0o644)
	for k, v := range map[string]string{"RETRO_MAME_CONFIG": mameCfg, "RETRO_SUPERMODEL_HOME": home, "FAKE_ARGS": argsFile,
		"RETRO_REC_DIR": t.TempDir(), "XDG_CONFIG_HOME": t.TempDir(), "HOME": t.TempDir(), "RETRO_MAME_BUNDLE": filepath.Join(dir, "none"),
		"FAKE_FAIL": "", "FAKE_PLAY": "1"} {
		t.Setenv(k, v)
	}
	off := false
	cfgMu.Lock()
	cfg = config{SupermodelExe: exe, SupermodelEmbed: &off, AutoRecord: &off} // docking needs Windows
	cfgMu.Unlock()
	smMu.Lock()
	smFinding, smSearched, smCheck, smProblem = false, false, nil, nil
	smMu.Unlock()
	setManualWanted(false)
	recMu.Lock()
	recLastErr, recLastMsg = "", ""
	recMu.Unlock()
	return
}

func jsonString(s string) string {
	return `"` + strings.ReplaceAll(strings.ReplaceAll(s, `\`, `\\`), `"`, `\"`) + `"`
}

func waitFound(t *testing.T) {
	t.Helper()
	for i := 0; i < 200; i++ {
		smMu.Lock()
		done := smSearched && !smFinding
		smMu.Unlock()
		if done {
			return
		}
		time.Sleep(20 * time.Millisecond)
	}
	t.Fatal("never finished looking for the game")
}

func TestSupermodelFindsTheGameOnceAndPlaysIt(t *testing.T) {
	exe, rom, argsFile, home := setupFakeSupermodel(t, true)
	if err := launchSupermodel(); err != errNoSupermodelROM {
		t.Fatalf("before the game is found: %v", err)
	}
	// Opening the tab looks for it in the folders RetroPixels knows.
	st := supermodelStatus()
	if st["rom"] != "" || st["exe"] != exe || st["patched"] != true {
		t.Fatalf("status before: %v", st)
	}
	supermodelFind(false)
	waitFound(t)
	st = supermodelStatus()
	c := st["check"].(*supermodel.Check)
	if st["rom"] != rom || !c.OK || c.Set != "swtrilgya" || !c.DriveBoard {
		t.Fatalf("status after: %v %+v", st, c)
	}
	cfgMu.Lock()
	remembered := cfg.SupermodelROM
	cfgMu.Unlock()
	if remembered != rom {
		t.Fatalf("not remembered: %q", remembered)
	}

	// Clicking the tile: fullscreen in Supermodel's own window (docking is
	// Windows-only), force feedback on (the set has its program).
	if err := launchSupermodel(); err != nil {
		t.Fatal(err)
	}
	if err := launchSupermodel(); err == nil {
		t.Error("started a second game while one is running")
	}
	waitIdle(t)
	args, _ := os.ReadFile(argsFile)
	want := rom + " -fullscreen -desktop-res -no-wide-screen -force-feedback -input-system=sdlgamepad\nHOME=" + home
	if got := strings.TrimSpace(string(args)); got != want {
		t.Fatalf("args:\n got %q\nwant %q", got, want)
	}
	ini, err := os.ReadFile(filepath.Join(home, "Config", "Supermodel.ini"))
	if err != nil || !strings.Contains(string(ini), `InputAnalogJoyX = "JOY1_XAXIS"`) {
		t.Fatalf("Supermodel.ini: %v\n%s", err, ini)
	}
	cfgMu.Lock()
	played := cfg.SupermodelPlayed
	cfg.SupermodelWide, cfg.SupermodelInvertY, cfg.SupermodelFullscreen = true, true, new(bool)
	cfgMu.Unlock()
	if played.IsZero() {
		t.Error("when it was played isn't kept")
	}

	// The settings: windowed, wide screen, flight-style stick.
	os.Remove(argsFile)
	if err := launchSupermodel(); err != nil {
		t.Fatal(err)
	}
	waitIdle(t)
	args, _ = os.ReadFile(argsFile)
	if !strings.HasPrefix(string(args), rom+" -window -res=992,768 -wide-screen -force-feedback") {
		t.Fatalf("windowed: %q", args)
	}
	if ini, _ := os.ReadFile(filepath.Join(home, "Config", "Supermodel.ini")); !strings.Contains(string(ini), "JOY1_YAXIS_INV") {
		t.Fatal("the flight-style setting isn't in Supermodel.ini")
	}
}

func TestSupermodelSaysWhyItDidntStart(t *testing.T) {
	_, rom, _, _ := setupFakeSupermodel(t, true)
	if err := useSupermodelROM(rom); err != nil {
		t.Fatal(err)
	}
	t.Setenv("FAKE_FAIL", "1")
	if err := launchSupermodel(); err != nil {
		t.Fatal(err)
	}
	waitIdle(t)
	time.Sleep(100 * time.Millisecond)
	smMu.Lock()
	p := smProblem
	smMu.Unlock()
	if p == nil || !strings.Contains(p["text"].(string), "swtrilgy.zip") {
		t.Fatalf("problem: %v", p)
	}

	// A file that isn't the game can't be chosen; a split set is refused
	// before Supermodel starts.
	other := filepath.Join(t.TempDir(), "tmnt.zip")
	writeTestZip(t, other, []supermodel.ROMFile{{Name: "a", CRC32: 1}})
	if err := useSupermodelROM(other); err == nil || !strings.Contains(err.Error(), "isn't Star Wars") {
		t.Fatalf("another game: %v", err)
	}
	split := filepath.Join(t.TempDir(), "swtrilgya.zip")
	files := supermodel.SetFiles("swtrilgya", false)[:4] // only the clone's own program ROMs
	writeTestZip(t, split, files)
	if err := useSupermodelROM(split); err != nil {
		t.Fatal(err)
	}
	if err := launchSupermodel(); err == nil || !strings.Contains(err.Error(), "swtrilgy.zip") {
		t.Fatalf("split set: %v", err)
	}
}

func TestSupermodelOfficialBuild(t *testing.T) {
	exe, rom, argsFile, home := setupFakeSupermodel(t, false)
	if err := useSupermodelROM(rom); err != nil {
		t.Fatal(err)
	}
	on := true
	cfgMu.Lock()
	cfg.SupermodelEmbed, cfg.AutoRecord = &on, &on // neither works with the official build
	cfgMu.Unlock()
	if err := launchSupermodel(); err != nil {
		t.Fatal(err)
	}
	waitIdle(t)
	args, _ := os.ReadFile(argsFile)
	if !strings.HasPrefix(string(args), rom+" -fullscreen -res=1920,1080 ") || strings.Contains(string(args), "-rp-") {
		t.Fatalf("official build: %q", args)
	}
	// Its settings live in its own Config folder.
	if _, err := os.Stat(filepath.Join(filepath.Dir(exe), "Config", "Supermodel.ini")); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(filepath.Join(home, "Config", "Supermodel.ini")); err == nil {
		t.Fatal("written to the RetroPixels folder, which the official build doesn't read")
	}
}

// Recording: RetroPixels' Supermodel saves the game into a folder of its
// own, and the result is one .mp4 without the black bars.
func TestSupermodelRecordEndToEnd(t *testing.T) {
	ff := testFFmpeg(t)
	_, rom, argsFile, _ := setupFakeSupermodel(t, true)
	if err := useSupermodelROM(rom); err != nil {
		t.Fatal(err)
	}
	on := true
	cfgMu.Lock()
	cfg.AutoRecord = &on
	cfgMu.Unlock()
	if err := launchSupermodel(); err != nil {
		t.Fatal(err)
	}
	if st := recStatus(); !st["dumping"].(bool) {
		t.Fatalf("not recording: %v", st)
	}
	waitIdle(t)
	args, _ := os.ReadFile(argsFile)
	tmp := filepath.Join(recordingsDir(), "temp")
	if !strings.Contains(string(args), "-rp-record="+filepath.Join(tmp, "Supermodel-")) || !strings.Contains(string(args), "-rp-ffmpeg="+ff) {
		t.Fatalf("args: %s", args)
	}
	st := recStatus()
	if st["error"] != "" || !strings.HasPrefix(st["message"].(string), "Saved Star Wars Trilogy Arcade") {
		t.Fatalf("status: %v", st)
	}
	files, _ := filepath.Glob(filepath.Join(recordingsDir(), "*.mp4"))
	if len(files) != 1 {
		t.Fatalf("want 1 recording, got %v", files)
	}
	size, dur, audio := probe(t, ff, files[0])
	t.Logf("%s: %s %.2fs audio=%v", filepath.Base(files[0]), size, dur, audio)
	if size != "960x720" || !audio || dur < 2.7 || dur > 3.3 {
		t.Fatalf("bad recording: %s %.2fs audio=%v", size, dur, audio)
	}
	if left, _ := os.ReadDir(tmp); len(left) != 0 {
		t.Fatalf("raw files left behind: %v", left)
	}
}
