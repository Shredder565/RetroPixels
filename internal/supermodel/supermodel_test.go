package supermodel

import (
	"archive/zip"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"
)

// writeZip makes a .zip whose directory lists files with the given CRC-32s
// (the contents are a stand-in: only the directory is ever read).
func writeZip(t *testing.T, path string, files []romFile) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		t.Fatal(err)
	}
	f, err := os.Create(path)
	if err != nil {
		t.Fatal(err)
	}
	zw := zip.NewWriter(f)
	for _, rf := range files {
		data := []byte("x")
		w, err := zw.CreateRaw(&zip.FileHeader{Name: rf.Name, Method: zip.Store, CRC32: rf.CRC32,
			CompressedSize64: uint64(len(data)), UncompressedSize64: uint64(len(data))})
		if err != nil {
			t.Fatal(err)
		}
		w.Write(data)
	}
	if err := zw.Close(); err != nil {
		t.Fatal(err)
	}
	f.Close()
}

func filesOf(regions []romRegion, withOptional bool) []romFile {
	var out []romFile
	for _, r := range regions {
		if r.Required || withOptional {
			out = append(out, r.Files...)
		}
	}
	return out
}

// mergedA is a merged (or non-merged) swtrilgya.zip as MAME makes it: the
// clone's own program ROMs plus everything else from the parent.
func mergedA(withDrive bool) []romFile { return filesOf(swtrilgya.merged(), withDrive) }

func TestCheckROMMergedClone(t *testing.T) {
	dir := t.TempDir()
	p := filepath.Join(dir, "swtrilgya.zip")
	writeZip(t, p, mergedA(true))
	c := CheckROM(p)
	if !c.OK || c.Set != "swtrilgya" || !c.DriveBoard || c.Parent != "" || c.Problem != "" {
		t.Fatalf("merged swtrilgya: %+v", c)
	}
	writeZip(t, p, mergedA(false))
	if c := CheckROM(p); !c.OK || c.DriveBoard {
		t.Fatalf("without the drive board ROM it still plays, without force feedback: %+v", c)
	}
}

func TestCheckROMSplitClone(t *testing.T) {
	dir := t.TempDir()
	p := filepath.Join(dir, "swtrilgya.zip")
	writeZip(t, p, swtrilgya.Regions[0].Files)
	c := CheckROM(p)
	if c.OK || !c.NeedsParent || c.Set != "swtrilgya" || !strings.Contains(c.Problem, "swtrilgy.zip") {
		t.Fatalf("split swtrilgya alone: %+v", c)
	}
	// With the parent next to it, Supermodel loads both.
	writeZip(t, filepath.Join(dir, "swtrilgy.zip"), filesOf(swtrilgy.Regions, true))
	c = CheckROM(p)
	if !c.OK || c.Parent != filepath.Join(dir, "swtrilgy.zip") || !c.DriveBoard {
		t.Fatalf("split swtrilgya with its parent: %+v", c)
	}
}

func TestCheckROMParentAndBroken(t *testing.T) {
	dir := t.TempDir()
	p := filepath.Join(dir, "swtrilgy.zip")
	writeZip(t, p, filesOf(swtrilgy.Regions, false))
	if c := CheckROM(p); !c.OK || c.Set != "swtrilgy" {
		t.Fatalf("swtrilgy: %+v", c)
	}
	// Two sound ROMs missing.
	files := filesOf(swtrilgy.Regions, false)
	var short []romFile
	for _, f := range files {
		if f.Name != "mpr-21355.22" && f.Name != "mpr-21357.24" {
			short = append(short, f)
		}
	}
	writeZip(t, p, short)
	c := CheckROM(p)
	if c.OK || c.MissingN != 2 || !reflect.DeepEqual(c.Missing, []string{"mpr-21355.22", "mpr-21357.24"}) || !strings.Contains(c.Problem, "2 of the game's files") {
		t.Fatalf("incomplete swtrilgy: %+v", c)
	}
	// Some other game.
	other := filepath.Join(dir, "tmnt.zip")
	writeZip(t, other, []romFile{{"a.bin", 1}, {"b.bin", 2}})
	if c := CheckROM(other); c.OK || c.Set != "" || !strings.Contains(c.Problem, "isn't Star Wars") {
		t.Fatalf("another game: %+v", c)
	}
	bad := filepath.Join(dir, "swtrilgya.zip")
	os.WriteFile(bad, []byte("not a zip"), 0o644)
	if c := CheckROM(bad); c.OK || !strings.Contains(c.Problem, "can't be read") {
		t.Fatalf("not a zip: %+v", c)
	}
}

func TestFindROM(t *testing.T) {
	root := t.TempDir()
	if p, _ := FindROM([]string{root, ""}); p != "" {
		t.Fatalf("nothing there, found %q", p)
	}
	// A broken copy in one folder, a good one deeper in another.
	broken := filepath.Join(root, "a", "swtrilgya.zip")
	writeZip(t, broken, swtrilgya.Regions[0].Files)
	good := filepath.Join(root, "b", "MAME", "roms", "SWTRILGYA.ZIP")
	writeZip(t, good, mergedA(true))
	writeZip(t, filepath.Join(root, "b", "MAME", "roms", "tmnt.zip"), []romFile{{"x", 3}})
	p, c := FindROM([]string{filepath.Join(root, "a"), filepath.Join(root, "b")})
	if p != good || !c.OK {
		t.Fatalf("FindROM = %q %+v, want %q", p, c, good)
	}
	// Only the broken one: it's returned with what's wrong.
	p, c = FindROM([]string{filepath.Join(root, "a")})
	if p != broken || c.OK || !c.NeedsParent {
		t.Fatalf("FindROM broken = %q %+v", p, c)
	}
	if !IsRomFile("SwTrilgy.zip") || IsRomFile("swtrilgyp.zip") || IsRomFile("swtrilgya.7z") {
		t.Fatal("IsRomFile")
	}
}

func TestGameArgs(t *testing.T) {
	rom := filepath.Join("C:", "ROMs", "swtrilgya.zip")
	docked := GameArgs(rom, LaunchOptions{Embed: true, Hidden: true, ForceFeedback: true, Patched: true,
		RecordDir: filepath.Join("C:", "rec"), FFmpeg: filepath.Join("C:", "ffmpeg.exe")})
	want := []string{rom, "-window", "-borderless", "-window-pos=-20000,-20000", "-no-wide-screen", "-force-feedback",
		"-input-system=sdlgamepad", "-rp-record=" + filepath.Join("C:", "rec"), "-rp-ffmpeg=" + filepath.Join("C:", "ffmpeg.exe")}
	if !reflect.DeepEqual(docked, want) {
		t.Fatalf("docked:\n got %q\nwant %q", docked, want)
	}
	full := GameArgs(rom, LaunchOptions{Fullscreen: true, WideScreen: true, Patched: true})
	if !reflect.DeepEqual(full, []string{rom, "-fullscreen", "-desktop-res", "-wide-screen", "-input-system=sdlgamepad"}) {
		t.Fatalf("fullscreen: %q", full)
	}
	// The official build knows none of RetroPixels' options.
	off := GameArgs(rom, LaunchOptions{Fullscreen: true, RecordDir: "r", FFmpeg: "f"})
	if strings.Contains(strings.Join(off, " "), "-rp-") || strings.Contains(strings.Join(off, " "), "-desktop-res") {
		t.Fatalf("official build got RetroPixels options: %q", off)
	}
	win := GameArgs(rom, LaunchOptions{})
	if win[1] != "-window" || win[2] != "-res=992,768" {
		t.Fatalf("window: %q", win)
	}
}

func TestINI(t *testing.T) {
	s := INI(Controls{})
	for _, want := range []string{
		iniMarker, "[ Global ]", `InputSystem = "sdlgamepad"`,
		`InputStart1 = "KEY_1,JOY1_BUTTON8"`, `InputCoin1 = "KEY_5,KEY_3,JOY1_BUTTON7"`,
		`InputAnalogJoyX = "JOY1_XAXIS"`, `InputAnalogJoyY = "JOY1_YAXIS"`,
		`InputAnalogJoyTrigger = "KEY_LEFTCTRL,KEY_A,JOY1_BUTTON1,JOY1_RZAXIS_POS"`,
		`InputUIExit = "KEY_ESCAPE,JOY1_BUTTON7+JOY1_BUTTON8"`,
		"[ swtrilgya ]", "[ swtrilgy ]", `MusicVolume = "70"`,
	} {
		if !strings.Contains(s, want) {
			t.Errorf("INI is missing %q", want)
		}
	}
	if !strings.HasPrefix(s, iniMarker) {
		t.Error("the marker must be the first line")
	}
	if !strings.Contains(INI(Controls{InvertY: true}), `InputAnalogJoyY = "JOY1_YAXIS_INV"`) {
		t.Error("InvertY")
	}
	// Every key Supermodel reads is set once in the global section.
	seen := map[string]bool{}
	for _, l := range strings.Split(strings.SplitN(s, "[ swtrilgya ]", 2)[0], "\n") {
		if k, _, ok := strings.Cut(l, " = "); ok {
			if seen[k] {
				t.Errorf("%s set twice", k)
			}
			seen[k] = true
		}
	}
}

func TestWriteINI(t *testing.T) {
	p := filepath.Join(t.TempDir(), "Config", "Supermodel.ini")
	if wrote, err := WriteINI(p, Controls{}); !wrote || err != nil {
		t.Fatalf("first write: %v %v", wrote, err)
	}
	b, _ := os.ReadFile(p)
	if !strings.Contains(string(b), "\r\n") || !Ours(p) {
		t.Fatal("written with Windows line endings, and known as RetroPixels' own")
	}
	if wrote, _ := WriteINI(p, Controls{InvertY: true}); !wrote {
		t.Fatal("RetroPixels' own file is written again")
	}
	if b, _ := os.ReadFile(p); !strings.Contains(string(b), "JOY1_YAXIS_INV") {
		t.Fatal("the new setting is in it")
	}
	// The player's own settings (Supermodel's input setup rewrote the file).
	os.WriteFile(p, []byte("[ Global ]\nInputStart1 = \"KEY_ENTER\"\n"), 0o644)
	if wrote, err := WriteINI(p, Controls{}); wrote || err != nil || Ours(p) {
		t.Fatalf("the player's file was replaced: %v %v", wrote, err)
	}
}

func TestFindExe(t *testing.T) {
	app := t.TempDir()
	home := t.TempDir()
	if got := FindExe("", app, home); got != "" {
		t.Fatalf("nothing there, found %q", got)
	}
	dl := filepath.Join(home, "Downloads", "supermodel-0.3a-20260726-git-24d2ffc-windows", ExeName)
	os.MkdirAll(filepath.Dir(dl), 0o755)
	os.WriteFile(dl, nil, 0o755)
	if got := FindExe("", app, home); got != dl {
		t.Fatalf("Downloads: %q", got)
	}
	own := filepath.Join(app, "Supermodel", ExeName)
	os.MkdirAll(filepath.Dir(own), 0o755)
	os.WriteFile(own, nil, 0o755)
	if got := FindExe("", app, home); got != own {
		t.Fatalf("the one next to RetroPixels comes first: %q", got)
	}
	if got := FindExe(dl, app, home); got != dl {
		t.Fatalf("the chosen one: %q", got)
	}
	if got := FindExe(filepath.Join(app, "gone.exe"), app, home); got != own {
		t.Fatalf("a chosen one that's gone: %q", got)
	}
	if Patched(own) {
		t.Fatal("no marker yet")
	}
	os.WriteFile(filepath.Join(app, "Supermodel", PatchedMarker), nil, 0o644)
	if !Patched(own) {
		t.Fatal("RetroPixels' build")
	}
	home2 := filepath.Join(home, "data")
	if got := NVRAMFile(home2, own, "swtrilgya"); got != filepath.Join(home2, "NVRAM", "swtrilgya.nv") {
		t.Fatalf("NVRAM: %q", got)
	}
	if got := ConfigFile(home2, dl); got != filepath.Join(filepath.Dir(dl), "Config", "Supermodel.ini") {
		t.Fatalf("official build's config: %q", got)
	}
}

func TestProblem(t *testing.T) {
	for out, want := range map[string]string{
		"Error: Expected to find parent ROM set of 'swtrilgya' at 'C:/roms/swtrilgy.zip'.\n":                                                                                       "also needs swtrilgy.zip",
		"Error: 'mpr-21359.26' (CRC32 0x34ef4122) not found in 'x.zip' for game 'swtrilgy'.\nError: 'mpr-21360.27' (CRC32 0x2882b95e) not found in 'x.zip' for game 'swtrilgy'.\n": "(2 missing or different)",
		"Error: Unable to create an OpenGL display: Couldn't find matching GLX visual\n":                                                                                           "OpenGL 4.1",
		"Error: Unable to open 44.1KHz 4-channel audio with SDL: no device\n":                                                                                                      "sound device",
		"":     "",
		"a\nb": "a b",
	} {
		if got := Problem(out); !strings.Contains(got, want) || (want == "" && got != "") {
			t.Errorf("Problem(%q) = %q, want it to say %q", out, got, want)
		}
	}
}

func TestOutputKeepsTheEnd(t *testing.T) {
	var o Output
	o.Write([]byte(strings.Repeat("a", outputKeep)))
	o.Write([]byte("end"))
	if s := o.String(); len(s) != outputKeep || !strings.HasSuffix(s, "end") {
		t.Fatalf("kept %d bytes", len(s))
	}
}

func TestCommand(t *testing.T) {
	exe := filepath.Join(t.TempDir(), "Supermodel", ExeName)
	cmd, out := Command(exe, "/data/Supermodel", "swtrilgya.zip", "-window")
	if cmd.Dir != filepath.Dir(exe) || out == nil {
		t.Fatalf("dir %q", cmd.Dir)
	}
	found := false
	for _, e := range cmd.Env {
		found = found || e == "SUPERMODEL_HOME=/data/Supermodel"
	}
	if !found {
		t.Fatal("SUPERMODEL_HOME isn't passed")
	}
	if !reflect.DeepEqual(cmd.Args[1:], []string{"swtrilgya.zip", "-window"}) {
		t.Fatalf("args %q", cmd.Args)
	}
}
