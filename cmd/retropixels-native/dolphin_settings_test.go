package main

import (
	"archive/zip"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"retropixels/internal/dolphin"
)

func TestDolphinSettingsDefaults(t *testing.T) {
	var d DolphinSettings
	if !d.fullscreen() || !d.embed() || d.volume() != 100 || d.Muted {
		t.Fatal("defaults differ from RetroPixels.exe's (fullscreen, in the window, recorded, 100%)")
	}
	off := false
	d.Fullscreen, d.Embed = &off, &off
	d.setVolume(130)
	if d.fullscreen() || d.embed() || d.volume() != 100 {
		t.Fatal("switches")
	}
	d.setVolume(0)
	if d.volume() != 0 {
		t.Fatal("0 is a real volume")
	}

	// kept in settings.json and read back
	t.Setenv("RETRO_NATIVE_DIR", t.TempDir())
	s := loadSettings()
	s.Dolphin = d
	s.Dolphin.Exe = `C:\Dolphin\Dolphin.exe`
	s.Dolphin.markPlayed(`D:\Games\Sunshine.rvz`, time.Date(2026, 10, 1, 0, 0, 0, 0, time.UTC))
	if err := saveSettings(s); err != nil {
		t.Fatal(err)
	}
	b, _ := os.ReadFile(settingsPath())
	if !strings.Contains(string(b), `"dolphin": {`) || !strings.Contains(string(b), `"playInWindow": false`) {
		t.Fatalf("settings.json:\n%s", b)
	}
	got := loadSettings().Dolphin
	if got.Exe != s.Dolphin.Exe || got.volume() != 0 || got.embed() || got.Played[`d:\games\sunshine.rvz`].IsZero() {
		t.Fatalf("read back %+v", got)
	}
}

func TestDolphinPlayedAndFolders(t *testing.T) {
	var d DolphinSettings
	start := time.Now()
	for i := 0; i < 250; i++ {
		d.markPlayed(filepath.Join("games", string(rune('a'+i%26))+strings.Repeat("x", i)), start.Add(time.Duration(i)*time.Second))
	}
	if len(d.Played) != 200 {
		t.Fatalf("kept %d", len(d.Played))
	}
	if _, ok := d.Played[strings.ToLower(filepath.Join("games", "a"))]; ok {
		t.Error("kept the oldest instead of the newest")
	}
	dir := t.TempDir()
	if !d.addFolder(dir) || d.addFolder(dir+string(os.PathSeparator)) || d.addFolder("") {
		t.Error("addFolder")
	}
	d.addFolder(filepath.Join(dir, "Wii"))
	d.removeFolder(dir)
	if len(d.Folders) != 1 || d.Folders[0] != filepath.Join(dir, "Wii") {
		t.Errorf("folders %v", d.Folders)
	}
}

// The first time, the games folders (and the Dolphin used before, for its
// old settings) come from what RetroPixels.exe already found, else from
// looking around near that Dolphin or near PixelU's engine.
func TestDetectDolphin(t *testing.T) {
	games := t.TempDir()
	exe := filepath.Join(t.TempDir(), "Dolphin-x64", "Dolphin.exe")
	os.MkdirAll(filepath.Dir(exe), 0o755)
	os.WriteFile(exe, []byte("x"), 0o644)
	cfg := filepath.Join(t.TempDir(), "config.json")
	t.Setenv("RETRO_APP_CONFIG", cfg)
	b, _ := json.Marshal(map[string]any{"dolphinExe": exe, "dolphinFolders": []string{games, filepath.Join(games, "gone")},
		"dolphinRecent": []map[string]any{{"path": `D:\Wii\Sports.wbfs`, "name": "Wii Sports", "lastPlayed": "2026-09-30T20:00:00Z"}}})
	os.WriteFile(cfg, b, 0o644)
	gotExe, folders, played := detectDolphin(DolphinSettings{}, "")
	if gotExe != exe || len(folders) != 1 || folders[0] != games || played[`d:\wii\sports.wbfs`].IsZero() {
		t.Fatalf("from RetroPixels.exe: %q %v %v", gotExe, folders, played)
	}
	// What the native app already has wins.
	mine := t.TempDir()
	gotExe, folders, _ = detectDolphin(DolphinSettings{Exe: exe, Folders: []string{mine}}, "")
	if gotExe != exe || len(folders) != 1 || folders[0] != mine {
		t.Fatalf("own settings: %q %v", gotExe, folders)
	}
	// No Dolphin used before: games are looked for near PixelU's engine.
	t.Setenv("RETRO_APP_CONFIG", filepath.Join(t.TempDir(), "none.json"))
	root := t.TempDir()
	engine := filepath.Join(root, "Emus", "PixelU", "wii", "PixelU-Wii.exe")
	os.MkdirAll(filepath.Dir(engine), 0o755)
	os.WriteFile(engine, []byte("x"), 0o644)
	gc := filepath.Join(root, "Emus", "GameCube")
	os.MkdirAll(gc, 0o755)
	hdr := make([]byte, 0x60)
	copy(hdr, "GMSE01")
	copy(hdr[0x1C:], []byte{0xC2, 0x33, 0x9F, 0x3D})
	copy(hdr[0x20:], "Super Mario Sunshine")
	os.WriteFile(filepath.Join(gc, "Sunshine.iso"), hdr, 0o644)
	gotExe, folders, _ = detectDolphin(DolphinSettings{Exe: filepath.Join(root, "moved", "Dolphin.exe")}, engine)
	if gotExe != "" || len(folders) != 1 || folders[0] != gc {
		t.Fatalf("near the engine: %q %v", gotExe, folders)
	}
}

// Games play only in PixelU's engine, found in the PixelU folder (never a
// Dolphin.exe), and the old Dolphin's settings come over once.
func TestDolphinEngine(t *testing.T) {
	t.Setenv("HOME", t.TempDir())
	t.Setenv("USERPROFILE", os.Getenv("HOME"))
	t.Setenv("RETRO_APP_CONFIG", filepath.Join(t.TempDir(), "none.json"))
	old := filepath.Join(t.TempDir(), "Dolphin-x64", "Dolphin.exe")
	os.MkdirAll(filepath.Join(filepath.Dir(old), "User", "Config"), 0o755)
	os.WriteFile(old, []byte("x"), 0o644)
	os.WriteFile(filepath.Join(filepath.Dir(old), "portable.txt"), nil, 0o644)
	os.WriteFile(filepath.Join(filepath.Dir(old), "User", "Config", "WiimoteNew.ini"), []byte("[Wiimote1]\r\nDevice = DInput/0/Keyboard Mouse\r\n"), 0o644)
	d := DolphinSettings{Exe: old}
	if got := d.exe(""); got != "" {
		t.Fatalf("a Dolphin.exe was used: %q", got)
	}
	px := filepath.Join(t.TempDir(), "PixelU")
	engine := filepath.Join(px, "wii", "PixelU-Wii.exe")
	os.MkdirAll(filepath.Dir(engine), 0o755)
	os.WriteFile(filepath.Join(px, "PixelU.exe"), []byte("x"), 0o644)
	os.WriteFile(engine, []byte("x"), 0o644)
	os.WriteFile(filepath.Join(px, "wii", "portable.txt"), nil, 0o644)
	if got := d.exe(filepath.Join(px, "PixelU.exe")); got != engine {
		t.Fatalf("engine = %q want %q", got, engine)
	}
	if !strings.Contains(noEngine, "Put the PixelU folder next to RetroPixels.exe") {
		t.Error(noEngine)
	}
	user := dolphin.UserDir(engine)
	if user != filepath.Join(px, "wii", "User") {
		t.Fatalf("user folder %q", user)
	}
	t.Setenv("RETRO_DOLPHIN_USER", "")
	t.Setenv("RETRO_OLD_DOLPHIN_USER", "")
	d.importOld(engine)
	if dolphin.PadState(user, "WiimoteNew.ini", "Wiimote1") != "custom" {
		t.Fatal("the old portable Dolphin's Wii Remote set-up didn't come over")
	}
	dolphin.SetupXboxPads(user, false) // as a game start does
	if dolphin.PadState(user, "WiimoteNew.ini", "Wiimote1") != "custom" || dolphin.PadState(user, "GCPadNew.ini", "GCPad1") != "xbox" {
		t.Fatal("Xbox set-up after the import")
	}
	if _, err := os.Stat(filepath.Join(filepath.Dir(old), "User", "Config", "GCPadNew.ini")); err == nil {
		t.Error("wrote into the old Dolphin's folder")
	}
}

func TestDiscInArchive(t *testing.T) {
	dir := t.TempDir()
	mk := func(name string, files ...string) string {
		p := filepath.Join(dir, name)
		f, _ := os.Create(p)
		zw := zip.NewWriter(f)
		for _, n := range files {
			w, _ := zw.Create(n)
			w.Write([]byte("data"))
		}
		zw.Close()
		f.Close()
		return p
	}
	if got := discInArchive(mk("gc.zip", "readme.txt", "Games/Super Mario Sunshine (USA).rvz")); got != "Super Mario Sunshine (USA).rvz" {
		t.Errorf("got %q", got)
	}
	if got := discInArchive(mk("nes.zip", "Super Mario Bros.nes")); got != "" {
		t.Errorf("NES zip: %q", got)
	}
	if got := discInArchive(filepath.Join(dir, "missing.7z")); got != "" {
		t.Errorf("missing: %q", got)
	}
	exts := strings.Join(dolphinExts(), " ")
	for _, e := range []string{".iso", ".gcm", ".rvz", ".wbfs", ".ciso", ".gcz", ".wia", ".wad"} {
		if !strings.Contains(exts, e) {
			t.Errorf("open dialog misses %s", e)
		}
	}
}

func TestLibraryList(t *testing.T) {
	day := func(n int) time.Time { return time.Date(2026, 10, n, 12, 0, 0, 0, time.UTC) }
	games := []dolphin.Game{
		{Name: "Super Mario Sunshine", File: "GMSE01.rvz", Platform: "GameCube", Added: day(1), Played: day(10)},
		{Name: "Wii Sports", File: "Wii Sports.wbfs", Platform: "Wii", Added: day(3)},
		{Name: "Mega Man 9", File: "MM9.wad", Platform: "WiiWare", Added: day(2), Played: day(12)},
		{Name: "Sonic Heroes", File: "heroes.iso", Platform: "GameCube", Added: day(4)},
	}
	names := func(l []dolphin.Game) string {
		var n []string
		for _, g := range l {
			n = append(n, g.Name)
		}
		return strings.Join(n, ", ")
	}
	for _, c := range []struct{ filter, q, order, want string }{
		{"all", "", "az", "Super Mario Sunshine, Wii Sports, Mega Man 9, Sonic Heroes"},
		{"GameCube", "", "az", "Super Mario Sunshine, Sonic Heroes"},
		{"Wii", "", "az", "Wii Sports"},
		{"all", "gmse", "az", "Super Mario Sunshine"},
		{"all", "  SONIC ", "az", "Sonic Heroes"},
		{"all", "", "played", "Mega Man 9, Super Mario Sunshine, Sonic Heroes, Wii Sports"},
		{"all", "", "added", "Sonic Heroes, Wii Sports, Mega Man 9, Super Mario Sunshine"},
	} {
		if got := names(filterGames(games, c.filter, c.q, c.order)); got != c.want {
			t.Errorf("%+v: %s", c, got)
		}
	}
	for b, want := range map[int64]string{1_400_000_000: "1.4 GB", 349_600_000: "350 MB", 12_300: "12 KB", 10: "1 KB", 0: "0"} {
		if got := fmtSize(b); got != want {
			t.Errorf("fmtSize(%d) = %q want %q", b, got, want)
		}
	}
	now := time.Date(2026, 10, 15, 9, 0, 0, 0, time.Local)
	for _, c := range []struct {
		t    time.Time
		want string
	}{
		{time.Time{}, ""}, {now.Add(-time.Hour), "Today"}, {now.Add(-24 * time.Hour), "Yesterday"},
		{now.Add(-72 * time.Hour), "3 days ago"}, {time.Date(2026, 3, 2, 0, 0, 0, 0, time.Local), "2 Mar"},
		{time.Date(2025, 3, 2, 0, 0, 0, 0, time.Local), "2 Mar 2025"},
	} {
		if got := fmtPlayed(c.t, now); got != c.want {
			t.Errorf("fmtPlayed(%v) = %q want %q", c.t, got, c.want)
		}
	}
}
