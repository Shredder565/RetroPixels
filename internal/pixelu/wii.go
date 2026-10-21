package pixelu

// PixelU's Wii / GameCube engine: Dolphin (GPL-2.0+) built from pixelu/wii
// with a new name. It ships inside the PixelU folder as
// PixelU\wii\PixelU-Wii.exe (PixelU/wii/PixelU-Wii on Linux) with
// portable.txt next to it, so it keeps its settings, memory cards and saves
// in PixelU\wii\User. Its command line and settings files are Dolphin's, so
// internal/dolphin starts it, docks it and records it as before.

import (
	"errors"
	"io"
	"io/fs"
	"os"
	"path/filepath"
	"strings"
	"time"

	"retropixels/internal/dolphin"
)

// WiiDir is the Wii / GameCube engine's folder inside the PixelU folder
// (its program is WiiExeName).
const WiiDir = "wii"

// FindWiiExe returns PixelU's Wii / GameCube engine, looking in the wii
// folder of the PixelU.exe the player chose, then appDir\PixelU\wii (next
// to RetroPixels), then the PixelU folder FindExe finds in Downloads, then
// the newest copy unpacked into Downloads by itself
// (Downloads\PixelU-Wii*\PixelU\wii\PixelU-Wii.exe). "" if there is none.
// A Dolphin.exe installed separately is never used.
func FindWiiExe(chosen, appDir, home string) string {
	isFile := func(p string) bool {
		st, err := os.Stat(p)
		return err == nil && !st.IsDir()
	}
	inPixelU := func(exe string) string {
		if exe == "" {
			return ""
		}
		if p := filepath.Join(filepath.Dir(exe), WiiDir, WiiExeName); isFile(p) {
			return p
		}
		return ""
	}
	if chosen != "" && isFile(chosen) {
		if p := inPixelU(chosen); p != "" {
			return p
		}
	}
	if appDir != "" {
		if p := filepath.Join(appDir, "PixelU", WiiDir, WiiExeName); isFile(p) {
			return p
		}
	}
	if p := inPixelU(FindExe(chosen, appDir, home)); p != "" {
		return p // the PixelU in Downloads that Wii U games use
	}
	if home == "" {
		return ""
	}
	var cands []string
	for _, dl := range dolphin.DownloadDirs(home) {
		for _, pat := range []string{
			filepath.Join(dl, "PixelU*", "PixelU", WiiDir, WiiExeName), // PixelU-Wii-<ver>\PixelU\wii\ (and full PixelU zips)
			filepath.Join(dl, "PixelU*", WiiDir, WiiExeName),
		} {
			m, _ := filepath.Glob(pat)
			cands = append(cands, m...)
		}
	}
	best, bestT := "", time.Time{}
	for _, p := range cands {
		if st, err := os.Stat(p); err == nil && !st.IsDir() && (best == "" || st.ModTime().After(bestT)) {
			best, bestT = p, st.ModTime()
		}
	}
	return best
}

// IsWiiExe tells whether exe is the Wii / GameCube engine (by file name:
// PixelU-Wii.exe, or PixelU-Wii as on Linux).
func IsWiiExe(exe string) bool {
	if i := strings.LastIndexAny(exe, `\/`); i >= 0 {
		exe = exe[i+1:]
	}
	if ext := filepath.Ext(exe); strings.EqualFold(ext, ".exe") {
		exe = exe[:len(exe)-len(ext)]
	}
	return strings.EqualFold(exe, "PixelU-Wii")
}

// ---------------- bringing settings over from Dolphin ----------------

// ImportMarker is written into the engine's User folder once the player's
// old Dolphin settings have been looked at, so it happens only once.
const ImportMarker = "RetroPixels-imported.txt"

// ImportFiles are the settings files brought over (controllers, Dolphin's
// own settings, graphics), each only when PixelU doesn't have it yet.
var ImportFiles = []string{"GCPadNew.ini", "WiimoteNew.ini", "Dolphin.ini", "GFX.ini"}

// ImportDolphinUser brings the player's settings over from their old
// Dolphin user folder (Documents\Dolphin Emulator, ~/.local/share/dolphin-emu
// or ~/.dolphin-emu on Linux, or a portable Dolphin's User folder; see
// dolphin.LegacyUserDir) into the engine's (PixelU\wii\User), the first
// time only:
//   - Config\GCPadNew.ini, WiimoteNew.ini, Dolphin.ini and GFX.ini (from
//     dolphin.ConfigDir(oldUser)), each only if PixelU has none yet;
//   - GameCube memory cards and the like (everything in GC\), file by file,
//     only where PixelU has no file of that name;
//   - Wii saves (Wii\title\<kind>\<game>), a whole game at a time, only for
//     games PixelU has nothing for.
//
// Nothing in newUser is ever overwritten and oldUser is only read. It
// returns what was copied (paths relative to the User folder).
func ImportDolphinUser(oldUser, newUser string) ([]string, error) {
	if newUser == "" {
		return nil, errors.New("no user folder")
	}
	marker := filepath.Join(newUser, ImportMarker)
	if _, err := os.Stat(marker); err == nil {
		return nil, nil // done before
	}
	if err := os.MkdirAll(newUser, 0o755); err != nil {
		return nil, err
	}
	var copied []string
	var firstErr error
	note := func(rel string, err error) {
		if err != nil {
			if firstErr == nil {
				firstErr = err
			}
			return
		}
		copied = append(copied, filepath.ToSlash(rel))
	}
	// An installed Dolphin on Linux keeps its settings files apart from its
	// data (~/.config/dolphin-emu), and may have only those; everywhere else
	// they're in oldUser\Config.
	oldConfig := dolphin.ConfigDir(oldUser)
	if oldUser != "" && (isDirPath(oldUser) || isDirPath(oldConfig)) && !samePath(oldUser, newUser) {
		for _, name := range ImportFiles {
			rel := filepath.Join("Config", name)
			if exists(filepath.Join(newUser, rel)) || !isRegular(filepath.Join(oldConfig, name)) {
				continue
			}
			note(rel, copyFile(filepath.Join(oldConfig, name), filepath.Join(newUser, rel)))
		}
		// GameCube memory cards (Card A.USA.raw, GCI folders, ...).
		gc := filepath.Join(oldUser, "GC")
		filepath.WalkDir(gc, func(p string, d fs.DirEntry, err error) error {
			if err != nil || !d.Type().IsRegular() {
				return nil // folders are walked, links and the rest left out
			}
			rel, _ := filepath.Rel(oldUser, p)
			if dst := filepath.Join(newUser, rel); !exists(dst) {
				note(rel, copyFile(p, dst))
			}
			return nil
		})
		// Wii saves: one folder per game under Wii\title\<kind>\.
		kinds, _ := os.ReadDir(filepath.Join(oldUser, "Wii", "title"))
		for _, k := range kinds {
			if !k.IsDir() {
				continue
			}
			games, _ := os.ReadDir(filepath.Join(oldUser, "Wii", "title", k.Name()))
			for _, g := range games {
				if !g.IsDir() {
					continue
				}
				rel := filepath.Join("Wii", "title", k.Name(), g.Name())
				if exists(filepath.Join(newUser, rel)) {
					continue
				}
				note(rel, copyDir(filepath.Join(oldUser, rel), filepath.Join(newUser, rel)))
			}
		}
	}
	if firstErr != nil {
		return copied, firstErr // try again next time (nothing copied is copied twice)
	}
	text := "RetroPixels looked for your Dolphin settings to bring over here on " + time.Now().Format("2 Jan 2006 15:04") + ".\r\n"
	if len(copied) == 0 {
		text += "Nothing needed copying.\r\n"
	} else {
		text += "Copied from " + oldUser + " (nothing here was overwritten):\r\n  " + strings.Join(copied, "\r\n  ") + "\r\n"
	}
	text += "Delete this file to have RetroPixels look again (it still never overwrites anything).\r\n"
	if err := os.WriteFile(marker, []byte(text), 0o644); err != nil {
		return copied, err
	}
	return copied, nil
}

func exists(p string) bool { _, err := os.Lstat(p); return err == nil }

func isDirPath(p string) bool {
	st, err := os.Stat(p)
	return err == nil && st.IsDir()
}

func isRegular(p string) bool {
	st, err := os.Lstat(p)
	return err == nil && st.Mode().IsRegular()
}

func samePath(a, b string) bool {
	ca, cb := filepath.Clean(a), filepath.Clean(b)
	if ra, err := filepath.EvalSymlinks(ca); err == nil {
		ca = ra
	}
	if rb, err := filepath.EvalSymlinks(cb); err == nil {
		cb = rb
	}
	return strings.EqualFold(ca, cb)
}

// copyFile copies one file through a temporary name, so a half-copied file
// is never left under the real name, and never replaces one that's there.
func copyFile(src, dst string) error {
	if exists(dst) {
		return nil
	}
	if err := os.MkdirAll(filepath.Dir(dst), 0o755); err != nil {
		return err
	}
	in, err := os.Open(src)
	if err != nil {
		return err
	}
	defer in.Close()
	st, err := in.Stat()
	if err != nil {
		return err
	}
	tmp := dst + ".retropixels-tmp"
	out, err := os.Create(tmp)
	if err != nil {
		return err
	}
	_, err = io.Copy(out, in)
	if cerr := out.Close(); err == nil {
		err = cerr
	}
	if err != nil {
		os.Remove(tmp)
		return err
	}
	os.Chtimes(tmp, st.ModTime(), st.ModTime())
	if exists(dst) {
		os.Remove(tmp)
		return nil
	}
	if err := os.Rename(tmp, dst); err != nil {
		os.Remove(tmp)
		return err
	}
	return nil
}

// copyDir copies a folder (regular files only) next to dst first, then
// moves it into place, so a game's save is either all there or not at all.
func copyDir(src, dst string) error {
	if exists(dst) {
		return nil
	}
	tmp := dst + ".retropixels-tmp"
	os.RemoveAll(tmp)
	err := filepath.WalkDir(src, func(p string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		rel, _ := filepath.Rel(src, p)
		switch {
		case d.IsDir():
			return os.MkdirAll(filepath.Join(tmp, rel), 0o755)
		case d.Type().IsRegular():
			return copyFile(p, filepath.Join(tmp, rel))
		}
		return nil
	})
	if err == nil && exists(dst) {
		err = os.RemoveAll(tmp)
		return err
	}
	if err == nil {
		err = os.Rename(tmp, dst)
	}
	if err != nil {
		os.RemoveAll(tmp)
	}
	return err
}
