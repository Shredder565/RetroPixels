// bundlemame puts MAME into the RetroPixels download.
//
// Windows (the default, -os windows): the MAME team's official Windows
// release, as it is. It downloads the release from MAME's GitHub, checks it
// against the SHA-256 GitHub lists for it, unpacks it (without running it)
// into <dir>/MAME 0.xxx and marks it as built in. It also saves MAME's source
// code for that release, for the GitHub release.
//
//	go run ./tools/bundlemame -dir dist/RetroPixels-2.17.0/MAME -source dist/MAME-0.281-source.tar.gz
//
// Linux (-os linux): the MAME team publishes no Linux build, so
// .github/workflows/mame-linux.yml builds the same official release from its
// source (tools/bundlemame/build-linux.sh) and this places that build, a
// folder holding "mame" and the RETROPIXELS-BUILD.txt the build wrote, into
// <dir>/MAME 0.xxx, marks it as built in and adds the roms folder. It needs
// no network (unless -source is given too).
//
//	go run ./tools/bundlemame -os linux -dir out/MAME -build build/stage
//
// -resolve only finds the release (the newest Windows release, or -version)
// and writes version=, tag= and sha256= to -github-output (with -source it
// also saves the source code and adds source_sha256=), so the Linux build
// builds the same release the Windows download has.
//
// -version picks a release ("0.281"); the default is the newest. GITHUB_TOKEN,
// when set, is sent to api.github.com only (CI runners share GitHub's limits).
package main

import (
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"flag"
	"fmt"
	"io"
	"io/fs"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"time"

	"retropixels/internal/mame"
)

type tokenTransport struct{ token string }

func (t tokenTransport) RoundTrip(r *http.Request) (*http.Response, error) {
	if t.token != "" && r.URL.Host == "api.github.com" {
		r = r.Clone(r.Context())
		r.Header.Set("Authorization", "Bearer "+t.token)
	}
	return http.DefaultTransport.RoundTrip(r)
}

// BuildInfo is the file build-linux.sh writes next to the mame it built.
const BuildInfo = "RETROPIXELS-BUILD.txt"

func main() {
	osName := flag.String("os", "windows", "the system the package is for: windows or linux")
	dir := flag.String("dir", "", "the MAME folder inside the package")
	version := flag.String("version", "", "the MAME version (default: the newest; for -os linux, the build's)")
	source := flag.String("source", "", "where to save MAME's source code (.tar.gz)")
	build := flag.String("build", "", "-os linux: the folder build-linux.sh made (mame and "+BuildInfo+")")
	resolve := flag.Bool("resolve", false, "only find the release (and save its source with -source)")
	ghOut := flag.String("github-output", os.Getenv("GITHUB_OUTPUT"), "file to write version=, tag=, sha256= and source= to")
	flag.Parse()

	out := map[string]string{}
	var order []string
	set := func(k, v string) {
		if _, ok := out[k]; !ok {
			order = append(order, k)
		}
		out[k] = v
	}
	switch {
	case *resolve:
		rel := findRelease(*version)
		set("version", rel.Version)
		set("tag", rel.Tag)
		set("sha256", rel.SHA256)
		if *source != "" {
			sum := saveSource(rel.Tag, *source)
			set("source", *source)
			set("source_sha256", sum)
		}
	case *osName == "linux":
		if *dir == "" || *build == "" {
			fail(errors.New("-os linux needs -dir and -build"))
		}
		info, err := placeLinux(*build, *dir, *version)
		if err != nil {
			fail(err)
		}
		set("version", info["version"])
		set("tag", info["tag"])
		if *source != "" {
			sum := saveSource(info["tag"], *source)
			if want := info["source_sha256"]; want != "" && want != sum {
				fail(fmt.Errorf("the source code downloaded now (sha256 %s) isn't what MAME was built from (sha256 %s)", sum, want))
			}
			set("source", *source)
			set("source_sha256", sum)
		}
	case *osName == "windows":
		if *dir == "" {
			fail(errors.New("-dir is needed"))
		}
		rel := placeWindows(*dir, *version)
		set("version", rel.Version)
		set("tag", rel.Tag)
		set("source", *source)
		set("sha256", rel.SHA256)
		if *source != "" {
			set("source_sha256", saveSource(rel.Tag, *source))
		}
	default:
		fail(fmt.Errorf("-os %s: windows or linux", *osName))
	}
	if *ghOut != "" {
		f, err := os.OpenFile(*ghOut, os.O_APPEND|os.O_CREATE|os.O_WRONLY, 0o644)
		if err != nil {
			fail(err)
		}
		for _, k := range order {
			fmt.Fprintf(f, "%s=%s\n", k, out[k])
		}
		f.Close()
	}
}

func installer() *mame.Installer {
	in := mame.NewInstaller()
	in.Client = &http.Client{Timeout: 30 * time.Minute, Transport: tokenTransport{os.Getenv("GITHUB_TOKEN")}}
	return in
}

// findRelease: the newest official Windows release, or that version.
func findRelease(version string) mame.Release {
	rels, err := installer().Releases()
	if err != nil {
		fail(err)
	}
	rel := rels[0] // newest first
	if version != "" {
		rel = mame.Release{}
		for _, r := range rels {
			if r.Version == strings.TrimPrefix(version, "v") {
				rel = r
			}
		}
		if rel.Version == "" {
			fail(fmt.Errorf("MAME %s isn't among the official Windows downloads", version))
		}
	}
	fmt.Printf("MAME %s: %s (%d MB, sha256 %s)\n", rel.Version, rel.URL, rel.Size>>20, rel.SHA256)
	return rel
}

func placeWindows(dir, version string) mame.Release {
	rel := findRelease(version)
	if rel.SHA256 == "" {
		fail(fmt.Errorf("GitHub lists no checksum for %s, so it can't be checked", rel.URL))
	}
	in := installer()
	in.Dirs = []string{dir}
	last := time.Now()
	in.Progress = func(p mame.Progress) {
		if time.Since(last) > 5*time.Second {
			last = time.Now()
			fmt.Printf("  %s: %d of %d MB\n", p.State, p.Got>>20, p.Total>>20)
		}
	}
	exe, err := in.Install(rel)
	if err != nil {
		fail(err)
	}
	folder := filepath.Dir(exe)
	mark := "version=" + rel.Version + "\r\n" +
		"This MAME came in the RetroPixels download: the MAME team's official release " + rel.Tag + ",\r\n" +
		"unchanged (" + rel.URL + ",\r\nSHA-256 " + rel.SHA256 + ").\r\n" +
		"Its source code: https://github.com/mamedev/mame/tree/" + rel.Tag + "\r\n"
	if err := os.WriteFile(filepath.Join(folder, mame.BuiltInMarker), []byte(mark), 0o644); err != nil {
		fail(err)
	}
	if err := romsFolder(dir, "\r\n"); err != nil {
		fail(err)
	}
	fmt.Println("unpacked to", folder)
	return rel
}

func romsFolder(dir, nl string) error {
	roms := filepath.Join(dir, "roms")
	if err := os.MkdirAll(roms, 0o755); err != nil {
		return err
	}
	readme := "Put your MAME ROM sets here (one .zip per game, named as MAME names it," + nl +
		"for example pacman.zip), then open Arcade (MAME) in RetroPixels." + nl +
		"No games come with RetroPixels." + nl
	return os.WriteFile(filepath.Join(roms, "PUT-ROM-SETS-HERE.txt"), []byte(readme), 0o644)
}

// readBuildInfo reads RETROPIXELS-BUILD.txt: key=value lines.
func readBuildInfo(path string) (map[string]string, error) {
	b, err := os.ReadFile(path)
	if err != nil {
		return nil, fmt.Errorf("the build has no %s: %v", BuildInfo, err)
	}
	info := map[string]string{}
	for _, l := range strings.Split(string(b), "\n") {
		if k, v, ok := strings.Cut(strings.TrimSpace(l), "="); ok && k != "" && !strings.HasPrefix(k, "#") {
			info[k] = v
		}
	}
	return info, nil
}

// placeLinux copies the folder build-linux.sh made into dir/MAME 0.xxx and
// marks it as built in. It returns the build's facts.
func placeLinux(build, dir, version string) (map[string]string, error) {
	info, err := readBuildInfo(filepath.Join(build, BuildInfo))
	if err != nil {
		return nil, err
	}
	v, tag := info["version"], info["tag"]
	if v == "" || mame.TagVersion(tag) != v {
		return nil, fmt.Errorf("%s names no MAME release (version=%q, tag=%q)", BuildInfo, v, tag)
	}
	if version != "" && strings.TrimPrefix(version, "v") != v {
		return nil, fmt.Errorf("the build is MAME %s, not %s", v, version)
	}
	st, err := os.Stat(filepath.Join(build, "mame"))
	if err != nil || !st.Mode().IsRegular() || st.Mode().Perm()&0o111 == 0 {
		return nil, errors.New("the build has no executable mame")
	}
	dest := filepath.Join(dir, "MAME "+v)
	if err := os.RemoveAll(dest); err != nil {
		return nil, err
	}
	if err := copyTree(build, dest); err != nil {
		return nil, err
	}
	src := "https://github.com/mamedev/mame/archive/refs/tags/" + url.PathEscape(tag) + ".tar.gz"
	mark := "version=" + v + "\n" +
		"This MAME came in the RetroPixels download for Linux. The MAME team publishes\n" +
		"ready-made builds for Windows only, so RetroPixels built it from the MAME team's\n" +
		"official source code of release " + tag + ", unchanged:\n" +
		src + "\n"
	if s := info["source_sha256"]; s != "" {
		mark += "(SHA-256 " + s + ")\n"
	}
	mark += "with SUBTARGET=" + info["subtarget"] + " (" + describeSubtarget(info["subtarget"]) + ").\n" +
		"How it was built: " + BuildInfo + " in this folder.\n" +
		"Its source code: https://github.com/mamedev/mame/tree/" + tag + "\n"
	if err := os.WriteFile(filepath.Join(dest, mame.BuiltInMarker), []byte(mark), 0o644); err != nil {
		return nil, err
	}
	if err := romsFolder(dir, "\n"); err != nil {
		return nil, err
	}
	fmt.Printf("MAME %s (%s, built from source) placed in %s\n", v, tag, dest)
	return info, nil
}

func describeSubtarget(s string) string {
	switch s {
	case "arcade":
		return "every arcade machine MAME has"
	case "", "mame":
		return "all of MAME"
	}
	return "some of MAME's machines"
}

// copyTree copies a folder, keeping the files' permission bits. Links are
// refused: the build has none, and the package mustn't point outside itself.
func copyTree(src, dst string) error {
	return filepath.WalkDir(src, func(p string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		rel, err := filepath.Rel(src, p)
		if err != nil {
			return err
		}
		to := filepath.Join(dst, rel)
		info, err := d.Info()
		if err != nil {
			return err
		}
		switch {
		case d.IsDir():
			return os.MkdirAll(to, 0o755)
		case info.Mode().IsRegular():
			return copyFile(p, to, info.Mode().Perm())
		}
		return fmt.Errorf("%s isn't a plain file or folder", p)
	})
}

func copyFile(src, dst string, perm fs.FileMode) error {
	in, err := os.Open(src)
	if err != nil {
		return err
	}
	defer in.Close()
	out, err := os.OpenFile(dst, os.O_CREATE|os.O_TRUNC|os.O_WRONLY, perm)
	if err != nil {
		return err
	}
	if _, err := io.Copy(out, in); err != nil {
		out.Close()
		return err
	}
	if err := out.Close(); err != nil {
		return err
	}
	return os.Chmod(dst, perm)
}

// saveSource saves the release's source code and returns its SHA-256.
func saveSource(tag, dst string) string {
	link := "https://github.com/mamedev/mame/archive/refs/tags/" + url.PathEscape(tag) + ".tar.gz"
	n, sum, err := download(link, dst)
	if err != nil {
		fail(fmt.Errorf("MAME's source code: %v", err))
	}
	fmt.Printf("source code: %s (%d MB, sha256 %s)\n", dst, n>>20, sum)
	return sum
}

// download saves a GitHub source archive (github.com, then GitHub's codeload).
func download(link, dst string) (int64, string, error) {
	c := &http.Client{Timeout: 30 * time.Minute, CheckRedirect: func(r *http.Request, via []*http.Request) error {
		if r.URL.Scheme != "https" || (r.URL.Host != "github.com" && r.URL.Host != "codeload.github.com") || len(via) > 5 {
			return errors.New("redirected somewhere unexpected: " + r.URL.String())
		}
		return nil
	}}
	resp, err := c.Get(link)
	if err != nil {
		return 0, "", err
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
		return 0, "", errors.New(resp.Status)
	}
	if err := os.MkdirAll(filepath.Dir(dst), 0o755); err != nil {
		return 0, "", err
	}
	f, err := os.Create(dst)
	if err != nil {
		return 0, "", err
	}
	h := sha256.New()
	n, err := io.Copy(io.MultiWriter(f, h), resp.Body)
	if cerr := f.Close(); err == nil {
		err = cerr
	}
	return n, hex.EncodeToString(h.Sum(nil)), err
}

func fail(err error) {
	fmt.Fprintln(os.Stderr, "bundlemame:", err)
	os.Exit(1)
}
