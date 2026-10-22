package dolphin

import (
	"encoding/binary"
	"encoding/hex"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
	"time"
)

// Header bytes as real game files have them (the same as RetroPixels.exe's tests).
var realHeaders = map[string]map[int64]string{
	"- Mario Games/013 - Super Mario Sunshine (USA).ciso": {
		0:      "4349534f0000200001010100",
		0x8000: "474d5345303100000000000000000000000000000000000000000000c2339f3d5375706572204d6172696f2053756e7368696e6500000000000000000000000000000000",
	},
	"Sonic Games/Gamecube- Sonic Heroes .iso": {
		0: "47395345385000000100000000000000000000000000000000000000c2339f3d534f4e4943204845524f455300000000000000000000000000000000000000000000000000",
	},
	"Sonic Games/Gamecube- Sonic Adventure 2 - Battle .iso": {
		0: "47534e45385000000100000000000000000000000000000000000000c2339f3d534f4e494320414456454e5455524520322042415454424c450000000000000000000000",
	},
	"Wii/WII   - Wii Sports [RSPE01].wbfs": {
		0:     "57424653000a100009150000010000",
		0x200: "5253504530310001000000000000000000000000000000005d1c9ea30000000053504f525453205041434b20666f72205245564f4c5554494f4e000000000000000000",
	},
	"Wii/WII   - Super Smash Bros. BrawlRSBE01.wbfs": {
		0:     "5742465300e9800009150000010000",
		0x200: "5253424530310002000000000000000000000000000000005d1c9ea300000000537570657220536d6173682042726f732e20427261776c0000000000000000000000",
	},
	"Wii/WAD   - Mega Man 9 (USA) (WiiWare).wad": {
		0: "000000204973000000000a0000000000000002a40000037000f1b140",
	},
	"Wii/WAD   - Mega Man 9 (USA) (WiiWare) (DLC).wad": {
		0: "000000204973000000000a0000000000000002a40000037000f1b140",
	},
	"- Turtle Games/TMNT (USA) (En,Fr,Es)/TMNT (USA) (En,Fr,Es).iso": {
		0:      "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd",
		0x8000: "0143443030310100504c415953544154494f4e",
	},
	"- Turtle Games/CD - TMNT_Win_ISO_EN/OGD_TMNT_ISO.iso": {0x8000: "014344303031010057494e3332"},
	"Wii U/WII  U -Hyrule Warriors (US).wua":               {0: "00"},
	"002 NES Complete/Super Mario Bros.nes":                {0: "4e45531a"},
}

func makeRoms(t *testing.T) string {
	root := filepath.Join(t.TempDir(), "Roms NES-switch")
	for name, parts := range realHeaders {
		p := filepath.Join(root, filepath.FromSlash(name))
		os.MkdirAll(filepath.Dir(p), 0o755)
		f, _ := os.Create(p)
		for off, h := range parts {
			b, _ := hex.DecodeString(h)
			f.WriteAt(b, off)
		}
		f.WriteAt([]byte{0}, 0x9000)
		f.Close()
	}
	os.MkdirAll(filepath.Join(root, "Wii", "- Wii", "wii"), 0o755)
	os.WriteFile(filepath.Join(root, "Wii", "- Wii", "wii", "PixelU-Wii.exe"), []byte("stand-in"), 0o755)
	return root
}

func TestLibrary(t *testing.T) {
	root := makeRoms(t)
	// Games folders are guessed from near the engine (people keep emulators next to their games).
	folders := GuessFolders(filepath.Join(root, "Wii", "- Wii", "wii", "PixelU-Wii.exe"))
	if len(folders) != 3 {
		t.Fatalf("want 3 folders (Mario, Sonic, Wii), got %v", folders)
	}
	played := time.Date(2026, 10, 1, 12, 0, 0, 0, time.UTC)
	sunshine := filepath.Join(root, "- Mario Games", "013 - Super Mario Sunshine (USA).ciso")
	got := map[string]string{}
	for _, g := range Library(folders, map[string]time.Time{strings.ToLower(sunshine): played}) {
		got[g.Name] = g.Platform + " " + g.ID
		if g.Name == "Super Mario Sunshine" && !g.Played.Equal(played) {
			t.Errorf("played time not carried: %v", g.Played)
		}
	}
	want := map[string]string{
		"Super Mario Sunshine":      "GameCube GMSE01",
		"Sonic Heroes":              "GameCube G9SE8P",
		"Sonic Adventure 2: Battle": "GameCube GSNE8P",
		"Wii Sports":                "Wii RSPE01",
		"Super Smash Bros. Brawl":   "Wii RSBE01",
		"Mega Man 9":                "WiiWare",
	}
	for k, v := range want {
		if g, ok := got[k]; !ok || !strings.HasPrefix(g, v) {
			t.Errorf("missing/wrong %q: got %q want %q", k, g, v)
		}
	}
	if len(got) != len(want) {
		t.Errorf("got %d games %v, want %d (PC/PS2 discs, DLC and Wii U must be left out)", len(got), got, len(want))
	}
	if g, ok := GameFor(sunshine); !ok || g.Name != "Super Mario Sunshine" || g.Platform != "GameCube" {
		t.Errorf("GameFor = %+v %v", g, ok)
	}
	if _, ok := GameFor(filepath.Join(root, "- Turtle Games", "CD - TMNT_Win_ISO_EN", "OGD_TMNT_ISO.iso")); ok {
		t.Error("a PC disc passed as a GameCube game")
	}
	if !IsUnder(filepath.Join(root, "Wii", "x"), folders) || IsUnder(root, folders) || !ContainsFold(folders, strings.ToUpper(folders[0])) {
		t.Error("IsUnder / ContainsFold")
	}
}

func TestVersion(t *testing.T) {
	p := filepath.Join(t.TempDir(), "wii", "PixelU-Wii.exe")
	os.MkdirAll(filepath.Dir(p), 0o755)
	os.WriteFile(p, []byte("x"), 0o644)
	if v := Version(p); v != "" {
		t.Fatalf("no build_info.txt: %q", v)
	}
	os.WriteFile(filepath.Join(filepath.Dir(p), "build_info.txt"), []byte("Some line\nVersion 2609a\n"), 0o644)
	if v := Version(p); v != "Version 2609a" {
		t.Fatalf("Version = %q", v)
	}
}

// The engine is told by its own file name; Dolphin.exe isn't it.
func TestIsProgram(t *testing.T) {
	exe := `C:\Games\PixelU\wii\PixelU-Wii.exe`
	for path, want := range map[string]bool{
		`C:\Games\PixelU\wii\PixelU-Wii.exe`: true,
		`D:\Other\pixelu-wii.EXE`:            true, // a second copy is the same program
		"PixelU-Wii.exe":                     true,
		"/home/me/PixelU/wii/PixelU-Wii.exe": true,
		`C:\Dolphin\Dolphin.exe`:             false,
		`C:\Games\PixelU\PixelU.exe`:         false,
		"":                                   false,
	} {
		if got := IsProgram(path, exe); got != want {
			t.Errorf("IsProgram(%q) = %v", path, got)
		}
	}
	if IsProgram("PixelU-Wii.exe", "") || IsProgram(".", ".") {
		t.Error("no engine: nothing matches")
	}
}

// The old Dolphin's folder, for bringing its settings over to the engine.
func TestLegacyUserDir(t *testing.T) {
	t.Setenv("RETRO_OLD_DOLPHIN_USER", "")
	home, ad := t.TempDir(), t.TempDir()
	t.Setenv("HOME", home)
	t.Setenv("USERPROFILE", home)
	t.Setenv("APPDATA", ad)
	if got := legacyUserDirFor("windows", ""); got != "" {
		t.Fatalf("no old Dolphin: %q", got)
	}
	os.MkdirAll(filepath.Join(ad, "Dolphin Emulator"), 0o755)
	if got := legacyUserDirFor("windows", ""); got != filepath.Join(ad, "Dolphin Emulator") {
		t.Errorf("AppData: %q", got)
	}
	docs := filepath.Join(home, "Documents", "Dolphin Emulator")
	os.MkdirAll(docs, 0o755)
	if got := legacyUserDirFor("windows", ""); got != docs {
		t.Errorf("Documents: %q", got)
	}
	old := filepath.Join(t.TempDir(), "Dolphin-x64", "Dolphin.exe")
	os.MkdirAll(filepath.Join(filepath.Dir(old), "User"), 0o755)
	if got := legacyUserDirFor("windows", old); got != docs {
		t.Errorf("not portable: %q", got)
	}
	os.WriteFile(filepath.Join(filepath.Dir(old), "portable.txt"), nil, 0o644)
	if got := legacyUserDirFor("windows", old); got != filepath.Join(filepath.Dir(old), "User") {
		t.Errorf("portable: %q", got)
	}
	t.Setenv("RETRO_OLD_DOLPHIN_USER", "old")
	if legacyUserDirFor("windows", old) != "old" {
		t.Error("override")
	}
}

func TestNames(t *testing.T) {
	for f, want := range map[string]string{"WII   - Punch-Out! [R7PE01].wbfs": "Punch-Out!", "hi-temunitu.iso": "Some Game", "GMSE01.iso": "Some Game", "Cuphead.iso": "Cuphead"} {
		if got := DisplayName(f, Disc{ID: "R7PE01", Title: "SOME GAME"}); got != want {
			t.Errorf("%s -> %q want %q", f, got, want)
		}
	}
	if NiceTitle("PUNCH-OUT!") != "Punch-Out!" {
		t.Error(NiceTitle("PUNCH-OUT!"))
	}
	if CleanName("Super Mario Sunshine (USA) [GMSE01].iso") != "Super Mario Sunshine" {
		t.Error(CleanName("Super Mario Sunshine (USA) [GMSE01].iso"))
	}
	for _, f := range []string{"a.ISO", "b.rvz", "c.wbfs", "d.wad", "e.gcz", "f.wia", "g.ciso", "h.gcm"} {
		if !IsGameFile(f) {
			t.Errorf("%s not a game file", f)
		}
	}
	if IsGameFile("x.zip") || IsGameFile("x.nes") {
		t.Error("not game files")
	}
}

func TestPadsAndSettings(t *testing.T) {
	user := t.TempDir()
	ini := func(n string) string { return ConfigFile(user, n) }
	if err := SetupXboxPads(user, false); err != nil {
		t.Fatal(err)
	}
	if PadState(user, "GCPadNew.ini", "GCPad1") != "xbox" || PadState(user, "WiimoteNew.ini", "Wiimote1") != "xbox" {
		t.Fatal("pads not set up")
	}
	b, _ := os.ReadFile(ini("GCPadNew.ini"))
	if !strings.Contains(string(b), "[GCPad1]\r\nDevice = XInput/0/Gamepad\r\nButtons/A = `Button A`") {
		t.Fatalf("GCPadNew.ini:\n%s", b)
	}
	if d, _ := os.ReadFile(ini("Dolphin.ini")); !strings.Contains(string(d), "SIDevice0 = 6") {
		t.Fatalf("Dolphin.ini:\n%s", d)
	}
	custom := "[GCPad1]\r\nDevice = DInput/0/Keyboard Mouse\r\nButtons/A = X\r\n[GCPad2]\r\nDevice = XInput/1/Gamepad\r\n"
	os.WriteFile(ini("GCPadNew.ini"), []byte(custom), 0o644)
	SetupXboxPads(user, false)
	if PadState(user, "GCPadNew.ini", "GCPad1") != "custom" {
		t.Fatal("overwrote a custom mapping")
	}
	SetupXboxPads(user, true)
	b, _ = os.ReadFile(ini("GCPadNew.ini"))
	if PadState(user, "GCPadNew.ini", "GCPad1") != "xbox" || !strings.Contains(string(b), "[GCPad2]\r\nDevice = XInput/1/Gamepad") {
		t.Fatalf("forced set-up wrong:\n%s", b)
	}
	// The first version of each file is kept.
	if orig, _ := os.ReadFile(ini("GCPadNew.ini") + ".retropixels-original"); len(orig) == 0 {
		t.Error("no backup of the original file")
	}

	os.WriteFile(ini("GFX.ini"), []byte("[Settings]\r\nInternalResolution = 1\r\nShaderCompilationMode = 2\r\n[Hardware]\r\nVSync = False\r\n"), 0o644)
	if v := ReadSettings(user); v["res"] != "1" || v["vsync"] != "False" || v["backend"] != "Vulkan" || v["confirmstop"] != "True" {
		t.Fatalf("read: %v", v)
	}
	if err := SaveSettings(user, map[string]string{"res": "3", "aspect": "2", "vsync": "true", "widehack": "True", "backend": "D3D12"}); err != nil {
		t.Fatal(err)
	}
	g, _ := os.ReadFile(ini("GFX.ini"))
	d, _ := os.ReadFile(ini("Dolphin.ini"))
	for _, want := range []string{"InternalResolution = 3", "AspectRatio = 2", "wideScreenHack = True", "ShaderCompilationMode = 2", "VSync = True"} {
		if !strings.Contains(string(g), want) {
			t.Errorf("GFX.ini missing %q:\n%s", want, g)
		}
	}
	if !strings.Contains(string(d), "GFXBackend = D3D12") {
		t.Error("backend not saved")
	}
	if v := ReadSettings(user); v["res"] != "3" || v["vsync"] != "True" || v["widehack"] != "True" {
		t.Errorf("read back: %v", v)
	}
	for _, bad := range []map[string]string{{"res": "7"}, {"vsync": "maybe"}, {"nope": "1"}} {
		if err := SaveSettings(user, bad); err == nil {
			t.Errorf("accepted %v", bad)
		}
	}
	// A value set in Dolphin itself that isn't one of the choices can be kept.
	os.WriteFile(ini("GFX.ini"), []byte("[Settings]\r\nInternalResolution = 7\r\n"), 0o644)
	if err := SaveSettings(user, map[string]string{"res": "7", "fps": "True"}); err != nil {
		t.Errorf("keeping Dolphin's own value: %v", err)
	}

	if DumpDir(user) != filepath.Join(user, "Dump") {
		t.Errorf("dump dir %s", DumpDir(user))
	}
	di := ReadINI(ini("Dolphin.ini"))
	di.Set("General", "DumpPath", "/data/dumps/")
	di.Save(ini("Dolphin.ini"))
	if DumpDir(user) != "/data/dumps" {
		t.Errorf("custom dump dir %s", DumpDir(user))
	}
}

func TestUserDir(t *testing.T) {
	t.Setenv("RETRO_DOLPHIN_USER", "")
	exe := filepath.Join(t.TempDir(), "Dolphin.exe")
	os.WriteFile(filepath.Join(filepath.Dir(exe), "portable.txt"), nil, 0o644)
	if UserDir(exe) != filepath.Join(filepath.Dir(exe), "User") {
		t.Errorf("portable: %s", UserDir(exe))
	}
	ad := t.TempDir()
	t.Setenv("APPDATA", ad)
	t.Setenv("HOME", t.TempDir())
	if userDirFor("windows", "") != filepath.Join(ad, "Dolphin Emulator") {
		t.Errorf("default: %s", userDirFor("windows", ""))
	}
	t.Setenv("RETRO_DOLPHIN_USER", "/x")
	if UserDir(exe) != "/x" {
		t.Error("override")
	}
}

func TestGameArgs(t *testing.T) {
	join := func(o LaunchOptions) string { return strings.Join(GameArgs(`D:\Games\Sunshine.rvz`, o), " ") }
	a := join(LaunchOptions{Embed: true, Fullscreen: true})
	for _, want := range []string{`-b -e D:\Games\Sunshine.rvz -C Dolphin.DSP.Volume=100`, "Dolphin.Display.Fullscreen=False",
		"Dolphin.Display.RenderToMain=False", "Dolphin.Interface.ConfirmStop=False", "Dolphin.Interface.PauseOnFocusLost=False",
		"Dolphin.Input.BackgroundInput=True"} {
		if !strings.Contains(a, want) {
			t.Errorf("embedded: missing %q in %s", want, a)
		}
	}
	if strings.Contains(a, "DumpFrames") {
		t.Error("records without being asked")
	}
	if a := join(LaunchOptions{Fullscreen: true}); !strings.Contains(a, "Dolphin.Display.Fullscreen=True") || strings.Contains(a, "RenderToMain") {
		t.Errorf("own window: %s", a)
	}
	if a := join(LaunchOptions{}); strings.Contains(a, "Fullscreen") {
		t.Errorf("windowed: %s", a)
	}
	if a := join(LaunchOptions{Embed: true, Record: true, Quality: "small"}); !strings.Contains(a, "Dolphin.Movie.DumpFrames=True") || !strings.Contains(a, "BitrateKbps=25000") {
		t.Errorf("recording: %s", a)
	}

	args := RecordArgs("high")
	want := map[string]bool{
		"Dolphin.Movie.DumpFrames=True": false, "Dolphin.DSP.DumpAudio=True": false,
		"Graphics.Settings.DumpFormat=avi": false, "Graphics.Settings.DumpCodec=mpeg4": false,
		"Graphics.Settings.InternalResolutionFrameDumps=False": false, "Graphics.Settings.BitrateKbps=50000": false,
		"Graphics.Settings.DumpPath=": false,
	}
	for i := 0; i < len(args); i += 2 {
		if args[i] != "-C" {
			t.Fatalf("args not in -C pairs: %v", args)
		}
		// Dolphin reads System.Section.Key=Value.
		if !regexp.MustCompile(`^(Dolphin|Graphics)\.[A-Za-z]+\.[A-Za-z0-9]+=[^=]*$`).MatchString(args[i+1]) {
			t.Errorf("bad setting %q", args[i+1])
		}
		if _, ok := want[args[i+1]]; ok {
			want[args[i+1]] = true
		}
	}
	for k, seen := range want {
		if !seen {
			t.Errorf("missing %s", k)
		}
	}
	if c := Command(`/opt/dolphin/Dolphin.exe`, "-b"); c.Dir != "/opt/dolphin" {
		t.Errorf("runs in %q", c.Dir)
	}
}

func TestWADAddOnByTitleID(t *testing.T) {
	dir := t.TempDir()
	mk := func(name string, upper uint32) string {
		b := make([]byte, 0x400)
		copy(b, []byte{0, 0, 0, 0x20, 'I', 's', 0, 0})
		binary.BigEndian.PutUint32(b[64+0x18C:], upper)
		copy(b[64+0x18C+4:], "WR9E")
		p := filepath.Join(dir, name)
		if err := os.WriteFile(p, b, 0o644); err != nil {
			t.Fatal(err)
		}
		return p
	}
	game := mk("Mega Man 9.wad", 0x00010001)
	addOn := mk("Mega Man 9 extra.wad", 0x00010005)
	if IsDLC(game) {
		t.Errorf("%s: a WiiWare game isn't add-on content", game)
	}
	if !IsDLC(addOn) {
		t.Errorf("%s: title ID 00010005 is add-on content", addOn)
	}
	if _, ok := GameFor(game); !ok {
		t.Errorf("the game should be recognised")
	}
}
