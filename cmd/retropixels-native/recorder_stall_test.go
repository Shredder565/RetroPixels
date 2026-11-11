package main

import (
	"os"
	"path/filepath"
	"testing"
	"time"
)

// ffmpeg stuck (nothing takes the pictures): the game keeps running, and the
// pictures left out take their sound with them.
func TestRecorderNeverWaitsForFFmpeg(t *testing.T) {
	wav, err := os.Create(filepath.Join(t.TempDir(), "a.wav"))
	if err != nil {
		t.Fatal(err)
	}
	defer wav.Close()
	r := &recorder{w: 256, h: 240, rate: sampleRate, wav: wav,
		frames: make(chan []byte, 180), free: make(chan []byte, 8), log: &tailBuf{}}
	pix := make([]uint32, 256*240)
	sound := make([]float32, 800)
	done := make(chan struct{})
	go func() {
		for i := 0; i < 600; i++ { // ten seconds of game
			r.frame(pix, 256, 240)
			r.audio(sound, false)
		}
		close(done)
	}()
	select {
	case <-done:
	case <-time.After(5 * time.Second):
		t.Fatal("the game waited for ffmpeg")
	}
	if r.nframes != 180 || r.dropped != 420 {
		t.Fatalf("recorded %d, left out %d", r.nframes, r.dropped)
	}
	if r.samples != 180*800 {
		t.Fatalf("sound for %d frames, want 180", r.samples/800)
	}
}
