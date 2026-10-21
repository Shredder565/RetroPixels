//go:build linux

package main

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

func TestLinuxFFmpegNamesAndEncoders(t *testing.T) {
	if strings.Join(encoderOrder, " ") != "h264_nvenc h264_vaapi h264_amf h264_qsv libx264" {
		t.Fatalf("order: %v", encoderOrder)
	}
	j := finalJob{Video: []vidSeg{{Path: "a.mkv", Dur: 10, W: 1280, H: 720}}, DSP: []string{"x.wav"}, DSPLen: 10,
		From: 0, To: 10, FPS: 60, Out: "o.mp4"}
	args := strings.Join(finalArgs(j, "h264_vaapi", "small"), " ")
	for _, want := range []string{"setpts=PTS-STARTPTS,format=nv12,hwupload[v]", "-map [v] -map [a] -vaapi_device /dev/dri/renderD",
		"-c:v h264_vaapi -rc_mode CQP -qp 27 -profile:v high", "-c:a aac"} {
		if !strings.Contains(args, want) {
			t.Errorf("missing %q in\n%s", want, args)
		}
	}
	// Found in ~/.local/share/RetroPixels/ffmpeg when that's where it was downloaded.
	data := t.TempDir()
	t.Setenv("XDG_DATA_HOME", data)
	t.Setenv("PATH", t.TempDir())
	t.Setenv("RETRO_FFMPEG", "")
	if findFFmpeg() != "" {
		t.Skip("an ffmpeg next to the test binary")
	}
	dirs := ffmpegInstallDirs()
	if dirs[len(dirs)-1] != filepath.Join(data, "RetroPixels", "ffmpeg") {
		t.Fatalf("install dirs %v", dirs)
	}
	p := filepath.Join(data, "RetroPixels", "ffmpeg", "ffmpeg")
	os.MkdirAll(filepath.Dir(p), 0o755)
	os.WriteFile(p, []byte("#!/bin/sh\n"), 0o755)
	if findFFmpeg() != p {
		t.Fatalf("found %q", findFFmpeg())
	}
}

// The encode runs under nice, with the same arguments.
func TestLinuxLowPriority(t *testing.T) {
	if _, err := exec.LookPath("nice"); err != nil {
		t.Skip("no nice")
	}
	cmd := lowPriority(hiddenCommand("sh", "-c", "echo $0", "x y"))
	if filepath.Base(cmd.Path) != "nice" {
		t.Fatalf("not under nice: %v", cmd.Args)
	}
	if out, err := cmd.Output(); err != nil || strings.TrimSpace(string(out)) != "x y" {
		t.Fatalf("%v %q", err, out)
	}
}
