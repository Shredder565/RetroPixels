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
	"runtime"
	"sort"
	"strings"
	"sync"
	"time"
)

// ExeName is ffmpeg's file name on this system.
var ExeName = exeName(runtime.GOOS)

func exeName(goos string) string {
	if goos == "windows" {
		return "ffmpeg.exe"
	}
	return "ffmpeg"
}

// Dirs: where ffmpeg(.exe) may be. Next to the program first, then the
// folder RetroPixels downloads it to, so both apps share one copy.
func Dirs() []string {
	var dirs []string
	if exe, err := os.Executable(); err == nil {
		d := filepath.Dir(exe)
		dirs = append(dirs, filepath.Join(d, "ffmpeg"), d, filepath.Join(d, "ffmpeg", "bin"))
	}
	if d := DataDir(); d != "" {
		dirs = append(dirs, d)
	}
	return dirs
}

// InstallDirs: where a downloaded ffmpeg goes, best first: the ffmpeg
// folder next to the program, else the user's own data folder.
func InstallDirs() []string {
	var dirs []string
	if exe, err := os.Executable(); err == nil {
		dirs = append(dirs, filepath.Join(filepath.Dir(exe), "ffmpeg"))
	}
	if d := DataDir(); d != "" {
		dirs = append(dirs, d)
	}
	return dirs
}

// DataDir is the per-user folder ffmpeg is downloaded to when the program's
// own folder can't be written: %LOCALAPPDATA%\RetroPixels\ffmpeg on
// Windows, $XDG_DATA_HOME/RetroPixels/ffmpeg (~/.local/share/RetroPixels/ffmpeg)
// elsewhere. "" when there is none.
func DataDir() string {
	if runtime.GOOS == "windows" {
		if la := os.Getenv("LOCALAPPDATA"); la != "" {
			return filepath.Join(la, "RetroPixels", "ffmpeg")
		}
		return ""
	}
	if d := os.Getenv("XDG_DATA_HOME"); filepath.IsAbs(d) {
		return filepath.Join(d, "RetroPixels", "ffmpeg")
	}
	if home, err := os.UserHomeDir(); err == nil && home != "" {
		return filepath.Join(home, ".local", "share", "RetroPixels", "ffmpeg")
	}
	return ""
}

// Find returns ffmpeg's path, or "".
func Find() string {
	for _, d := range Dirs() {
		p := filepath.Join(d, ExeName)
		if isProgram(p) {
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

// isProgram: a file (not a folder) that can be run (on Windows, any file).
func isProgram(p string) bool {
	st, err := os.Stat(p)
	if err != nil || st.IsDir() {
		return false
	}
	return runtime.GOOS == "windows" || st.Mode()&0o111 != 0
}

// Encoders, best first: graphics-card encoders, then the CPU. These are the
// encoders every caller's command lines handle; Order adds VAAPI on Linux
// for callers that take their picture format from PixFmt.
var Encoders = []string{"h264_nvenc", "h264_amf", "h264_qsv", "libx264"}

// Order: the encoders to try, best first, on system goos, for a caller
// that uses PixFmt and TestArgs. On Linux, VAAPI (Intel and AMD graphics
// through Mesa, the usual way there) comes right after NVIDIA's.
func Order(goos string) []string {
	if goos == "linux" {
		return []string{"h264_nvenc", "h264_vaapi", "h264_amf", "h264_qsv", "libx264"}
	}
	return Encoders
}

// VAAPIDevice is the graphics card VAAPI encodes on: the first render node.
func VAAPIDevice() string {
	if m, _ := filepath.Glob("/dev/dri/renderD*"); len(m) > 0 {
		sort.Strings(m)
		return m[0]
	}
	return "/dev/dri/renderD128"
}

// PixFmt: the end of the picture's filter chain for an encoder, after
// "format=": the pixel format it takes, and for VAAPI the upload to the
// graphics card.
func PixFmt(enc string) string {
	switch enc {
	case "h264_qsv":
		return "nv12"
	case "h264_vaapi":
		return "nv12,hwupload"
	}
	return "yuv420p"
}

// TestArgs: ffmpeg's options for test-encoding a few black frames with enc,
// to see whether it works on this PC.
func TestArgs(enc string) []string {
	args := []string{"-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i", "color=black:s=640x360:r=30:d=0.3"}
	if enc == "h264_vaapi" {
		args = append(args, "-vf", "format="+PixFmt(enc))
		args = append(args, EncoderArgs(enc, "balanced")...)
	} else {
		args = append(args, EncoderArgs(enc, "balanced")...)
		args = append(args, "-pix_fmt", "yuv420p")
	}
	return append(args, "-f", "null", "-")
}

// EncoderArgs: ffmpeg's options for an encoder at "high", "balanced" or "small".
func EncoderArgs(enc, quality string) []string {
	q := map[string][3]string{
		"h264_nvenc": {"18", "21", "26"},
		"h264_amf":   {"18", "22", "27"},
		"h264_qsv":   {"18", "22", "27"},
		"h264_vaapi": {"18", "22", "27"},
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
	case "h264_vaapi":
		// -vaapi_device is a global option, so it may come after the inputs;
		// it also gives hwupload (see PixFmt) its device.
		return []string{"-vaapi_device", VAAPIDevice(), "-c:v", "h264_vaapi", "-rc_mode", "CQP", "-qp", v, "-profile:v", "high"}
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
		if RunTimeout(Hidden(ff, TestArgs(e)...), 15*time.Second) == nil {
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

// The builds from github.com/BtbN/FFmpeg-Builds, as RetroPixels uses: the
// Windows one (a .zip) and the Linux ones (static programs in a .tar.xz).
var Release = "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/"

// Asset: the download for a system and processor, "" when there is none.
func Asset(goos, goarch string) string {
	switch {
	case goos == "windows":
		return "ffmpeg-n8.1-latest-win64-gpl-8.1.zip"
	case goos == "linux" && goarch == "amd64":
		return "ffmpeg-n8.1-latest-linux64-gpl-8.1.tar.xz"
	case goos == "linux" && goarch == "arm64":
		return "ffmpeg-n8.1-latest-linuxarm64-gpl-8.1.tar.xz"
	}
	return ""
}

// ErrNoDownload: there is no ffmpeg download for this system.
var ErrNoDownload = errors.New("there's no ffmpeg download for this system: install ffmpeg with your system's package manager")

// Checksum reads the asset's SHA-256 from the release's checksums.sha256.
func Checksum(r io.Reader, asset string) string {
	sc := bufio.NewScanner(r)
	for sc.Scan() {
		f := strings.Fields(sc.Text())
		if len(f) == 2 && f[1] == asset && len(f[0]) == 64 {
			return strings.ToLower(f[0])
		}
	}
	return ""
}

// Install downloads ffmpeg, checks it against the release's SHA-256 and
// puts ffmpeg (ffmpeg.exe) in the first of dirs it can write to. progress
// gets the bytes so far and the total (-1 when unknown).
func Install(dirs []string, progress func(got, total int64)) (string, error) {
	asset := Asset(runtime.GOOS, runtime.GOARCH)
	if asset == "" {
		return "", ErrNoDownload
	}
	client := &http.Client{Timeout: 30 * time.Minute}
	resp, err := client.Get(Release + "checksums.sha256")
	if err != nil {
		return "", fmt.Errorf("couldn't reach GitHub: %v", err)
	}
	want := Checksum(resp.Body, asset)
	resp.Body.Close()
	if want == "" {
		return "", errors.New("couldn't find the ffmpeg checksum on GitHub")
	}

	dir := WritableDir(dirs)
	if dir == "" {
		return "", errors.New("no folder ffmpeg can be saved in")
	}
	archive := filepath.Join(dir, "ffmpeg-download"+ArchiveExt(asset)+".tmp")
	defer os.Remove(archive)
	resp, err = client.Get(Release + asset)
	if err != nil {
		return "", fmt.Errorf("download failed: %v", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
		return "", fmt.Errorf("download failed: %s", resp.Status)
	}
	out, err := os.Create(archive)
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
	return Extract(archive, asset, dir)
}

// WritableDir: the first of dirs that can be made and written to, or "".
func WritableDir(dirs []string) string {
	for _, d := range dirs {
		if os.MkdirAll(d, 0o755) == nil {
			if f, err := os.CreateTemp(d, "dl-*.tmp"); err == nil {
				f.Close()
				os.Remove(f.Name())
				return d
			}
		}
	}
	return ""
}

// ArchiveExt: ".zip" or ".tar.xz", from the asset's name.
func ArchiveExt(asset string) string {
	if strings.HasSuffix(asset, ".tar.xz") {
		return ".tar.xz"
	}
	return ".zip"
}

// Extract takes ffmpeg's program (bin/ffmpeg.exe or bin/ffmpeg) and the
// top-level LICENSE.txt out of a downloaded build (asset is its name on the
// release, which says what kind of file it is) into dir, and returns the
// program's path.
func Extract(archive, asset, dir string) (string, error) {
	if ArchiveExt(asset) == ".tar.xz" {
		return extractTarXz(archive, strings.TrimSuffix(asset, ".tar.xz"), dir)
	}
	zr, err := zip.OpenReader(archive)
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

// extractTarXz unpacks the Linux build with the system's tar (GNU tar or
// bsdtar, both read .xz; Go has no xz reader). top is the archive's one
// folder, the asset's name without .tar.xz.
func extractTarXz(archive, top, dir string) (string, error) {
	tmp, err := os.MkdirTemp(dir, "unpack-*.tmp")
	if err != nil {
		return "", err
	}
	defer os.RemoveAll(tmp)
	bin, lic := top+"/bin/ffmpeg", top+"/LICENSE.txt"
	cmd := exec.Command("tar", "-xJf", archive, "-C", tmp, bin, lic)
	if out, err := cmd.CombinedOutput(); err != nil {
		if _, lerr := exec.LookPath("tar"); lerr != nil {
			return "", errors.New("unpacking ffmpeg needs the tar program, which isn't installed")
		}
		msg := strings.TrimSpace(string(out))
		if len(msg) > 300 {
			msg = msg[len(msg)-300:]
		}
		return "", fmt.Errorf("couldn't unpack ffmpeg (is xz installed?): %v %s", err, msg)
	}
	exe := filepath.Join(dir, "ffmpeg")
	if err := placeFile(filepath.Join(tmp, filepath.FromSlash(bin)), exe, 0o755); err != nil {
		return "", errors.New("ffmpeg wasn't in the download")
	}
	_ = placeFile(filepath.Join(tmp, filepath.FromSlash(lic)), filepath.Join(dir, "LICENSE.txt"), 0o644)
	return exe, nil
}

// placeFile moves src to dst (same file system: both are inside dir) with mode.
func placeFile(src, dst string, mode os.FileMode) error {
	if err := os.Chmod(src, mode); err != nil {
		return err
	}
	return os.Rename(src, dst)
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
