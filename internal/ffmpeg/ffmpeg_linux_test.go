//go:build linux

package ffmpeg

import (
	"crypto/sha256"
	"encoding/hex"
	"net/http"
	"net/http/httptest"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
)

func TestNamesAndAssets(t *testing.T) {
	if exeName("windows") != "ffmpeg.exe" || exeName("linux") != "ffmpeg" || ExeName != "ffmpeg" {
		t.Fatal("exe names")
	}
	if Asset("windows", "amd64") != "ffmpeg-n8.1-latest-win64-gpl-8.1.zip" || Asset("windows", "arm64") != Asset("windows", "amd64") {
		t.Fatal("windows asset changed")
	}
	if Asset("linux", "amd64") != "ffmpeg-n8.1-latest-linux64-gpl-8.1.tar.xz" || Asset("linux", "arm64") != "ffmpeg-n8.1-latest-linuxarm64-gpl-8.1.tar.xz" {
		t.Fatal("linux assets")
	}
	if Asset("darwin", "arm64") != "" || Asset("linux", "386") != "" {
		t.Fatal("asset for a system without a download")
	}
	if ArchiveExt(Asset("linux", "amd64")) != ".tar.xz" || ArchiveExt(Asset("windows", "amd64")) != ".zip" {
		t.Fatal("archive kinds")
	}
	sums := "68492f05dd6fa90a1d51f45ab02a0bb7b1ab4a8416a5068f3f95d596e947f197  ffmpeg-n8.1-latest-win64-gpl-8.1.zip\n" +
		"56DF3EA697554A4E50AE4AEC1737FA4E71FA201FD8B768F35B736D8C36ADA138  ffmpeg-n8.1-latest-linux64-gpl-8.1.tar.xz\n" +
		"abc  ffmpeg-n8.1-latest-linuxarm64-gpl-8.1.tar.xz\n"
	if Checksum(strings.NewReader(sums), Asset("linux", "amd64")) != "56df3ea697554a4e50ae4aec1737fa4e71fa201fd8b768f35b736d8c36ada138" {
		t.Fatal("checksum")
	}
	if Checksum(strings.NewReader(sums), Asset("linux", "arm64")) != "" {
		t.Fatal("short checksum taken")
	}
}

func TestDirs(t *testing.T) {
	data := t.TempDir()
	t.Setenv("XDG_DATA_HOME", data)
	want := filepath.Join(data, "RetroPixels", "ffmpeg")
	if DataDir() != want {
		t.Fatalf("data dir %q", DataDir())
	}
	t.Setenv("XDG_DATA_HOME", "relative/not/used")
	t.Setenv("HOME", "/home/someone")
	if DataDir() != "/home/someone/.local/share/RetroPixels/ffmpeg" {
		t.Fatalf("data dir without XDG_DATA_HOME: %q", DataDir())
	}
	t.Setenv("XDG_DATA_HOME", data)
	d := Dirs()
	if d[len(d)-1] != want {
		t.Fatalf("dirs %v", d)
	}
	in := InstallDirs()
	if len(in) != 2 || filepath.Base(in[0]) != "ffmpeg" || in[1] != want {
		t.Fatalf("install dirs %v", in)
	}

	// Found in the data folder only when it can be run.
	t.Setenv("PATH", t.TempDir())
	t.Setenv("RETRO_FFMPEG", "")
	if Find() != "" {
		t.Skip("an ffmpeg next to the test binary")
	}
	os.MkdirAll(want, 0o755)
	p := filepath.Join(want, "ffmpeg")
	os.WriteFile(p, []byte("#!/bin/sh\n"), 0o644)
	if Find() != "" {
		t.Fatal("found a file that can't be run")
	}
	os.Chmod(p, 0o755)
	if Find() != p {
		t.Fatalf("not found: %q", Find())
	}
}

func TestEncoderLists(t *testing.T) {
	if strings.Join(Order("windows"), " ") != "h264_nvenc h264_amf h264_qsv libx264" {
		t.Fatalf("windows order changed: %v", Order("windows"))
	}
	if strings.Join(Order("linux"), " ") != "h264_nvenc h264_vaapi h264_amf h264_qsv libx264" {
		t.Fatalf("linux order: %v", Order("linux"))
	}
	for _, e := range Encoders {
		if e == "h264_vaapi" {
			t.Fatal("VAAPI in Encoders: its callers don't upload frames")
		}
	}
	if PixFmt("libx264") != "yuv420p" || PixFmt("h264_qsv") != "nv12" || PixFmt("h264_vaapi") != "nv12,hwupload" {
		t.Fatal("pixel formats")
	}
	a := strings.Join(EncoderArgs("h264_vaapi", "high"), " ")
	if !strings.HasPrefix(a, "-vaapi_device /dev/dri/renderD") || !strings.Contains(a, "-c:v h264_vaapi -rc_mode CQP -qp 18") {
		t.Fatalf("vaapi args: %s", a)
	}
	if a := strings.Join(TestArgs("h264_vaapi"), " "); !strings.Contains(a, "-vf format=nv12,hwupload -vaapi_device") || strings.Contains(a, "-pix_fmt") {
		t.Fatalf("vaapi test args: %s", a)
	}
	if a := strings.Join(TestArgs("libx264"), " "); a != "-hide_banner -loglevel error -f lavfi -i color=black:s=640x360:r=30:d=0.3 -c:v libx264 -preset veryfast -crf 21 -pix_fmt yuv420p -f null -" {
		t.Fatalf("libx264 test args changed: %s", a)
	}
}

// With a real ffmpeg: VAAPI is only taken where it works (never in a
// container without a graphics card), the CPU encoder always.
func TestPickEncoderReal(t *testing.T) {
	ff := os.Getenv("RETRO_FFMPEG")
	if ff == "" {
		ff, _ = exec.LookPath("ffmpeg")
	}
	if ff == "" {
		t.Skip("no ffmpeg")
	}
	if RunTimeout(Hidden(ff, TestArgs("libx264")...), 15e9) != nil {
		t.Skip("this ffmpeg has no libx264")
	}
	if _, err := os.Stat("/dev/dri"); err != nil {
		if RunTimeout(Hidden(ff, TestArgs("h264_vaapi")...), 15e9) == nil {
			t.Fatal("VAAPI worked without a graphics card")
		}
	}
	if e := PickEncoder(ff); e == "" {
		t.Fatal("no encoder")
	}
}

func TestLowPriority(t *testing.T) {
	if _, err := exec.LookPath("nice"); err != nil {
		t.Skip("no nice")
	}
	out, err := exec.Command("nice").Output()
	if err != nil {
		t.Skip(err)
	}
	base := strings.TrimSpace(string(out))
	cmd := LowPriority(exec.Command("sh", "-c", `nice; echo "$0 $1"`, "a b", "c"))
	got, err := cmd.Output()
	if err != nil {
		t.Fatal(err)
	}
	lines := strings.Split(strings.TrimSpace(string(got)), "\n")
	if len(lines) != 2 || lines[1] != "a b c" {
		t.Fatalf("arguments changed: %q", got)
	}
	if base == "0" && lines[0] != "10" {
		t.Fatalf("niceness %s", lines[0])
	}
	// A program that isn't there still fails to start.
	if err := LowPriority(exec.Command("no-such-program-rp")).Run(); err == nil {
		t.Fatal("missing program started")
	}
}

// Install against a stand-in release: a .tar.xz laid out like BtbN's.
func TestInstallTarXz(t *testing.T) {
	asset := Asset(runtime.GOOS, runtime.GOARCH)
	if asset == "" {
		t.Skip("no download for this processor")
	}
	for _, p := range []string{"tar", "xz"} {
		if _, err := exec.LookPath(p); err != nil {
			t.Skip("no " + p)
		}
	}
	src := t.TempDir()
	top := strings.TrimSuffix(asset, ".tar.xz")
	os.MkdirAll(filepath.Join(src, top, "bin"), 0o755)
	os.MkdirAll(filepath.Join(src, top, "doc"), 0o755)
	os.WriteFile(filepath.Join(src, top, "bin", "ffmpeg"), []byte("#!/bin/sh\necho fake ffmpeg\n"), 0o755)
	os.WriteFile(filepath.Join(src, top, "bin", "ffprobe"), []byte("x"), 0o755)
	os.WriteFile(filepath.Join(src, top, "LICENSE.txt"), []byte("GPL"), 0o644)
	os.WriteFile(filepath.Join(src, top, "doc", "LICENSE.txt"), []byte("not this one"), 0o644)
	archive := filepath.Join(t.TempDir(), asset)
	if out, err := exec.Command("tar", "-cJf", archive, "-C", src, top).CombinedOutput(); err != nil {
		t.Fatalf("%v %s", err, out)
	}
	body, _ := os.ReadFile(archive)
	sum := sha256.Sum256(body)
	good := hex.EncodeToString(sum[:])
	sums := good
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/checksums.sha256":
			w.Write([]byte(sums + "  " + asset + "\n"))
		case "/" + asset:
			w.Write(body)
		default:
			http.NotFound(w, r)
		}
	}))
	defer srv.Close()
	old := Release
	Release = srv.URL + "/"
	defer func() { Release = old }()

	dir := filepath.Join(t.TempDir(), "ffmpeg")
	var last int64
	exe, err := Install([]string{"/proc/not-writable", dir}, func(got, total int64) { last = got })
	if err != nil {
		t.Fatal(err)
	}
	_ = last
	if exe != filepath.Join(dir, "ffmpeg") {
		t.Fatalf("exe %q", exe)
	}
	if out, err := exec.Command(exe).Output(); err != nil || strings.TrimSpace(string(out)) != "fake ffmpeg" {
		t.Fatalf("installed ffmpeg runs: %v %q", err, out)
	}
	if b, _ := os.ReadFile(filepath.Join(dir, "LICENSE.txt")); string(b) != "GPL" {
		t.Fatalf("licence %q", b)
	}
	ents, _ := os.ReadDir(dir)
	if len(ents) != 2 {
		var n []string
		for _, e := range ents {
			n = append(n, e.Name())
		}
		t.Fatalf("left in the folder: %v", n)
	}

	// A wrong checksum: nothing is installed.
	sums = strings.Repeat("0", 64)
	dir2 := filepath.Join(t.TempDir(), "ffmpeg")
	if _, err := Install([]string{dir2}, nil); err == nil || !strings.Contains(err.Error(), "checksum") {
		t.Fatalf("bad checksum: %v", err)
	}
	if ents, _ := os.ReadDir(dir2); len(ents) != 0 {
		t.Fatalf("left after a bad download: %d files", len(ents))
	}
}
