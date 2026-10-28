package rpcs3

import (
	"bytes"
	"encoding/binary"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// A made-up content ID for tests (no real game).
const testID = "XX0000-TEST00001_00-RETROPIXELSTEST1"

func TestSFOReadsWhatItWrites(t *testing.T) {
	b := BuildSFO(map[string]any{"TITLE": "Test Game ™", "TITLE_ID": "TEST00001", "CATEGORY": "HG", "PARENTAL_LEVEL": uint32(3)})
	s, err := ParseSFO(b)
	if err != nil {
		t.Fatal(err)
	}
	if s.String("TITLE") != "Test Game ™" || s.String("TITLE_ID") != "TEST00001" || s.String("CATEGORY") != "HG" || s.Int("PARENTAL_LEVEL") != 3 {
		t.Fatalf("%v", s)
	}
	if s.String("PARENTAL_LEVEL") != "" || s.Int("TITLE") != 0 || s.String("MISSING") != "" {
		t.Error("wrong kinds")
	}
}

// A PARAM.SFO laid out by hand: a text value without its zero (format
// 0x0004) and room kept beyond the value, as real ones have.
func TestSFOByHand(t *testing.T) {
	le := binary.LittleEndian
	var b []byte
	b = append(b, "\x00PSF"...)
	b = le.AppendUint32(b, 0x101)
	b = le.AppendUint32(b, 20+32) // keys
	b = le.AppendUint32(b, 20+32+16)
	b = le.AppendUint32(b, 2)
	entry := func(ko uint16, f uint16, n, room, off uint32) {
		b = le.AppendUint16(b, ko)
		b = le.AppendUint16(b, f)
		b = le.AppendUint32(b, n)
		b = le.AppendUint32(b, room)
		b = le.AppendUint32(b, off)
	}
	entry(0, 0x0004, 2, 4, 0)  // CATEGORY
	entry(9, 0x0204, 6, 16, 4) // TITLE
	b = append(b, "CATEGORY\x00TITLE\x00\x00"...)
	b = append(b, "DG\x00\x00"...)
	b = append(b, "Hello\x00"...)
	b = append(b, make([]byte, 10)...)
	s, err := ParseSFO(b)
	if err != nil {
		t.Fatal(err)
	}
	if s.String("CATEGORY") != "DG" || s.String("TITLE") != "Hello" {
		t.Fatalf("%v", s)
	}
	for _, bad := range [][]byte{nil, []byte("PSF\x00xxxxxxxxxxxxxxxx"), b[:30]} {
		if _, err := ParseSFO(bad); err == nil {
			t.Errorf("read %q", bad)
		}
	}
}

// fakeEboot is a signed program's start with an NPDRM block.
func fakeEboot(t *testing.T, path, contentID string, licence uint32) {
	t.Helper()
	var b bytes.Buffer
	b.WriteString("SCE\x00")
	b.Write(make([]byte, 0x200))
	b.WriteString("NPD\x00")
	binary.Write(&b, binary.BigEndian, uint32(3))
	binary.Write(&b, binary.BigEndian, licence)
	binary.Write(&b, binary.BigEndian, uint32(0))
	id := make([]byte, 0x30)
	copy(id, contentID)
	b.Write(id)
	b.Write(make([]byte, 0x100))
	os.MkdirAll(filepath.Dir(path), 0o755)
	if err := os.WriteFile(path, b.Bytes(), 0o644); err != nil {
		t.Fatal(err)
	}
}

func writeGame(t *testing.T, dir string, sfo map[string]any, eboot bool) {
	t.Helper()
	os.MkdirAll(filepath.Join(dir, "USRDIR"), 0o755)
	os.WriteFile(filepath.Join(dir, "PARAM.SFO"), BuildSFO(sfo), 0o644)
	if eboot {
		os.WriteFile(filepath.Join(dir, "USRDIR", "EBOOT.BIN"), []byte("\x7fELF plain program"), 0o644)
	}
}

// fakeRPCS3 is an RPCS3 folder: the program, and the system software unless noFW.
func fakeRPCS3(t *testing.T, noFW bool) string {
	t.Helper()
	root := filepath.Join(t.TempDir(), "rpcs3-v0.0.21")
	os.MkdirAll(root, 0o755)
	os.WriteFile(filepath.Join(root, ExeName), []byte("#!/bin/sh\n"), 0o755)
	if !noFW {
		os.MkdirAll(filepath.Join(root, "dev_flash", "vsh", "module"), 0o755)
		os.MkdirAll(filepath.Join(root, "dev_flash", "vsh", "etc"), 0o755)
		os.WriteFile(filepath.Join(root, "dev_flash", "vsh", "module", "vsh.self"), []byte("x"), 0o644)
		os.WriteFile(filepath.Join(root, "dev_flash", "vsh", "etc", "version.txt"), []byte("release:04.8900:\nbuild:12345\n"), 0o644)
	}
	return root
}

func TestGamesListsInstalledAndDiscGames(t *testing.T) {
	root := fakeRPCS3(t, false)
	game := filepath.Join(root, "dev_hdd0", "game")
	writeGame(t, filepath.Join(game, "TEST00001"), map[string]any{"TITLE": "Zeta Test", "TITLE_ID": "TEST00001", "CATEGORY": "HG", "APP_VER": "01.00"}, false)
	fakeEboot(t, filepath.Join(game, "TEST00001", "USRDIR", "EBOOT.BIN"), testID, licenceLocal)
	writeGame(t, filepath.Join(game, "TEST00002"), map[string]any{"TITLE": "Update data", "TITLE_ID": "TEST00002", "CATEGORY": "GD"}, true)
	writeGame(t, filepath.Join(game, "TEST00003"), map[string]any{"TITLE": "No program", "TITLE_ID": "TEST00003", "CATEGORY": "HG"}, false)
	os.MkdirAll(filepath.Join(game, "EMPTY"), 0o755)
	disc := filepath.Join(t.TempDir(), "Disc Game")
	writeGame(t, filepath.Join(disc, "PS3_GAME"), map[string]any{"TITLE": "Alpha Disc", "TITLE_ID": "TEST00004", "CATEGORY": "DG"}, true)
	os.MkdirAll(filepath.Join(root, "config"), 0o755)
	os.WriteFile(filepath.Join(root, "config", "games.yml"), []byte("TEST00004: "+filepath.ToSlash(disc)+"/\nTEST00009: /nowhere/\n"), 0o644)

	gs := Games(root)
	if len(gs) != 2 {
		t.Fatalf("%+v", gs)
	}
	if gs[0].Title != "Alpha Disc" || !gs[0].Disc || gs[0].Eboot != filepath.Join(disc, "PS3_GAME", "USRDIR", "EBOOT.BIN") || gs[0].Licence.Needed {
		t.Errorf("disc: %+v", gs[0])
	}
	z := gs[1]
	if z.Title != "Zeta Test" || z.TitleID != "TEST00001" || z.Version != "01.00" || z.Disc {
		t.Errorf("installed: %+v", z)
	}
	if !z.Licence.Needed || z.Licence.ContentID != testID || z.Licence.OK() || !strings.Contains(z.Licence.Message(), testID+".rap") {
		t.Errorf("licence: %+v", z.Licence)
	}
	if err := Ready(root, z); err == nil || !strings.Contains(err.Error(), "Add licence file") {
		t.Errorf("ready without a licence: %v", err)
	}
	if err := Ready(root, gs[0]); err != nil {
		t.Errorf("disc game: %v", err)
	}

	// The player's licence file goes into exdata, named as it was.
	src := filepath.Join(t.TempDir(), strings.ToLower(testID)+".rap")
	os.WriteFile(src, bytes.Repeat([]byte{7}, 16), 0o644)
	dst, err := AddLicence(root, src)
	if err != nil {
		t.Fatal(err)
	}
	if dst != filepath.Join(root, "dev_hdd0", "home", "00000001", "exdata", testID+".rap") {
		t.Errorf("went to %s", dst)
	}
	if b, _ := os.ReadFile(dst); !bytes.Equal(b, bytes.Repeat([]byte{7}, 16)) {
		t.Error("not copied as it was")
	}
	if l := CheckLicence(root, z.Eboot); !l.OK() || l.Found != dst {
		t.Errorf("after adding: %+v", l)
	}
	if err := Ready(root, z); err != nil {
		t.Error(err)
	}
}

func TestLicenceKinds(t *testing.T) {
	root := fakeRPCS3(t, false)
	p := filepath.Join(t.TempDir(), "EBOOT.BIN")
	fakeEboot(t, p, testID, licenceFree)
	if l := CheckLicence(root, p); l.Needed || !l.OK() || l.ContentID != testID {
		t.Errorf("free: %+v", l)
	}
	fakeEboot(t, p, testID, licenceNetwork)
	if l := CheckLicence(root, p); !l.Needed {
		t.Errorf("network: %+v", l)
	}
	// An .edat licence in another user's folder counts too.
	d := ExdataDir(root, "00000002")
	os.MkdirAll(d, 0o755)
	os.WriteFile(filepath.Join(d, testID+".edat"), []byte("NPD\x00..."), 0o644)
	if l := CheckLicence(root, p); !l.OK() {
		t.Errorf("edat: %+v", l)
	}
	os.WriteFile(p, []byte("\x7fELF"), 0o644)
	if l := CheckLicence(root, p); l.Needed || l.ContentID != "" {
		t.Errorf("plain: %+v", l)
	}
}

func TestAddLicenceChecksTheFile(t *testing.T) {
	root := fakeRPCS3(t, false)
	dir := t.TempDir()
	write := func(name string, b []byte) string {
		p := filepath.Join(dir, name)
		os.WriteFile(p, b, 0o644)
		return p
	}
	for _, p := range []string{
		write("licence.txt", make([]byte, 16)),
		write("my game.rap", make([]byte, 16)),
		write(testID+".rap", make([]byte, 20)),
		write(testID+".edat", []byte("not an edat")),
	} {
		if _, err := AddLicence(root, p); err == nil {
			t.Errorf("took %s", filepath.Base(p))
		}
	}
	if _, err := AddLicence(root, write(testID+".edat", []byte("NPD\x00rest"))); err != nil {
		t.Error(err)
	}
	if _, err := AddLicence(filepath.Join(dir, "no rpcs3"), write(testID+".rap", make([]byte, 16))); err == nil {
		t.Error("copied into a folder without RPCS3")
	}
}

func TestFirmware(t *testing.T) {
	if ok, _ := Firmware(fakeRPCS3(t, true)); ok {
		t.Error("empty dev_flash counted as installed")
	}
	ok, v := Firmware(fakeRPCS3(t, false))
	if !ok || v != "4.89" {
		t.Errorf("%v %q", ok, v)
	}
	root := fakeRPCS3(t, true)
	g := Game{Eboot: filepath.Join(root, "EBOOT.BIN")}
	if err := Ready(root, g); err == nil || !strings.Contains(err.Error(), "PS3UPDAT.PUP") {
		t.Errorf("%v", err)
	}
}

func TestXboxPadAsPlayer1(t *testing.T) {
	root := fakeRPCS3(t, false)
	p := InputConfig(root)
	if Player1Handler(root) != "" {
		t.Fatal("handler before")
	}
	if ch, err := UseXboxPad(root, false); err != nil || !ch {
		t.Fatal(ch, err)
	}
	b, _ := os.ReadFile(p)
	if Player1Handler(root) != "XInput" || !strings.Contains(string(b), "Cross: A") || !strings.Contains(string(b), "Enable Large Vibration Motor: true") {
		t.Fatalf("%s", b)
	}
	if ch, _ := UseXboxPad(root, false); ch {
		t.Error("written again")
	}

	// The player's own choice stays, unless they ask.
	mine := "Player 1 Input:\r\n  Handler: Keyboard\r\n  Device: Keyboard\r\n  Config:\r\n    Cross: Z\r\nPlayer 2 Input:\r\n  Handler: DualShock 4\r\n  Device: DS4 Pad #1\r\n"
	os.WriteFile(p, []byte(mine), 0o644)
	if ch, _ := UseXboxPad(root, false); ch || Player1Handler(root) != "Keyboard" {
		t.Fatal("changed the player's settings")
	}
	if ch, err := UseXboxPad(root, true); err != nil || !ch {
		t.Fatal(ch, err)
	}
	b, _ = os.ReadFile(p)
	s := string(b)
	if Player1Handler(root) != "XInput" || !strings.Contains(s, "Player 2 Input:\n  Handler: DualShock 4") || strings.Contains(s, "Cross: Z") {
		t.Fatalf("%s", s)
	}
	if bak, _ := os.ReadFile(p + ".bak"); string(bak) != mine {
		t.Error("no backup")
	}

	// No controller for player 1: set up without asking.
	os.WriteFile(p, []byte("Player 1 Input:\n  Handler: \"Null\"\n"), 0o644)
	if ch, _ := UseXboxPad(root, false); !ch || Player1Handler(root) != "XInput" {
		t.Error("Null not replaced")
	}
}

func TestFindExe(t *testing.T) {
	t.Setenv("RETRO_RPCS3_HOME", filepath.Join(t.TempDir(), "home"))
	root := fakeRPCS3(t, false)
	exe := filepath.Join(root, ExeName)
	other := fakeRPCS3(t, false)
	if FindExe(root, filepath.Join(other, ExeName), "") != exe {
		t.Error("the chosen folder doesn't win")
	}
	if FindExe(t.TempDir(), exe, "") != "" {
		t.Error("a chosen folder without RPCS3 fell back to another")
	}
	if FindExe("", exe, "") != exe {
		t.Error("remembered")
	}
	app := t.TempDir()
	os.MkdirAll(filepath.Join(app, "RPCS3"), 0o755)
	os.WriteFile(filepath.Join(app, "RPCS3", ExeName), nil, 0o755)
	if FindExe("", "", app) != filepath.Join(app, "RPCS3", ExeName) {
		t.Error("next to RetroPixels")
	}

	// A drive: E:\Roms All Systems\008 - PS3\- PS3\rpcs3-…
	drive := t.TempDir()
	deep := filepath.Join(drive, "Roms All Systems", "008 - PS3", "- PS3", "rpcs3-v0.0.21-13400-e53bbd66_win64")
	os.MkdirAll(deep, 0o755)
	os.WriteFile(filepath.Join(deep, ExeName), nil, 0o755)
	os.MkdirAll(filepath.Join(drive, "Music", "rpcs3"), 0o755)
	if got := Search([]string{drive}); got != filepath.Join(deep, ExeName) {
		t.Errorf("search: %q", got)
	}
	if got := Search([]string{t.TempDir()}); got != "" {
		t.Errorf("found %q", got)
	}
}

func TestCommandLines(t *testing.T) {
	if got := strings.Join(GameArgs(`E:\g\EBOOT.BIN`, LaunchOptions{Fullscreen: true}), " "); got != `--no-gui --fullscreen E:\g\EBOOT.BIN` {
		t.Error(got)
	}
	if got := strings.Join(GameArgs("EBOOT.BIN", LaunchOptions{Fullscreen: true, Embed: true}), " "); got != "--no-gui EBOOT.BIN" {
		t.Error(got)
	}
	if got := strings.Join(InstallPkgArgs("a.pkg"), " "); got != "--installpkg a.pkg" {
		t.Error(got)
	}
	if got := strings.Join(InstallFirmwareArgs("PS3UPDAT.PUP"), " "); got != "--installfw PS3UPDAT.PUP" {
		t.Error(got)
	}
	d := t.TempDir()
	pkg, pup := filepath.Join(d, "game.pkg"), filepath.Join(d, "PS3UPDAT.PUP")
	os.WriteFile(pkg, []byte("\x7fPKG...."), 0o644)
	os.WriteFile(pup, []byte("SCEUF..."), 0o644)
	if CheckPkg(pkg) != nil || CheckPkg(pup) == nil || CheckPUP(pup) != nil || CheckPUP(pkg) == nil {
		t.Error("file checks")
	}
}

func TestProblem(t *testing.T) {
	if p := Problem("·E 0:00:01 SELF: Failed to locate the game license file: x.rap"); !strings.Contains(p, "Add licence file") {
		t.Error(p)
	}
	if p := Problem("·F 0:00:00 SYS: Something fatal\n"); p != "·F 0:00:00 SYS: Something fatal" {
		t.Error(p)
	}
	if Problem("") != "" {
		t.Error("empty")
	}
}
