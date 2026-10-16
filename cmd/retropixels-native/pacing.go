package main

import (
	"math"

	"retropixels/internal/games"
)

// pacer decides how many frames to run, like Player.loop in host.js: frames
// follow the clock, and run a little early or late to keep the sound queue
// around 50-120 ms (the sound card's clock is the one that matters).
type pacer struct {
	frameTime float64 // seconds per frame
	acc       float64
}

const (
	pacerLow     = 0.035 // queued sound below this: hurry
	pacerHigh    = 0.14  // above this: slow down
	maxFrames    = 4     // most frames run in one go (host.js)
	fastFrames   = 4     // frames per step while fast-forwarding
	maxStepDelta = 0.1   // a longer gap (a stall) counts as this
)

func newPacer(fps float64) *pacer {
	if fps <= 0 || math.IsNaN(fps) {
		fps = 60
	}
	return &pacer{frameTime: 1 / fps}
}

// step: dt seconds have passed; queued is the sound waiting to be played, in
// seconds (< 0 when there is no sound device: then only the clock counts).
// host.js steps once per screen refresh and nudges by half a frame each
// time; this steps more often, so the nudge is scaled by dt to the same rate.
func (p *pacer) step(dt, queued float64, fast bool) int {
	if dt > maxStepDelta {
		dt = maxStepDelta
	}
	if dt < 0 {
		dt = 0
	}
	if fast {
		p.acc = 0
		return fastFrames
	}
	p.acc += dt
	if queued >= 0 {
		if queued < pacerLow {
			p.acc += dt * 0.5
		} else if queued > pacerHigh {
			p.acc -= dt * 0.5
		}
	}
	n := 0
	for p.acc >= p.frameTime && n < maxFrames {
		n++
		p.acc -= p.frameTime
	}
	if p.acc > p.frameTime*maxFrames {
		p.acc = 0
	}
	return n
}

// untilNext: how long until the next frame is due.
func (p *pacer) untilNext() float64 {
	d := p.frameTime - p.acc
	if d < 0 {
		return 0
	}
	return d
}

func (p *pacer) reset() { p.acc = 0 }

// toPCM converts a core's float samples (mono or interleaved stereo) to
// 16-bit stereo for the sound card, at volume 0..1, clipping.
func toPCM(dst []int16, src []float32, stereo bool, volume float32) []int16 {
	return games.ToPCM(dst, src, stereo, volume)
}
