// bundlemame puts the MAME team's official Windows release of MAME, as it
// is, into the RetroPixels download: it downloads the release from MAME's
// GitHub, checks it against the SHA-256 GitHub lists for it, unpacks it
// (without running it) into <dir>/MAME 0.xxx and marks it as built in. It
// also saves MAME's source code for that release, for the GitHub release.
//
//	go run ./tools/bundlemame -dir dist/RetroPixels-2.17.0/MAME -source dist/MAME-0.281-source.tar.gz
//
// -version picks a release ("0.281"); the default is the newest. GITHUB_TOKEN,
// when set, is sent to api.github.com only (CI runners share GitHub's limits).
package main

import (
	"errors"
	"flag"
	"fmt"
	"io"
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

func main() {
	dir := flag.String("dir", "", "the MAME folder inside the package")
	version := flag.String("version", "", "the MAME version (default: the newest)")
	source := flag.String("source", "", "where to save MAME's source code (.tar.gz)")
	ghOut := flag.String("github-output", os.Getenv("GITHUB_OUTPUT"), "file to write version=, tag= and source= to")
	flag.Parse()
	if *dir == "" {
		fail(errors.New("-dir is needed"))
	}

	in := mame.NewInstaller()
	in.Client = &http.Client{Timeout: 30 * time.Minute, Transport: tokenTransport{os.Getenv("GITHUB_TOKEN")}}
	in.Dirs = []string{*dir}
	last := time.Now()
	in.Progress = func(p mame.Progress) {
		if time.Since(last) > 5*time.Second {
			last = time.Now()
			fmt.Printf("  %s: %d of %d MB\n", p.State, p.Got>>20, p.Total>>20)
		}
	}
	rels, err := in.Releases()
	if err != nil {
		fail(err)
	}
	rel := rels[0] // newest first
	if *version != "" {
		rel = mame.Release{}
		for _, r := range rels {
			if r.Version == strings.TrimPrefix(*version, "v") {
				rel = r
			}
		}
		if rel.Version == "" {
			fail(fmt.Errorf("MAME %s isn't among the official Windows downloads", *version))
		}
	}
	if rel.SHA256 == "" {
		fail(fmt.Errorf("GitHub lists no checksum for %s, so it can't be checked", rel.URL))
	}
	fmt.Printf("MAME %s: %s (%d MB, sha256 %s)\n", rel.Version, rel.URL, rel.Size>>20, rel.SHA256)
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
	roms := filepath.Join(*dir, "roms")
	os.MkdirAll(roms, 0o755)
	readme := "Put your MAME ROM sets here (one .zip per game, named as MAME names it,\r\n" +
		"for example pacman.zip), then open Arcade (MAME) in RetroPixels.\r\n" +
		"No games come with RetroPixels.\r\n"
	if err := os.WriteFile(filepath.Join(roms, "PUT-ROM-SETS-HERE.txt"), []byte(readme), 0o644); err != nil {
		fail(err)
	}
	fmt.Println("unpacked to", folder)

	if *source != "" {
		link := "https://github.com/mamedev/mame/archive/refs/tags/" + url.PathEscape(rel.Tag) + ".tar.gz"
		n, err := download(link, *source)
		if err != nil {
			fail(fmt.Errorf("MAME's source code: %v", err))
		}
		fmt.Printf("source code: %s (%d MB)\n", *source, n>>20)
	}
	if *ghOut != "" {
		f, err := os.OpenFile(*ghOut, os.O_APPEND|os.O_CREATE|os.O_WRONLY, 0o644)
		if err != nil {
			fail(err)
		}
		fmt.Fprintf(f, "version=%s\ntag=%s\nsource=%s\nsha256=%s\n", rel.Version, rel.Tag, *source, rel.SHA256)
		f.Close()
	}
}

// download saves a GitHub source archive (github.com, then GitHub's codeload).
func download(link, dst string) (int64, error) {
	c := &http.Client{Timeout: 30 * time.Minute, CheckRedirect: func(r *http.Request, via []*http.Request) error {
		if r.URL.Scheme != "https" || (r.URL.Host != "github.com" && r.URL.Host != "codeload.github.com") || len(via) > 5 {
			return errors.New("redirected somewhere unexpected: " + r.URL.String())
		}
		return nil
	}}
	resp, err := c.Get(link)
	if err != nil {
		return 0, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
		return 0, errors.New(resp.Status)
	}
	if err := os.MkdirAll(filepath.Dir(dst), 0o755); err != nil {
		return 0, err
	}
	f, err := os.Create(dst)
	if err != nil {
		return 0, err
	}
	n, err := io.Copy(f, resp.Body)
	if cerr := f.Close(); err == nil {
		err = cerr
	}
	return n, err
}

func fail(err error) {
	fmt.Fprintln(os.Stderr, "bundlemame:", err)
	os.Exit(1)
}
