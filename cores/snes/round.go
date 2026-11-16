package snes

import "math"

// jsRound rounds to the nearest whole number, with halves going up (toward
// +Infinity), the rounding the chip tables in this core were built with.
func jsRound(x float64) float64 {
	r := math.Floor(x)
	if x-r >= 0.5 {
		r++
	}
	return r
}
