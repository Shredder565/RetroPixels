package nes

import "retropixels/core"

// Hooks for arcade boards built from the NES's chips (Nintendo's Vs.
// System): a PPU with its own colours, extra bits on the controller ports
// (coins, DIP switches). A plain NES never sets
// them.

// ArcadeHooks are what an arcade board adds to the console.
type ArcadeHooks struct {
	// Palette, if set, replaces the console's colours: entry e*64+c is
	// colour c with the colour-emphasis bits (PPUMASK bits 5-7) e.
	Palette *[512]uint32
	// Port, if set, gives the value read from $4016 or $4017; serial is the
	// controller's data bit (0 or 1) the console would put on bit 0.
	Port func(addr, serial int) int
	// RGBPPU: the PPU is one of the RGB arcade ones (RP2C03/RP2C04), which
	// never skip a dot on odd frames. Its palette RAM starts cleared (as in
	// MAME; the real power-on contents are unknown).
	RGBPPU bool
}

// NewArcade opens a game (an iNES image) for an arcade board.
func NewArcade(rom []byte, sampleRate int, h ArcadeHooks) (*NES, error) {
	n, err := newNES(rom, core.Options{SampleRate: sampleRate})
	if err != nil {
		return nil, err
	}
	n.hooks = &h
	if h.Palette != nil {
		n.ppu.pal = h.Palette
	}
	if h.RGBPPU {
		n.ppu.noSkip = true
		n.ppu.palRam = [32]byte{}
	}
	return n, nil
}
