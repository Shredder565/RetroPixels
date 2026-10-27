// Package rpcs3 runs PlayStation 3 games in RPCS3, the open source PS3
// emulator (GPL v2, https://rpcs3.net). The RetroPixels download comes with
// the RPCS3 team's official Windows build, unchanged, in the RPCS3 folder
// next to RetroPixels-Native.exe. RPCS3 there is portable: its settings, the
// PS3 system software and the installed games live in that folder.
//
// RetroPixels has its own PS3 emulator too (the ps3 package); RPCS3 plays the
// games until that one can.
package rpcs3

import (
	"bytes"
	"io/fs"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"time"
)

// ExeName is RPCS3's program.
const ExeName = "rpcs3.exe"

// BundleDir is the RPCS3 folder next to the program (RETRO_RPCS3_BUNDLE
// overrides it, for tests).
func BundleDir() string {
	if d := os.Getenv("RETRO_RPCS3_BUNDLE"); d != "" {
		return d
	}
	exe, err := os.Executable()
	if err != nil {
		return ""
	}
	return filepath.Join(filepath.Dir(exe), "RPCS3")
}

// Exe is the bundled rpcs3.exe, or "" if it isn't there.
func Exe() string {
	d := BundleDir()
	if d == "" {
		return ""
	}
	p := filepath.Join(d, ExeName)
	if st, err := os.Stat(p); err == nil && !st.IsDir() {
		return p
	}
	return ""
}

// HasFirmware tells whether the PS3 system software is installed in RPCS3's
// folder dir.
func HasFirmware(dir string) bool {
	st, err := os.Stat(filepath.Join(dir, "dev_flash", "vsh", "module", "vsh.self"))
	return err == nil && st.Size() > 0
}

// IsGameFile tells whether path is something RPCS3 opens: a PSN package, a
// game's PARAM.SFO or EBOOT.BIN.
func IsGameFile(path string) bool {
	switch strings.ToLower(filepath.Base(path)) {
	case "param.sfo", "eboot.bin":
		return true
	}
	return strings.EqualFold(filepath.Ext(path), ".pkg")
}

// PkgTitleID reads the title id (e.g. NPEB00149) from a PSN package's
// content id (e.g. EP4062-NPEB00149_00-...).
func PkgTitleID(path string) string {
	f, err := os.Open(path)
	if err != nil {
		return ""
	}
	defer f.Close()
	var h [0x60]byte
	if _, err := f.ReadAt(h[:], 0); err != nil {
		return ""
	}
	if !bytes.Equal(h[:4], []byte{0x7f, 'P', 'K', 'G'}) {
		return ""
	}
	id := string(bytes.TrimRight(h[0x30:0x54], "\x00"))
	if len(id) < 16 || id[6] != '-' {
		return ""
	}
	return id[7:16]
}

// GameEboot is the installed game's EBOOT.BIN in RPCS3's folder dir, or ""
// if the game isn't installed there.
func GameEboot(dir, titleID string) string {
	if titleID == "" {
		return ""
	}
	p := filepath.Join(dir, "dev_hdd0", "game", titleID, "USRDIR", "EBOOT.BIN")
	if st, err := os.Stat(p); err == nil && st.Size() > 0 {
		return p
	}
	return ""
}

// Eboot is the EBOOT.BIN to start for a PARAM.SFO or EBOOT.BIN path.
func Eboot(path string) string {
	if strings.EqualFold(filepath.Base(path), "eboot.bin") {
		return path
	}
	dir := filepath.Dir(path)
	for _, p := range []string{filepath.Join(dir, "USRDIR", "EBOOT.BIN"), filepath.Join(dir, "PS3_GAME", "USRDIR", "EBOOT.BIN")} {
		if _, err := os.Stat(p); err == nil {
			return p
		}
	}
	return ""
}

// Command makes the command that runs RPCS3; it starts in RPCS3's folder.
func Command(exe string, args ...string) *exec.Cmd {
	cmd := exec.Command(exe, args...)
	cmd.Dir = filepath.Dir(exe)
	return cmd
}

// PlayArgs starts the game at eboot without RPCS3's own window around it.
func PlayArgs(eboot string) []string { return []string{"--no-gui", eboot} }

// InstallFirmwareArgs installs the PS3 system software file (PS3UPDAT.PUP).
func InstallFirmwareArgs(pup string) []string { return []string{"--installfw", pup} }

// InstallPkgArgs installs a PSN package.
func InstallPkgArgs(pkg string) []string { return []string{"--installpkg", pkg} }

// Settled waits until the files under dir stop changing (count and sizes the
// same for quiet), done reports true, or until stop is closed or timeout
// passes. It returns whether done held once the files settled.
func Settled(dir string, done func() bool, quiet, timeout time.Duration, stop <-chan struct{}) bool {
	deadline := time.Now().Add(timeout)
	last, since := int64(-1), time.Now()
	for time.Now().Before(deadline) {
		select {
		case <-stop:
			return done()
		case <-time.After(500 * time.Millisecond):
		}
		n := treeSize(dir)
		if n != last {
			last, since = n, time.Now()
			continue
		}
		if done() && time.Since(since) >= quiet {
			return true
		}
	}
	return done()
}

// treeSize: files and bytes under dir, as one number that changes when
// either does.
func treeSize(dir string) int64 {
	var n int64
	filepath.WalkDir(dir, func(_ string, d fs.DirEntry, err error) error {
		if err != nil || d.IsDir() {
			return nil
		}
		if info, err := d.Info(); err == nil {
			n += info.Size() + 1<<40
		}
		return nil
	})
	return n
}

// guiSettings turns off RPCS3's welcome screen, the boxes saying an install
// worked and the "exit the game?" question, which would stop RetroPixels'
// installs and its Stop game.
const guiSettings = "[main_window]\r\ninfoBoxEnabledWelcome=false\r\ninfoBoxEnabledInstallPKG=false\r\ninfoBoxEnabledInstallPUP=false\r\nconfirmationBoxExitGame=false\r\nconfirmationBoxBootGame=false\r\n"

// audioMark records that RetroPixels has set RPCS3's sound output once, so
// a later change made in RPCS3 itself is kept.
const audioMark = "RETROPIXELS-AUDIO.txt"

// PrepareSettings writes those settings in RPCS3's folder dir the first
// time (RPCS3's own settings stay as they are after that). It also moves
// RPCS3's sound output from Cubeb to XAudio2 once: with Cubeb, RPCS3 crashed
// (0xc0000005) when a game reopened the sound output.
func PrepareSettings(dir string) error {
	p := filepath.Join(dir, "GuiConfigs", "CurrentSettings.ini")
	if _, err := os.Stat(p); err != nil {
		if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
			return err
		}
		if err := os.WriteFile(p, []byte(guiSettings), 0o644); err != nil {
			return err
		}
	}
	return prepareAudio(dir)
}

func prepareAudio(dir string) error {
	mark := filepath.Join(dir, audioMark)
	if _, err := os.Stat(mark); err == nil {
		return nil
	}
	cfg := filepath.Join(dir, "config", "config.yml")
	b, err := os.ReadFile(cfg)
	switch {
	case os.IsNotExist(err):
		if err := os.MkdirAll(filepath.Dir(cfg), 0o755); err != nil {
			return err
		}
		b = []byte("Audio:\n  Renderer: XAudio2\n")
	case err != nil:
		return err
	default:
		b = SetAudioRenderer(b, "XAudio2")
	}
	if err := os.WriteFile(cfg, b, 0o644); err != nil {
		return err
	}
	return os.WriteFile(mark, []byte("RetroPixels set RPCS3's sound output to XAudio2 once. Change it in RPCS3 if you like; RetroPixels won't change it again.\r\n"), 0o644)
}

// SetAudioRenderer sets "Renderer:" in the Audio section of RPCS3's
// config.yml text, adding the section if it's missing.
func SetAudioRenderer(b []byte, renderer string) []byte {
	nl := "\n"
	if bytes.Contains(b, []byte("\r\n")) {
		nl = "\r\n"
	}
	lines := strings.Split(strings.ReplaceAll(string(b), "\r\n", "\n"), "\n")
	in := false
	for i, l := range lines {
		if len(l) > 0 && l[0] != ' ' {
			in = strings.TrimSpace(l) == "Audio:"
			continue
		}
		if in && strings.HasPrefix(l, "  Renderer:") {
			lines[i] = "  Renderer: " + renderer
			return []byte(strings.Join(lines, nl))
		}
	}
	for i, l := range lines {
		if strings.TrimSpace(l) == "Audio:" && !strings.HasPrefix(l, " ") {
			lines = append(lines[:i+1], append([]string{"  Renderer: " + renderer}, lines[i+1:]...)...)
			return []byte(strings.Join(lines, nl))
		}
	}
	s := strings.Join(lines, nl)
	if s != "" && !strings.HasSuffix(s, nl) {
		s += nl
	}
	return []byte(s + "Audio:" + nl + "  Renderer: " + renderer + nl)
}

// GameTitle reads the game's title from the PARAM.SFO beside its USRDIR.
func GameTitle(eboot string) string {
	b, err := os.ReadFile(filepath.Join(filepath.Dir(filepath.Dir(eboot)), "PARAM.SFO"))
	if err != nil || len(b) < 20 || string(b[1:4]) != "PSF" {
		return ""
	}
	le32 := func(o int) int { return int(b[o]) | int(b[o+1])<<8 | int(b[o+2])<<16 | int(b[o+3])<<24 }
	keys, data, n := le32(8), le32(12), le32(16)
	for i := 0; i < n && 20+i*16+16 <= len(b); i++ {
		e := 20 + i*16
		ko := keys + (int(b[e]) | int(b[e+1])<<8)
		end := ko
		for end < len(b) && b[end] != 0 {
			end++
		}
		if ko >= len(b) || string(b[ko:end]) != "TITLE" {
			continue
		}
		vo, vl := data+le32(e+12), le32(e+4)
		if vo+vl > len(b) {
			return ""
		}
		return strings.TrimRight(strings.ReplaceAll(string(b[vo:vo+vl]), "\n", " "), "\x00")
	}
	return ""
}
