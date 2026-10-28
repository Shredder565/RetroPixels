package rpcs3

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestSetRecording(t *testing.T) {
	dir := t.TempDir()
	os.WriteFile(ConfigFile(dir), []byte("Audio:\n  Renderer: XAudio2\n  Dump to file: false\n"), 0o644)
	if err := SetRecording(dir, true); err != nil {
		t.Fatal(err)
	}
	b, _ := os.ReadFile(ConfigFile(dir))
	if !strings.Contains(string(b), "  Dump to file: true\n") || !strings.Contains(string(b), "Renderer: XAudio2") {
		t.Fatalf("config %q", b)
	}
	rc, _ := os.ReadFile(filepath.Join(dir, "config", "recording.yml"))
	if !strings.Contains(string(rc), "Framerate: 60\n") || !strings.Contains(string(rc), "Video Bitrate: 25000000\n") {
		t.Fatalf("recording.yml %q", rc)
	}
	SetRecording(dir, false)
	if b, _ := os.ReadFile(ConfigFile(dir)); !strings.Contains(string(b), "  Dump to file: false\n") {
		t.Fatalf("off: %q", b)
	}
}

func TestFindRecordingAndRepair(t *testing.T) {
	dir := t.TempDir()
	start := time.Now()
	vid := filepath.Join(dir, "recordings", "NPEB00149", "NPEB00149_recording_x.mp4")
	os.MkdirAll(filepath.Dir(vid), 0o755)
	os.WriteFile(vid, []byte("mp4"), 0o644)
	wav := filepath.Join(dir, "cache", "audio_NPEB00149_x.wav")
	os.MkdirAll(filepath.Dir(wav), 0o755)
	os.WriteFile(wav, make([]byte, wavHeaderSize+8*480), 0o644) // RPCS3 closed before writing the header
	os.WriteFile(filepath.Join(dir, "cache", "other.bin"), []byte("x"), 0o644)
	rec := FindRecording(dir, start)
	if len(rec.Videos) != 1 || len(rec.Sounds) != 1 {
		t.Fatalf("found %+v", rec)
	}
	if err := RepairWav(wav); err != nil {
		t.Fatal(err)
	}
	b, _ := os.ReadFile(wav)
	if string(b[:4]) != "RIFF" || string(b[48:52]) != "data" || int(b[52])|int(b[53])<<8 != 8*480 {
		t.Fatalf("header % x", b[:56])
	}
	rec.Remove()
	if _, err := os.Stat(vid); err == nil {
		t.Fatal("video not removed")
	}
}
