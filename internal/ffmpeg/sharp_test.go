package ffmpeg

import "testing"

func TestSharpScale(t *testing.T) {
	for h, want := range map[int]int{384: 3, 480: 3, 240: 5, 720: 2, 1080: 1, 1440: 1, 0: 1} {
		if got := SharpScale(h); got != want {
			t.Errorf("SharpScale(%d) = %d, want %d", h, got, want)
		}
	}
	if f := SharpFilter(400, 480); f != "scale=1200:1440:flags=neighbor," {
		t.Errorf("SharpFilter = %q", f)
	}
}
