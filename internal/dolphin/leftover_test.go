package dolphin

import (
	"os"
	"path/filepath"
	"testing"
	"time"
)

func TestFindLeftovers(t *testing.T) {
	dir := t.TempDir()
	old := time.Now().Add(-time.Hour)
	put := func(sub, name string, mod time.Time) string {
		p := filepath.Join(dir, sub, name)
		os.MkdirAll(filepath.Dir(p), 0o755)
		os.WriteFile(p, []byte("x"), 0o644)
		os.Chtimes(p, mod, mod)
		return p
	}
	// An hour of Luigi's Mansion in two pieces (a state was loaded), with its sound.
	v0 := put("Frames", "GLME01_2026-11-01_17-47-15_0.avi", old.Add(-30*time.Minute))
	v1 := put("Frames", "GLME01_2026-11-01_17-47-15_1.avi", old)
	dsp := put("Audio", "GLME01_2026-11-01_17-47-13_dspdump.wav", old)
	dtk := put("Audio", "GLME01_2026-11-01_17-47-13_dtkdump.wav", old)
	// Sound alone from another game: nothing to make, so it's cleared away.
	lone := put("Audio", "GALE01_2026-10-30_10-00-00_dspdump.wav", old)
	// A game still being recorded.
	put("Frames", "GZLE01_2026-11-02_09-00-00_0.avi", time.Now())
	put("Audio", "GZLE01_2026-11-02_08-59-59_dspdump.wav", time.Now())

	got := FindLeftovers(dir, 2*time.Minute)
	if len(got) != 1 {
		t.Fatalf("found %d leftovers, want 1: %+v", len(got), got)
	}
	l := got[0]
	if l.GameID != "GLME01" || len(l.Files.Video) != 2 || l.Files.Video[0] != v0 || l.Files.Video[1] != v1 ||
		len(l.Files.DSP) != 1 || l.Files.DSP[0] != dsp || len(l.Files.DTK) != 1 || l.Files.DTK[0] != dtk {
		t.Fatalf("wrong files: %+v", l)
	}
	if want := time.Date(2026, 11, 1, 17, 47, 13, 0, time.Local); !l.Started.Equal(want) {
		t.Errorf("started %v, want %v", l.Started, want)
	}
	if _, err := os.Stat(lone); err == nil {
		t.Error("left the sound-only files in place")
	}

	r := LeftoverRecording(l, "Luigi's Mansion", "")
	if r.files == nil || len(r.clips) != 1 || r.clips[0].Open || !r.clips[0].From.Edge || !r.clips[0].To.Edge {
		t.Fatalf("the recording should be the whole game, finished: %+v", r.clips)
	}
}
