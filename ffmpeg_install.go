package main

// One-click ffmpeg download (the Windows build from github.com/BtbN/FFmpeg-Builds),
// checked against the release's published SHA-256 before it's used.

import (
	"archive/zip"
	"bufio"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"
)

const (
	ffRelease = "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/"
	ffAsset   = "ffmpeg-n8.1-latest-win64-gpl-8.1.zip"
)

var (
	ffMu       sync.Mutex
	ffState    = "idle" // idle | downloading | verifying | installing | done | error
	ffGot      int64
	ffTotal    int64
	ffErr      string
	ffInstallF = installFFmpeg
)

// ffmpegInstallDirs: next to the app if possible, otherwise in the user's app data.
func ffmpegInstallDirs() []string {
	var dirs []string
	if exe, err := os.Executable(); err == nil {
		dirs = append(dirs, filepath.Join(filepath.Dir(exe), "ffmpeg"))
	}
	if la := os.Getenv("LOCALAPPDATA"); la != "" {
		dirs = append(dirs, filepath.Join(la, "RetroPixels", "ffmpeg"))
	}
	return dirs
}

func ffStatus() map[string]any {
	ffMu.Lock()
	defer ffMu.Unlock()
	return map[string]any{"state": ffState, "got": ffGot, "total": ffTotal, "error": ffErr}
}

func setFF(state string, err error) {
	ffMu.Lock()
	ffState = state
	if err != nil {
		ffErr = err.Error()
	} else if state != "error" {
		ffErr = ""
	}
	ffMu.Unlock()
}

type countingWriter struct{ w io.Writer }

func (c countingWriter) Write(p []byte) (int, error) {
	n, err := c.w.Write(p)
	ffMu.Lock()
	ffGot += int64(n)
	ffMu.Unlock()
	return n, err
}

func startFFmpegInstall() {
	ffMu.Lock()
	if ffState == "downloading" || ffState == "verifying" || ffState == "installing" {
		ffMu.Unlock()
		return
	}
	ffState, ffGot, ffTotal, ffErr = "downloading", 0, 0, ""
	ffMu.Unlock()
	go func() {
		if err := ffInstallF(); err != nil {
			setFF("error", err)
			return
		}
		encMu.Lock()
		encFound = nil // re-test encoders with the new ffmpeg
		encMu.Unlock()
		setFF("done", nil)
	}()
}

func installFFmpeg() error {
	client := &http.Client{Timeout: 30 * time.Minute}
	// 1. Expected checksum.
	resp, err := client.Get(ffRelease + "checksums.sha256")
	if err != nil {
		return fmt.Errorf("couldn't reach GitHub: %v", err)
	}
	want := ""
	sc := bufio.NewScanner(resp.Body)
	for sc.Scan() {
		f := strings.Fields(sc.Text())
		if len(f) == 2 && f[1] == ffAsset {
			want = strings.ToLower(f[0])
		}
	}
	resp.Body.Close()
	if len(want) != 64 {
		return errors.New("couldn't find the ffmpeg checksum on GitHub")
	}

	// 2. Download to a temp file while hashing.
	tmpDir := ""
	for _, d := range ffmpegInstallDirs() {
		if os.MkdirAll(d, 0o755) == nil {
			if f, err := os.CreateTemp(d, "dl-*.tmp"); err == nil {
				f.Close()
				os.Remove(f.Name())
				tmpDir = d
				break
			}
		}
	}
	if tmpDir == "" {
		return errors.New("no folder RetroPixels can write ffmpeg into")
	}
	zipPath := filepath.Join(tmpDir, "ffmpeg-download.zip.tmp")
	defer os.Remove(zipPath)
	resp, err = client.Get(ffRelease + ffAsset)
	if err != nil {
		return fmt.Errorf("download failed: %v", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
		return fmt.Errorf("download failed: %s", resp.Status)
	}
	ffMu.Lock()
	ffTotal = resp.ContentLength
	ffMu.Unlock()
	out, err := os.Create(zipPath)
	if err != nil {
		return err
	}
	h := sha256.New()
	if _, err := io.Copy(io.MultiWriter(countingWriter{out}, h), resp.Body); err != nil {
		out.Close()
		return fmt.Errorf("download interrupted: %v", err)
	}
	out.Close()

	setFF("verifying", nil)
	if got := hex.EncodeToString(h.Sum(nil)); got != want {
		return errors.New("the downloaded file didn't match its checksum, so it wasn't used — try again")
	}

	// 3. Pull ffmpeg.exe and the licence out of the zip.
	setFF("installing", nil)
	zr, err := zip.OpenReader(zipPath)
	if err != nil {
		return err
	}
	defer zr.Close()
	found := false
	for _, f := range zr.File {
		base := filepath.Base(f.Name)
		if (base == "ffmpeg.exe" && strings.Contains(f.Name, "/bin/")) || (base == "LICENSE.txt" && strings.Count(f.Name, "/") == 1) {
			rc, err := f.Open()
			if err != nil {
				return err
			}
			dst := filepath.Join(tmpDir, base)
			w, err := os.Create(dst + ".tmp")
			if err != nil {
				rc.Close()
				return err
			}
			_, err = io.Copy(w, rc)
			rc.Close()
			w.Close()
			if err != nil {
				return err
			}
			if err := os.Rename(dst+".tmp", dst); err != nil {
				return err
			}
			if base == "ffmpeg.exe" {
				found = true
			}
		}
	}
	if !found {
		return errors.New("ffmpeg.exe wasn't in the download")
	}
	return nil
}

func registerFFmpegRoutes(mux *http.ServeMux) {
	mux.HandleFunc("/api/rec/ffmpeg-install", guard(func(w http.ResponseWriter, r *http.Request) {
		if r.Method == http.MethodPost {
			if findFFmpeg() == "" {
				startFFmpegInstall()
			}
		}
		writeJSON(w, 200, map[string]any{"install": ffStatus(), "ffmpeg": findFFmpeg() != ""})
	}))
}
