package main

// One-click ffmpeg download (the Windows or Linux build from
// github.com/BtbN/FFmpeg-Builds, see internal/ffmpeg), checked against the
// release's published SHA-256 before it's used.

import (
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"runtime"
	"sync"
	"time"

	"retropixels/internal/ffmpeg"
)

var (
	ffMu       sync.Mutex
	ffState    = "idle" // idle | downloading | verifying | installing | done | error
	ffGot      int64
	ffTotal    int64
	ffErr      string
	ffInstallF = installFFmpeg
)

// ffmpegInstallDirs: next to the app if possible, otherwise in the user's
// app data (%LOCALAPPDATA%\RetroPixels\ffmpeg, ~/.local/share/RetroPixels/ffmpeg).
func ffmpegInstallDirs() []string { return ffmpeg.InstallDirs() }

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
	asset := ffmpeg.Asset(runtime.GOOS, runtime.GOARCH)
	if asset == "" {
		return ffmpeg.ErrNoDownload
	}
	client := &http.Client{Timeout: 30 * time.Minute}
	// 1. Expected checksum.
	resp, err := client.Get(ffmpeg.Release + "checksums.sha256")
	if err != nil {
		return fmt.Errorf("couldn't reach GitHub: %v", err)
	}
	want := ffmpeg.Checksum(resp.Body, asset)
	resp.Body.Close()
	if want == "" {
		return errors.New("couldn't find the ffmpeg checksum on GitHub")
	}

	// 2. Download to a temp file while hashing.
	tmpDir := ffmpeg.WritableDir(ffmpegInstallDirs())
	if tmpDir == "" {
		return errors.New("no folder RetroPixels can write ffmpeg into")
	}
	archive := filepath.Join(tmpDir, "ffmpeg-download"+ffmpeg.ArchiveExt(asset)+".tmp")
	defer os.Remove(archive)
	resp, err = client.Get(ffmpeg.Release + asset)
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
	out, err := os.Create(archive)
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

	// 3. Take ffmpeg (ffmpeg.exe) and the licence out of the download.
	setFF("installing", nil)
	_, err = ffmpeg.Extract(archive, asset, tmpDir)
	return err
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
