package rpcs3

import (
	"os"
	"path/filepath"
	"testing"
)

func TestPkgTitleID(t *testing.T) {
	dir := t.TempDir()
	h := make([]byte, 0x100)
	copy(h, []byte{0x7f, 'P', 'K', 'G'})
	copy(h[0x30:], "EP4062-NPEB00149_00-TMNTTURTLESTIME01")
	p := filepath.Join(dir, "game.pkg")
	os.WriteFile(p, h, 0o644)
	if id := PkgTitleID(p); id != "NPEB00149" {
		t.Fatalf("title id %q", id)
	}
	os.WriteFile(p, make([]byte, 0x100), 0o644)
	if id := PkgTitleID(p); id != "" {
		t.Fatalf("not a package: title id %q", id)
	}
}

func TestGameFiles(t *testing.T) {
	dir := t.TempDir()
	usr := filepath.Join(dir, "dev_hdd0", "game", "NPEB00149", "USRDIR")
	os.MkdirAll(usr, 0o755)
	if GameEboot(dir, "NPEB00149") != "" {
		t.Fatal("found a game that isn't installed")
	}
	os.WriteFile(filepath.Join(usr, "EBOOT.BIN"), []byte("SCE"), 0o644)
	if GameEboot(dir, "NPEB00149") == "" {
		t.Fatal("installed game not found")
	}
	sfo := filepath.Join(dir, "dev_hdd0", "game", "NPEB00149", "PARAM.SFO")
	if got := Eboot(sfo); got != filepath.Join(usr, "EBOOT.BIN") {
		t.Fatalf("EBOOT for PARAM.SFO: %q", got)
	}
	for _, p := range []string{"a/PARAM.SFO", "x/EBOOT.BIN", "Game.PKG"} {
		if !IsGameFile(p) {
			t.Errorf("%s not a game file", p)
		}
	}
	if IsGameFile("game.iso") {
		t.Error("iso taken as a game file")
	}
	if HasFirmware(dir) {
		t.Error("firmware found in an empty folder")
	}
}

func TestGameTitleLocal(t *testing.T) {
	eboot := "/tmp/claude-0/ps3/game/USRDIR/EBOOT.BIN"
	if _, err := os.Stat("/tmp/claude-0/ps3/game/PARAM.SFO"); err != nil {
		t.Skip("no game here")
	}
	t.Log(GameTitle(eboot))
}

func TestSetAudioRenderer(t *testing.T) {
	in := "Video:\r\n  Renderer: Vulkan\r\nAudio:\r\n  Renderer: Cubeb\r\n  Audio Provider: CellAudio\r\n"
	want := "Video:\r\n  Renderer: Vulkan\r\nAudio:\r\n  Renderer: XAudio2\r\n  Audio Provider: CellAudio\r\n"
	if got := string(SetAudioRenderer([]byte(in), "XAudio2")); got != want {
		t.Fatalf("got %q", got)
	}
	if got := string(SetAudioRenderer([]byte("Video:\n  Renderer: Vulkan\n"), "XAudio2")); got != "Video:\n  Renderer: Vulkan\nAudio:\n  Renderer: XAudio2\n" {
		t.Fatalf("no audio section: %q", got)
	}
}

func TestPrepareSettingsAudioOnce(t *testing.T) {
	dir := t.TempDir()
	cfg := filepath.Join(dir, "config", "config.yml")
	os.MkdirAll(filepath.Dir(cfg), 0o755)
	os.WriteFile(cfg, []byte("Audio:\n  Renderer: Cubeb\n"), 0o644)
	if err := PrepareSettings(dir); err != nil {
		t.Fatal(err)
	}
	b, _ := os.ReadFile(cfg)
	if string(b) != "Audio:\n  Renderer: XAudio2\n" {
		t.Fatalf("config %q", b)
	}
	os.WriteFile(cfg, []byte("Audio:\n  Renderer: Cubeb\n"), 0o644)
	PrepareSettings(dir)
	if b, _ := os.ReadFile(cfg); string(b) != "Audio:\n  Renderer: Cubeb\n" {
		t.Fatalf("changed the user's choice: %q", b)
	}
}
