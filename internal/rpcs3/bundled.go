package rpcs3

// The RPCS3 that comes with RetroPixels (the RPCS3 folder next to
// RetroPixels.exe, the RPCS3 team's official Windows build, unchanged).
//
// A RetroPixels download is replaced on every update, but RPCS3 keeps the
// player's system software (dev_flash), installed games and saves
// (dev_hdd0), licences, settings (config) and games.yml inside its own
// folder. So the bundled copy is never run where it is: on first use it is
// copied to a lasting folder (%APPDATA%\RetroPixels\RPCS3, BundledHome) and
// run from there. When a newer RetroPixels brings a newer RPCS3, only
// RPCS3's program files (rpcs3.exe, its DLLs, Qt plugins, themes, icons…)
// are replaced; the player's folders and settings files are never written
// over or deleted (Protected).

import (
	"bufio"
	"errors"
	"io"
	"io/fs"
	"os"
	"path"
	"path/filepath"
	"sort"
	"strings"
	"sync"
)

// BundleMarker is the file bundle-test.yml puts in the bundled RPCS3 folder
// (which RPCS3 build it is). A folder with it is RetroPixels' bundled copy,
// never a player's own RPCS3. It's copied to BundledHome last, so a copy
// that was cut off is finished the next time.
const BundleMarker = "RETROPIXELS-BUNDLED.txt"

// bundleManifest lists the program files RetroPixels put in BundledHome
// (slash paths, one a line), so an update can remove the ones the newer
// build doesn't have. Only files on that list are ever removed.
const bundleManifest = "RETROPIXELS-FILES.txt"

// BundledHome is where the bundled RPCS3 lives and runs:
// %APPDATA%\RetroPixels\RPCS3 (next to the Supermodel folder).
// RETRO_RPCS3_HOME overrides it, for tests.
func BundledHome() string {
	if d := os.Getenv("RETRO_RPCS3_HOME"); d != "" {
		return d
	}
	d, err := os.UserConfigDir()
	if err != nil {
		d = "."
	}
	return filepath.Join(d, AppFolder, "RPCS3")
}

// IsBundle says whether dir is a RetroPixels bundled RPCS3 (it has
// BundleMarker): such a folder is only copied from, never run.
func IsBundle(dir string) bool { return isFile(filepath.Join(dir, BundleMarker)) }

// BundledSource is the bundled RPCS3 folder next to RetroPixels (appDir),
// or "" when this download has none.
func BundledSource(appDir string) string {
	if appDir == "" {
		return ""
	}
	d := filepath.Join(appDir, "RPCS3")
	if IsBundle(d) && ExeIn(d) != "" {
		return d
	}
	return ""
}

// BundledVersion is the first line of a bundled RPCS3's marker ("" when
// dir isn't one), e.g. "RPCS3 0.0.43-20267 (ff91404b)".
func BundledVersion(dir string) string {
	f, err := os.Open(filepath.Join(dir, BundleMarker))
	if err != nil {
		return ""
	}
	defer f.Close()
	s := bufio.NewScanner(f)
	if s.Scan() {
		return strings.TrimSpace(s.Text())
	}
	return ""
}

// protectedDirs are RPCS3's folders for the player's things: the system
// software, games, saves, licences, settings, caches and captures. Any
// folder starting "dev_" is protected too.
var protectedDirs = map[string]bool{
	"config": true, "custom_configs": true, "cache": true, "shaderlog": true,
	"savestates": true, "captures": true, "patches": true, "logs": true,
	"screenshots": true, "recordings": true, "data": true, "games": true,
}

// protectedExts are settings and record files, wherever they are
// (games.yml, GuiConfigs\CurrentSettings.ini, RPCS3.log…).
var protectedExts = map[string]bool{".yml": true, ".yaml": true, ".ini": true, ".log": true, ".json": true, ".rap": true, ".edat": true}

// Protected says whether rel (a path inside RPCS3's folder) is the
// player's: such a file is never replaced or deleted by an update; it is
// only put there when it is missing.
func Protected(rel string) bool {
	rel = strings.ToLower(filepath.ToSlash(rel))
	first := strings.SplitN(rel, "/", 2)[0]
	if strings.HasPrefix(first, "dev_") || protectedDirs[first] {
		return true
	}
	if rel == strings.ToLower(BundleMarker) || rel == strings.ToLower(bundleManifest) {
		return false
	}
	return protectedExts[path.Ext(rel)]
}

var installMu sync.Mutex

// InstallBundled copies the bundled RPCS3 in src to dst (BundledHome) the
// first time, and again when src is a different build (its BundleMarker
// differs), replacing only program files. It returns RPCS3's program in
// dst and whether anything was copied. The player's files in dst (Protected,
// or anything RetroPixels didn't put there) are never changed or removed.
func InstallBundled(src, dst string) (exe string, changed bool, err error) {
	installMu.Lock()
	defer installMu.Unlock()
	want, err := os.ReadFile(filepath.Join(src, BundleMarker))
	if err != nil {
		return "", false, errors.New("the bundled RPCS3 has no " + BundleMarker)
	}
	if ExeIn(src) == "" {
		return "", false, errors.New(ExeName + " isn't in the bundled RPCS3 folder " + src)
	}
	if have, err := os.ReadFile(filepath.Join(dst, BundleMarker)); err == nil && string(have) == string(want) && ExeIn(dst) != "" {
		return ExeIn(dst), false, nil
	}
	if err := os.MkdirAll(dst, 0o755); err != nil {
		return "", false, err
	}
	old := readManifest(filepath.Join(dst, bundleManifest))
	var files []string
	err = filepath.WalkDir(src, func(p string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if !d.Type().IsRegular() {
			return nil // folders are made as needed; links are left out
		}
		rel, err := filepath.Rel(src, p)
		if err != nil {
			return err
		}
		slash := filepath.ToSlash(rel)
		if strings.EqualFold(slash, BundleMarker) || strings.EqualFold(slash, bundleManifest) {
			return nil
		}
		to := filepath.Join(dst, rel)
		if Protected(slash) {
			if _, err := os.Lstat(to); err == nil {
				return nil // the player's: left as it is
			}
			return copyFile(p, to)
		}
		if st, err := os.Lstat(to); err == nil && !st.Mode().IsRegular() {
			return nil // something of the player's with that name: left alone
		}
		files = append(files, slash)
		return copyFile(p, to)
	})
	if err != nil {
		return "", true, err
	}
	// Program files of the build before that this one doesn't have.
	now := map[string]bool{}
	for _, f := range files {
		now[strings.ToLower(f)] = true
	}
	for _, f := range old {
		if now[strings.ToLower(f)] || Protected(f) || !safeRel(f) {
			continue
		}
		p := filepath.Join(dst, filepath.FromSlash(f))
		if st, err := os.Lstat(p); err == nil && st.Mode().IsRegular() {
			_ = os.Remove(p)
		}
	}
	sort.Strings(files)
	if err := writeAtomic(filepath.Join(dst, bundleManifest), []byte(strings.Join(files, "\n")+"\n")); err != nil {
		return "", true, err
	}
	if err := writeAtomic(filepath.Join(dst, BundleMarker), want); err != nil {
		return "", true, err
	}
	exe = ExeIn(dst)
	if exe == "" {
		return "", true, errors.New(ExeName + " didn't arrive in " + dst)
	}
	return exe, true, nil
}

// BundledExe is the bundled RPCS3 ready to run from BundledHome: copied or
// brought up to date from the one next to RetroPixels (appDir) first. When
// the update can't be made (RPCS3 is open from there, say), the copy
// already there is used. "" when there's no bundled RPCS3 at all.
func BundledExe(appDir string) string {
	home := BundledHome()
	src := BundledSource(appDir)
	if src == "" || sameDir(src, home) {
		if IsBundle(home) {
			return ExeIn(home)
		}
		return ""
	}
	exe, _, err := InstallBundled(src, home)
	if err != nil && IsBundle(home) {
		return ExeIn(home)
	}
	return exe
}

func sameDir(a, b string) bool {
	a, _ = filepath.Abs(a)
	b, _ = filepath.Abs(b)
	return strings.EqualFold(filepath.Clean(a), filepath.Clean(b))
}

func safeRel(rel string) bool {
	if rel == "" || strings.HasPrefix(rel, "/") || filepath.IsAbs(rel) || strings.Contains(rel, ":") || strings.Contains(rel, `\`) {
		return false
	}
	for _, part := range strings.Split(rel, "/") {
		if part == ".." || part == "." || part == "" {
			return false
		}
	}
	return true
}

func readManifest(p string) []string {
	b, err := os.ReadFile(p)
	if err != nil {
		return nil
	}
	var out []string
	for _, l := range strings.Split(string(b), "\n") {
		if l = strings.TrimSpace(l); l != "" {
			out = append(out, l)
		}
	}
	return out
}

func copyFile(from, to string) error {
	if err := os.MkdirAll(filepath.Dir(to), 0o755); err != nil {
		return err
	}
	in, err := os.Open(from)
	if err != nil {
		return err
	}
	defer in.Close()
	st, err := in.Stat()
	if err != nil {
		return err
	}
	tmp := to + ".retropixels-new"
	out, err := os.OpenFile(tmp, os.O_WRONLY|os.O_CREATE|os.O_TRUNC, st.Mode().Perm()|0o200)
	if err != nil {
		return err
	}
	if _, err := io.Copy(out, in); err != nil {
		out.Close()
		os.Remove(tmp)
		return err
	}
	if err := out.Close(); err != nil {
		os.Remove(tmp)
		return err
	}
	if err := os.Rename(tmp, to); err != nil {
		os.Remove(tmp)
		return err
	}
	return nil
}

func writeAtomic(p string, b []byte) error {
	tmp := p + ".retropixels-new"
	if err := os.WriteFile(tmp, b, 0o644); err != nil {
		return err
	}
	return os.Rename(tmp, p)
}
