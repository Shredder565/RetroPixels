package atari

import (
	"math"

	"retropixels/cores/arcade"
	"retropixels/cores/pokey"
)

// newPOKEY: the arcade boards' POKEY, held while SKCTL resets it.
func newPOKEY() *pokey.Chip {
	p := pokey.New()
	p.HoldOnReset = true
	return p
}

// pokeyOut turns the POKEY's summed channel volumes (0..1) into the board's
// sound: the chip's output stage isn't linear (quiet channels are relatively
// louder than the sum of the volumes says), and the board's output
// capacitor rounds off the square waves' highest harmonics (a 3.5 kHz low
// pass).
type pokeyOut struct{ lp float64 }

var pokeyCut = 1 - math.Exp(-2*math.Pi*3500/(1512000/32))

func (o *pokeyOut) next(x float64) float64 {
	o.lp += (0.67*(1-math.Exp(-6*x)) - o.lp) * pokeyCut
	return o.lp
}

// dopt is one setting of a switch.
func dopt(label string, v int) arcade.DipOption { return arcade.DipOption{Label: label, Value: v} }
