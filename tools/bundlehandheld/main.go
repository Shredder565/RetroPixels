// bundlehandheld puts the 3DS / DS tab's two emulators into the RetroPixels
// download: Azahar (3DS) and melonDS (DS), the projects' own official Windows
// releases, as they are. Each is downloaded from its GitHub release, checked
// against the SHA-256 below, and unpacked (without running it) into
// <dir>/Azahar and <dir>/melonDS. Their source code for the same release is
// saved too, for the GitHub release (both are GPL).
//
//	go run ./tools/bundlehandheld -dir dist/RetroPixels-2.28.0 -source dist
//
// The versions are pinned: to move to a newer release, change the tag, file
// and sha256 here (GitHub lists each file's SHA-256 on the release page).
package main

import (
	"archive/zip"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"flag"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"time"
)

// pkg is one emulator's Windows release.
type pkg struct {
	Name, Dir   string   // "Azahar", its folder in the download
	Version     string   // the release tag
	URL, SHA256 string   // the Windows zip
	Exes        []string // the program, to find the folder that holds it
	Source      string   // the release's source code
	SourceFile  string   // saved as
	Licence     string
	Home        string
}

var pkgs = []pkg{
	{
		Name: "Azahar", Dir: "Azahar", Version: "2126.2",
		URL:        "https://github.com/azahar-emu/azahar/releases/download/2126.2/azahar-windows-msys2-2126.2.zip",
		SHA256:     "aa6d4c631c883355e50f5f62fd26a0e053d63e4cc29b8ded6b3b7d175ec3864e",
		Exes:       []string{"azahar.exe", "azahar-qt.exe"},
		Source:     "https://github.com/azahar-emu/azahar/releases/download/2126.2/azahar-unified-source-2126.2.tar.xz",
		SourceFile: "Azahar-2126.2-source.tar.xz",
		Licence:    "GPL-2.0-or-later", Home: "https://github.com/azahar-emu/azahar",
	},
	{
		Name: "melonDS", Dir: "melonDS", Version: "1.1",
		URL:        "https://github.com/melonDS-emu/melonDS/releases/download/1.1/melonDS-1.1-windows-x86_64.zip",
		SHA256:     "9f3f8a244103be20b5b657af5b0ed1b2a66bb20a7181476a6d294c9a53d4f8c8",
		Exes:       []string{"melonDS.exe"},
		Source:     "https://github.com/melonDS-emu/melonDS/archive/refs/tags/1.1.tar.gz",
		SourceFile: "melonDS-1.1-source.tar.gz",
		Licence:    "GPL-3.0-or-later", Home: "https://github.com/melonDS-emu/melonDS",
	},
}

func main() {
	dir := flag.String("dir", "", "the RetroPixels folder in the package (Azahar and melonDS go inside)")
	source := flag.String("source", "", "folder to save the emulators' source code in (optional)")
	ghOut := flag.String("github-output", os.Getenv("GITHUB_OUTPUT"), "file to write azahar= and melonds= (the versions) to")
	flag.Parse()
	if *dir == "" {
		fail(errors.New("-dir is needed"))
	}
	tmp, err := os.MkdirTemp("", "bundlehandheld-")
	if err != nil {
		fail(err)
	}
	defer os.RemoveAll(tmp)
	var out []string
	for _, p := range pkgs {
		zipPath := filepath.Join(tmp, filepath.Base(p.URL))
		n, sum, err := download(p.URL, zipPath)
		if err != nil {
			fail(fmt.Errorf("%s: %w", p.Name, err))
		}
		if sum != p.SHA256 {
			fail(fmt.Errorf("%s: %s has sha256 %s, not %s", p.Name, filepath.Base(p.URL), sum, p.SHA256))
		}
		fmt.Printf("%s %s: %s (%d bytes, sha256 %s)\n", p.Name, p.Version, filepath.Base(p.URL), n, sum)
		dst := filepath.Join(*dir, p.Dir)
		if err := place(zipPath, filepath.Join(tmp, p.Dir), dst, p.Exes); err != nil {
			fail(fmt.Errorf("%s: %w", p.Name, err))
		}
		note := fmt.Sprintf("%s %s, the official Windows release, unchanged.\r\n"+
			"From %s\r\n(%s, sha256 %s)\r\n\r\n"+
			"RetroPixels' 3DS / DS tab starts it: %s.\r\n"+
			"Licence: %s. Its source code for this release is attached to the RetroPixels release (%s)\r\nand at %s/tree/%s\r\n",
			p.Name, p.Version, p.URL, filepath.Base(p.URL), sum,
			map[string]string{"Azahar": "3DS games", "melonDS": "DS and DSi games"}[p.Name],
			p.Licence, p.SourceFile, p.Home, p.Version)
		if err := os.WriteFile(filepath.Join(dst, "RETROPIXELS-README.txt"), []byte(note), 0o644); err != nil {
			fail(err)
		}
		if *source != "" {
			n, sum, err := download(p.Source, filepath.Join(*source, p.SourceFile))
			if err != nil {
				fail(fmt.Errorf("%s source: %w", p.Name, err))
			}
			fmt.Printf("%s source: %s (%d bytes, sha256 %s)\n", p.Name, p.SourceFile, n, sum)
		}
		out = append(out, strings.ToLower(p.Name)+"="+p.Version)
	}
	if *ghOut != "" {
		f, err := os.OpenFile(*ghOut, os.O_APPEND|os.O_CREATE|os.O_WRONLY, 0o644)
		if err == nil {
			fmt.Fprintln(f, strings.Join(out, "\n"))
			f.Close()
		}
	}
}

// place unpacks the zip into tmp, finds the folder holding the program and
// moves it to dst.
func place(zipPath, tmp, dst string, exes []string) error {
	if err := unzip(zipPath, tmp); err != nil {
		return err
	}
	root, err := findExeDir(tmp, exes)
	if err != nil {
		return err
	}
	os.RemoveAll(dst)
	if err := os.MkdirAll(filepath.Dir(dst), 0o755); err != nil {
		return err
	}
	if err := os.Rename(root, dst); err != nil {
		return err
	}
	fmt.Printf("  unpacked into %s\n", dst)
	return nil
}

// findExeDir: the shallowest folder under root that holds one of exes.
func findExeDir(root string, exes []string) (string, error) {
	best, bestDepth := "", 1<<30
	err := filepath.WalkDir(root, func(p string, d os.DirEntry, err error) error {
		if err != nil || d.IsDir() {
			return err
		}
		for _, e := range exes {
			if strings.EqualFold(d.Name(), e) {
				if depth := strings.Count(p, string(filepath.Separator)); depth < bestDepth {
					best, bestDepth = filepath.Dir(p), depth
				}
			}
		}
		return nil
	})
	if err != nil {
		return "", err
	}
	if best == "" {
		return "", fmt.Errorf("none of %s is in the download", strings.Join(exes, ", "))
	}
	return best, nil
}

func unzip(zipPath, dst string) error {
	zr, err := zip.OpenReader(zipPath)
	if err != nil {
		return err
	}
	defer zr.Close()
	for _, f := range zr.File {
		name := filepath.FromSlash(f.Name)
		if !filepath.IsLocal(name) {
			return fmt.Errorf("unsafe path in the zip: %q", f.Name)
		}
		p := filepath.Join(dst, name)
		if f.FileInfo().IsDir() {
			if err := os.MkdirAll(p, 0o755); err != nil {
				return err
			}
			continue
		}
		if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
			return err
		}
		rc, err := f.Open()
		if err != nil {
			return err
		}
		w, err := os.OpenFile(p, os.O_CREATE|os.O_TRUNC|os.O_WRONLY, 0o644)
		if err != nil {
			rc.Close()
			return err
		}
		_, err = io.Copy(w, rc)
		rc.Close()
		if cerr := w.Close(); err == nil {
			err = cerr
		}
		if err != nil {
			return err
		}
		os.Chtimes(p, f.Modified, f.Modified)
	}
	return nil
}

// download saves link to dst and returns its size and SHA-256 (retrying a
// few times on network errors).
func download(link, dst string) (int64, string, error) {
	if err := os.MkdirAll(filepath.Dir(dst), 0o755); err != nil {
		return 0, "", err
	}
	var last error
	for try := 0; try < 4; try++ {
		if try > 0 {
			time.Sleep(time.Duration(2<<try) * time.Second)
		}
		n, sum, err := downloadOnce(link, dst)
		if err == nil {
			return n, sum, nil
		}
		last = err
		fmt.Fprintf(os.Stderr, "download %s: %v\n", link, err)
	}
	return 0, "", last
}

func downloadOnce(link, dst string) (int64, string, error) {
	c := &http.Client{Timeout: 20 * time.Minute}
	resp, err := c.Get(link)
	if err != nil {
		return 0, "", err
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
		return 0, "", fmt.Errorf("HTTP %s", resp.Status)
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
	if err != nil {
		return 0, "", err
	}
	return n, hex.EncodeToString(h.Sum(nil)), nil
}

func fail(err error) {
	fmt.Fprintln(os.Stderr, "bundlehandheld:", err)
	os.Exit(1)
}
