package pixelu

import (
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"
	"time"
)

func write(t *testing.T, p string, data []byte) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(p, data, 0o644); err != nil {
		t.Fatal(err)
	}
}

func TestFindExe(t *testing.T) {
	root := t.TempDir()
	app := filepath.Join(root, "RetroPixels")
	home := filepath.Join(root, "home")
	os.MkdirAll(app, 0o755)
	if got := FindExe("", app, home); got != "" {
		t.Fatalf("nothing installed: got %q", got)
	}
	// The user's copy: Downloads\PixelU-test1\PixelU\PixelU.exe (the newest one wins).
	old := filepath.Join(home, "Downloads", "PixelU-old", "PixelU", ExeName)
	cur := filepath.Join(home, "Downloads", "PixelU-test1", "PixelU", ExeName)
	write(t, old, []byte("old"))
	write(t, cur, []byte("new"))
	os.Chtimes(old, time.Now().Add(-time.Hour), time.Now().Add(-time.Hour))
	write(t, filepath.Join(home, "Downloads", "Other", "PixelU", ExeName), []byte("x")) // not a PixelU* folder
	if got := FindExe("", app, home); got != cur {
		t.Fatalf("Downloads: got %q want %q", got, cur)
	}
	// A PixelU folder next to RetroPixels comes before Downloads.
	next := filepath.Join(app, "PixelU", ExeName)
	write(t, next, []byte("x"))
	if got := FindExe("", app, home); got != next {
		t.Fatalf("next to RetroPixels: got %q", got)
	}
	// The one the player chose comes first, while it exists.
	chosen := filepath.Join(root, "Emus", "PixelU", ExeName)
	write(t, chosen, []byte("x"))
	if got := FindExe(chosen, app, home); got != chosen {
		t.Fatalf("chosen: got %q", got)
	}
	if got := FindExe(filepath.Join(root, "gone", ExeName), app, home); got != next {
		t.Fatalf("chosen but deleted: got %q", got)
	}
}

func TestLibrary(t *testing.T) {
	dir := filepath.Join(t.TempDir(), "Nintendo Wii - Wii U games")
	write(t, filepath.Join(dir, "WII  U -Hyrule Warriors (US).wua"), []byte("WUA0"))
	write(t, filepath.Join(dir, "Mario Kart 8 (USA).wux"), []byte("WUX0"))
	write(t, filepath.Join(dir, "Discs", "Wii Party U [ANXE01].wud"), []byte("WUP-P-ANXE"))
	write(t, filepath.Join(dir, "Discs", "Xenoblade_part1.wud"), []byte("WUP-P-AX5E"))
	write(t, filepath.Join(dir, "Discs", "Xenoblade_part2.wud"), []byte("rest"))
	write(t, filepath.Join(dir, "Discs", "Pikmin 3.iso"), []byte("WUP-P-AC3E"))
	write(t, filepath.Join(dir, "Discs", "Wii Sports.iso"), append(make([]byte, 0x18), 0x5D, 0x1C, 0x9E, 0xA3)) // a Wii disc
	write(t, filepath.Join(dir, "Discs", "PC game.iso"), []byte("\x00\x00\x00\x00"))
	// Unpacked games: <title>\code\*.rpx. Updates and DLC are left out.
	title := filepath.Join(dir, "Unpacked", "Super Mario 3D World [0005000010145D00]")
	write(t, filepath.Join(title, "code", "app.xml"), []byte(`<app><title_id type="hexBinary" length="8">0005000010145D00</title_id></app>`))
	write(t, filepath.Join(title, "code", "RedCarpet.rpx"), []byte("rpx"))
	write(t, filepath.Join(title, "content", "x.bin"), []byte("x"))
	two := filepath.Join(dir, "Unpacked", "02 - Splatoon")
	write(t, filepath.Join(two, "code", "aaa.rpx"), []byte("rpx"))
	write(t, filepath.Join(two, "code", "Gambit.rpx"), []byte("rpx"))
	write(t, filepath.Join(two, "code", "cos.xml"), []byte(`<app><argstr type="string">Gambit.rpx</argstr></app>`))
	upd := filepath.Join(dir, "Unpacked", "Super Mario 3D World [Update]")
	write(t, filepath.Join(upd, "code", "app.xml"), []byte(`<title_id type="hexBinary" length="8">0005000E10145D00</title_id>`))
	write(t, filepath.Join(upd, "code", "RedCarpet.rpx"), []byte("rpx"))
	write(t, filepath.Join(dir, "readme.txt"), []byte("x"))

	played := time.Date(2026, 10, 1, 12, 0, 0, 0, time.UTC)
	hw := filepath.Join(dir, "WII  U -Hyrule Warriors (US).wua")
	got := map[string]string{}
	for _, g := range Library([]string{dir, dir}, map[string]time.Time{strings.ToLower(hw): played}) {
		rel, _ := filepath.Rel(dir, g.Path)
		got[g.Name] = g.Kind + " " + filepath.ToSlash(rel)
		if g.Path == hw && !g.Played.Equal(played) {
			t.Errorf("Hyrule Warriors played = %v", g.Played)
		}
	}
	want := map[string]string{
		"Hyrule Warriors":      "WUA WII  U -Hyrule Warriors (US).wua",
		"Mario Kart 8":         "WUX Mario Kart 8 (USA).wux",
		"Wii Party U":          "WUD Discs/Wii Party U [ANXE01].wud",
		"Xenoblade":            "WUD Discs/Xenoblade_part1.wud",
		"Pikmin 3":             "ISO Discs/Pikmin 3.iso",
		"Super Mario 3D World": "Folder Unpacked/Super Mario 3D World [0005000010145D00]/code/RedCarpet.rpx",
		"Splatoon":             "Folder Unpacked/02 - Splatoon/code/Gambit.rpx",
	}
	if !reflect.DeepEqual(got, want) {
		t.Errorf("library:\n got %v\nwant %v", got, want)
	}
}

func TestNamesAndFiles(t *testing.T) {
	for in, want := range map[string]string{
		"WII  U -Hyrule Warriors (US)":          "Hyrule Warriors",
		"Wii U - Bayonetta 2 [AQUE01]":          "Bayonetta 2",
		"Wii Party U (USA)":                     "Wii Party U",
		"Wii Sports Club":                       "Wii Sports Club",
		"Captain_Toad_Treasure_Tracker":         "Captain Toad Treasure Tracker",
		"007 - Legend of Zelda - Wind Waker HD": "Legend of Zelda: Wind Waker HD",
		"Mario Kart 8 v4.1":                     "Mario Kart 8 v4.1",
	} {
		if got := DisplayName(in); got != want {
			t.Errorf("DisplayName(%q) = %q want %q", in, got, want)
		}
	}
	dir := t.TempDir()
	for name, want := range map[string]bool{"a.wua": true, "a.WUD": true, "a.wux": true, "a.rpx": true, "a.elf": true, "a.wbfs": false, "a.zip": false} {
		write(t, filepath.Join(dir, name), []byte("x"))
		if IsGameFile(filepath.Join(dir, name)) != want {
			t.Errorf("IsGameFile(%s) != %v", name, want)
		}
	}
	write(t, filepath.Join(dir, "wiiu.iso"), []byte("WUP-P-ARKE"))
	write(t, filepath.Join(dir, "gc.iso"), []byte("GMSE01"))
	if !IsGameFile(filepath.Join(dir, "wiiu.iso")) || IsGameFile(filepath.Join(dir, "gc.iso")) {
		t.Error(".iso check wrong")
	}
	rpx := filepath.Join(dir, "Zelda BotW", "code", "U-King.rpx")
	write(t, rpx, []byte("x"))
	if g, ok := GameFor(rpx); !ok || g.Name != "Zelda BotW" {
		t.Errorf("GameFor(rpx) = %+v %v", g, ok)
	}
}

func TestGameArgs(t *testing.T) {
	g := `D:\Roms All Systems\Nintendo Wii - Wii U games\Hyrule Warriors.wua`
	cases := []struct {
		o    LaunchOptions
		want []string
	}{
		{LaunchOptions{}, []string{"-g", g}},
		{LaunchOptions{Fullscreen: true}, []string{"-g", g, "-f"}},
		{LaunchOptions{Fullscreen: true, RecordDir: `C:\rec\tmp`, FFmpeg: `C:\ff\ffmpeg.exe`},
			[]string{"-g", g, "-f", "--rp-record", `C:\rec\tmp`, "--rp-ffmpeg", `C:\ff\ffmpeg.exe`}},
		{LaunchOptions{RecordDir: `C:\rec\tmp`}, []string{"-g", g}}, // no ffmpeg: no recording
		{LaunchOptions{Embed: true}, []string{"-g", g, "--rp-embed"}},
	}
	for _, c := range cases {
		if got := GameArgs(g, c.o); !reflect.DeepEqual(got, c.want) {
			t.Errorf("GameArgs(%+v) = %q want %q", c.o, got, c.want)
		}
	}
	if cmd := Command(`/x/PixelU/PixelU.exe`, "-g", g); cmd.Dir != `/x/PixelU` {
		t.Errorf("Command dir = %q", cmd.Dir)
	}
}
