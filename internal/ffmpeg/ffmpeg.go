// Package ffmpeg finds (or downloads) ffmpeg and picks a video encoder, for
// RetroPixels Native's recorder. It follows the RetroPixels app's recorder
// (recorder.go, ffmpeg_install.go at the repository root): the same places to
// look, the same encoders in the same order, the same download.
package ffmpeg

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
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"time"
)

// Dirs: where ffmpeg.exe may be. Next to the program first, then the folder
// RetroPixels.exe downloads it to, so both apps share one copy.
func Dirs() []string {
	var dirs []string
	if exe, err := os.Executable(); err == nil {
		d := filepath.Dir(exe)
		dirs = append(dirs, filepath.Join(d, "ffmpeg"), d, filepath.Join(d, "ffmpeg", "bin"))
	}
	if la := os.Getenv("LOCALAPPDATA"); la != "" {
		dirs = append(dirs, filepath.Join(la, "RetroPixels", "ffmpeg"))
	}
	return dirs
}

// Find returns ffmpeg's path, or "".
func Find() string {
	for _, d := range Dirs() {
		p := filepath.Join(d, "ffmpeg.exe")
		if st, err := os.Stat(p); err == nil && !st.IsDir() {
			return p
		}
	}
	if p := os.Getenv("RETRO_FFMPEG"); p != "" {
		return p
	}
	if p, err := exec.LookPath("ffmpeg"); err == nil {
		return p
	}
	return ""
}

// Hidden makes a command that opens no console window.
func Hidden(name string, args ...string) *exec.Cmd {
	cmd := exec.Command(name, args...)
	hide(cmd)
	return cmd
}

// Encoders, best first: graphics-card encoders, then the CPU.
var Encoders = []string{"h264_nvenc", "h264_amf", "h264_qsv", "libx264"}

// EncoderArgs: ffmpeg's options for an encoder at "high", "balanced" or "small".
func EncoderArgs(enc, quality string) []string {
	q := map[string][3]string{
		"h264_nvenc": {"18", "21", "26"},
		"h264_amf":   {"18", "22", "27"},
		"h264_qsv":   {"18", "22", "27"},
		"libx264":    {"18", "21", "26"},
	}[enc]
	v := q[1]
	if quality == "high" {
		v = q[0]
	} else if quality == "small" {
		v = q[2]
	}
	switch enc {
	case "h264_nvenc":
		return []string{"-c:v", "h264_nvenc", "-preset", "p5", "-tune", "hq", "-rc", "vbr", "-cq", v, "-b:v", "0", "-profile:v", "high"}
	case "h264_amf":
		return []string{"-c:v", "h264_amf", "-quality", "quality", "-rc", "cqp", "-qp_i", v, "-qp_p", v}
	case "h264_qsv":
		return []string{"-c:v", "h264_qsv", "-preset", "medium", "-global_quality", v}
	}
	return []string{"-c:v", "libx264", "-preset", "veryfast", "-crf", v}
}

var (
	encMu    sync.Mutex
	encFound = map[string]string{} // ffmpeg path -> encoder
)

// PickEncoder test-encodes a few frames with each encoder and returns the
// first that works on this PC (remembered per ffmpeg).
func PickEncoder(ff string) string {
	encMu.Lock()
	defer encMu.Unlock()
	if e, ok := encFound[ff]; ok {
		return e
	}
	enc := "libx264"
	for _, e := range Encoders {
		args := append([]string{"-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i", "color=black:s=640x360:r=30:d=0.3"},
			EncoderArgs(e, "balanced")...)
		args = append(args, "-pix_fmt", "yuv420p", "-f", "null", "-")
		if RunTimeout(Hidden(ff, args...), 15*time.Second) == nil {
			enc = e
			break
		}
	}
	encFound[ff] = enc
	return enc
}

// RunTimeout runs a command, killing it after d.
func RunTimeout(cmd *exec.Cmd, d time.Duration) error {
	if err := cmd.Start(); err != nil {
		return err
	}
	done := make(chan error, 1)
	go func() { done <- cmd.Wait() }()
	select {
	case err := <-done:
		return err
	case <-time.After(d):
		cmd.Process.Kill()
		return errors.New("timed out")
	}
}

// The Windows build from github.com/BtbN/FFmpeg-Builds, as RetroPixels.exe uses.
const (
	release = "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/"
	asset   = "ffmpeg-n8.1-latest-win64-gpl-8.1.zip"
)

// Install downloads ffmpeg, checks it against the release's SHA-256 and
// puts ffmpeg.exe in the first of dirs it can write to. progress gets the
// bytes so far and the total (-1 when unknown).
func Install(dirs []string, progress func(got, total int64)) (string, error) {
	client := &http.Client{Timeout: 30 * time.Minute}
	resp, err := client.Get(release + "checksums.sha256")
	if err != nil {
		return "", fmt.Errorf("couldn't reach GitHub: %v", err)
	}
	want := ""
	sc := bufio.NewScanner(resp.Body)
	for sc.Scan() {
		f := strings.Fields(sc.Text())
		if len(f) == 2 && f[1] == asset {
			want = strings.ToLower(f[0])
		}
	}
	resp.Body.Close()
	if len(want) != 64 {
		return "", errors.New("couldn't find the ffmpeg checksum on GitHub")
	}

	dir := ""
	for _, d := range dirs {
		if os.MkdirAll(d, 0o755) == nil {
			if f, err := os.CreateTemp(d, "dl-*.tmp"); err == nil {
				f.Close()
				os.Remove(f.Name())
				dir = d
				break
			}
		}
	}
	if dir == "" {
		return "", errors.New("no folder ffmpeg can be saved in")
	}
	zipPath := filepath.Join(dir, "ffmpeg-download.zip.tmp")
	defer os.Remove(zipPath)
	resp, err = client.Get(release + asset)
	if err != nil {
		return "", fmt.Errorf("download failed: %v", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
		return "", fmt.Errorf("download failed: %s", resp.Status)
	}
	out, err := os.Create(zipPath)
	if err != nil {
		return "", err
	}
	h := sha256.New()
	pw := &progressWriter{total: resp.ContentLength, f: progress}
	if _, err := io.Copy(io.MultiWriter(out, h, pw), resp.Body); err != nil {
		out.Close()
		return "", fmt.Errorf("download interrupted: %v", err)
	}
	out.Close()
	if got := hex.EncodeToString(h.Sum(nil)); got != want {
		return "", errors.New("the downloaded file didn't match its checksum, so it wasn't used — try again")
	}

	zr, err := zip.OpenReader(zipPath)
	if err != nil {
		return "", err
	}
	defer zr.Close()
	exe := ""
	for _, f := range zr.File {
		base := filepath.Base(f.Name)
		if (base == "ffmpeg.exe" && strings.Contains(f.Name, "/bin/")) || (base == "LICENSE.txt" && strings.Count(f.Name, "/") == 1) {
			dst := filepath.Join(dir, base)
			if err := extract(f, dst); err != nil {
				return "", err
			}
			if base == "ffmpeg.exe" {
				exe = dst
			}
		}
	}
	if exe == "" {
		return "", errors.New("ffmpeg.exe wasn't in the download")
	}
	return exe, nil
}

func extract(f *zip.File, dst string) error {
	rc, err := f.Open()
	if err != nil {
		return err
	}
	defer rc.Close()
	w, err := os.Create(dst + ".tmp")
	if err != nil {
		return err
	}
	_, err = io.Copy(w, rc)
	w.Close()
	if err != nil {
		os.Remove(dst + ".tmp")
		return err
	}
	return os.Rename(dst+".tmp", dst)
}

type progressWriter struct {
	got, total int64
	last       time.Time
	f          func(got, total int64)
}

func (p *progressWriter) Write(b []byte) (int, error) {
	p.got += int64(len(b))
	if p.f != nil && time.Since(p.last) > 200*time.Millisecond {
		p.last = time.Now()
		p.f(p.got, p.total)
	}
	return len(b), nil
}
