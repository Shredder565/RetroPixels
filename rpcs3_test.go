//go:build !windows

package main

import (
	"bytes"
	"encoding/binary"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"retropixels/internal/rpcs3"
)

// fakeRPCS3 is a stand-in rpcs3: it notes its arguments; FAKE_FAIL makes it
// say what RPCS3 says about a missing licence and quit.
const fakeRPCS3 = `#!/bin/sh
echo "$@" >> "$FAKE_ARGS"
if [ -n "$FAKE_FAIL" ]; then
  echo "·E 0:00:00.100 {PPU} SELF: Failed to locate the game license file: x.rap"
  exit 1
fi
sleep "${FAKE_PLAY:-1}"
`

// A made-up content ID (no real game).
const ps3TestID = "XX0000-TEST00001_00-RETROPIXELSTEST1"

// setupFakePS3: RPCS3 with the system software and one installed game
// that needs a licence, in a folder the player chose.
func setupFakePS3(t *testing.T) (root, argsFile string, g rpcs3.Game) {
	t.Helper()
	root = filepath.Join(t.TempDir(), "rpcs3-v0.0.21")
	os.MkdirAll(filepath.Join(root, "dev_flash", "vsh", "module"), 0o755)
	os.WriteFile(filepath.Join(root, "dev_flash", "vsh", "module", "vsh.self"), []byte("x"), 0o644)
	os.WriteFile(filepath.Join(root, rpcs3.ExeName), []byte(fakeRPCS3), 0o755)
	dir := filepath.Join(root, "dev_hdd0", "game", "TEST00001")
	os.MkdirAll(filepath.Join(dir, "USRDIR"), 0o755)
	os.WriteFile(filepath.Join(dir, "PARAM.SFO"), rpcs3.BuildSFO(map[string]any{"TITLE": "Test Game", "TITLE_ID": "TEST00001", "CATEGORY": "HG"}), 0o644)
	var b bytes.Buffer
	b.WriteString("SCE\x00")
	b.Write(make([]byte, 0x100))
	b.WriteString("NPD\x00")
	binary.Write(&b, binary.BigEndian, [3]uint32{3, 2, 0}) // version, local licence, kind
	id := make([]byte, 0x30)
	copy(id, ps3TestID)
	b.Write(id)
	os.WriteFile(filepath.Join(dir, "USRDIR", "EBOOT.BIN"), b.Bytes(), 0o644)
	argsFile = filepath.Join(t.TempDir(), "args")
	cfgFile := filepath.Join(t.TempDir(), "rpcs3.json")
	off := false
	for k, v := range map[string]string{"RETRO_RPCS3_CONFIG": cfgFile, "RETRO_RPCS3_HOME": filepath.Join(t.TempDir(), "home"), "FAKE_ARGS": argsFile, "FAKE_FAIL": "", "FAKE_PLAY": "1"} {
		t.Setenv(k, v)
	}
	if _, err := rpcs3.Update(func(c *rpcs3.Config) { c.Folder, c.Embed = root, &off }); err != nil {
		t.Fatal(err)
	}
	ps3Mu.Lock()
	ps3Finding, ps3Searched, ps3Problem, ps3Busy, ps3BusyErr = false, true, nil, "", ""
	ps3Mu.Unlock()
	st := rpcs3.Look(rpcs3.Load(), "")
	if len(st.Games) != 1 {
		t.Fatalf("games: %+v", st)
	}
	return root, argsFile, st.Games[0]
}

func TestPS3PlaysAnInstalledGame(t *testing.T) {
	root, argsFile, g := setupFakePS3(t)
	st := ps3Status()["rpcs3"].(rpcs3.Status)
	if st.Exe != filepath.Join(root, rpcs3.ExeName) || st.Source != rpcs3.SourceChosen || !st.Firmware || st.Games[0].Licence.OK() {
		t.Fatalf("status: %+v", st)
	}
	// Without its licence the game doesn't start, and the reason names the file.
	if err := launchPS3("TEST00001"); err == nil || !strings.Contains(err.Error(), ps3TestID+".rap") {
		t.Fatalf("without a licence: %v", err)
	}
	src := filepath.Join(t.TempDir(), ps3TestID+".rap")
	os.WriteFile(src, make([]byte, 16), 0o644)
	if _, err := rpcs3.AddLicence(root, src); err != nil {
		t.Fatal(err)
	}
	if err := launchPS3("TEST00001"); err != nil {
		t.Fatal(err)
	}
	if err := launchPS3("TEST00001"); err == nil {
		t.Error("started a second game while one is running")
	}
	waitIdle(t)
	args, _ := os.ReadFile(argsFile)
	if got := strings.TrimSpace(string(args)); got != "--no-gui --fullscreen "+g.Eboot {
		t.Fatalf("args: %q", got)
	}
	if h := rpcs3.Player1Handler(root); h != "XInput" {
		t.Errorf("player 1: %q", h)
	}
	if rpcs3.Load().Played["TEST00001"].IsZero() {
		t.Error("not marked as played")
	}
	if launchPS3("NOPE00000") == nil {
		t.Error("started a game that isn't there")
	}
}

func TestPS3SaysWhyAGameDidntStart(t *testing.T) {
	root, _, _ := setupFakePS3(t)
	src := filepath.Join(t.TempDir(), ps3TestID+".rap")
	os.WriteFile(src, make([]byte, 16), 0o644)
	rpcs3.AddLicence(root, src)
	t.Setenv("FAKE_FAIL", "1")
	if err := launchPS3("TEST00001"); err != nil {
		t.Fatal(err)
	}
	waitIdle(t)
	time.Sleep(100 * time.Millisecond)
	p, _ := ps3Status()["problem"].(map[string]any)
	if p == nil || !strings.Contains(p["text"].(string), "licence") {
		t.Fatalf("problem: %v", p)
	}
}

func TestPS3NeedsTheSystemSoftware(t *testing.T) {
	root, _, _ := setupFakePS3(t)
	os.RemoveAll(filepath.Join(root, "dev_flash"))
	if err := launchPS3("TEST00001"); err == nil || !strings.Contains(err.Error(), "PS3UPDAT.PUP") {
		t.Fatalf("%v", err)
	}
}

func TestPS3InstallRunsRPCS3sInstaller(t *testing.T) {
	_, argsFile, _ := setupFakePS3(t)
	t.Setenv("FAKE_PLAY", "0")
	pkg := filepath.Join(t.TempDir(), "game.pkg")
	if err := ps3Install(rpcs3.InstallPkgArgs(pkg), pkg); err != nil {
		t.Fatal(err)
	}
	for i := 0; i < 100; i++ {
		ps3Mu.Lock()
		busy := ps3Busy
		ps3Mu.Unlock()
		if busy == "" {
			break
		}
		time.Sleep(50 * time.Millisecond)
	}
	args, _ := os.ReadFile(argsFile)
	if got := strings.TrimSpace(string(args)); got != "--installpkg "+pkg {
		t.Fatalf("args: %q", got)
	}
}
