package main

import (
	"math"

	"retropixels/internal/games"
)

type rect struct{ X, Y, W, H int }

// pictureAspect: the core's own value, else the web player's for the system,
// else the picture's width / height.
func pictureAspect(coreAspect float64, sys string, w, h int) float64 {
	syncSystems()
	return games.PictureAspect(coreAspect, sys, w, h)
}

// fitRect: the biggest picture of that aspect that fits cw x ch, centred
// (draw() in host.js).
func fitRect(cw, ch int, aspect float64) rect {
	if cw <= 0 || ch <= 0 || aspect <= 0 {
		return rect{}
	}
	dw, dh := cw, int(math.Round(float64(cw)/aspect))
	if dh > ch {
		dh = ch
		dw = int(math.Round(float64(ch) * aspect))
	}
	return rect{(cw - dw) / 2, (ch - dh) / 2, dw, dh}
}

// baseSize: the picture at 1x (its own height, width from the aspect).
func baseSize(h int, aspect float64) (int, int) {
	if h <= 0 {
		h = 240
	}
	return int(math.Round(float64(h) * aspect)), h
}

// recordSize: the video size for recordings (recordingCanvas in host.js):
// about 720 lines, a whole multiple of the picture's height, even sizes.
func recordSize(h int, aspect float64) (int, int) {
	if h <= 0 {
		h = 240
	}
	scale := int(math.Max(1, math.Round(720/float64(h))))
	rh := (h * scale) &^ 1
	rw := int(math.Round(float64(rh)*aspect)) &^ 1
	return rw, rh
}

// scaleTo resizes a picture (nearest neighbour) so a recording keeps one size
// when the game changes resolution.
func scaleTo(dst []uint32, dw, dh int, src []uint32, sw, sh int) {
	games.ScaleTo(dst, dw, dh, src, sw, sh)
}
