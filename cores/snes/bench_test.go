package snes

import (
	"testing"

	"retropixels/core"
)

// BenchmarkRunFrame: one frame of a busy generated test ROM (random code,
// DMA, HDMA, sound), for the plain console and the heavier chips. A frame
// has 16.7 ms (NTSC) of real time.
func BenchmarkRunFrame(b *testing.B) {
	for _, name := range []string{"lorom", "hirom-sram8-fast", "sa1", "gsu2-fast", "dsp1-lle", "cx4", "sdd1", "spc7110-rtc", "st018"} {
		var v fuzzSpec
		for _, x := range fuzzVariants {
			if x.name == name {
				v = x
			}
		}
		v.seed = 4242
		rom, f := buildFuzz(v)
		b.Run(name, func(b *testing.B) {
			wall := 1.7e12
			g, err := newSNES(rom, core.Options{SampleRate: 48000, Firmware: f.firmware}, func() float64 { return wall })
			if err != nil {
				b.Fatal(err)
			}
			for i := 0; i < 200; i++ { // past the start-up
				g.SetInput(0, uint32(i*37)&0xeff)
				g.RunFrame()
				g.TakeAudio()
			}
			b.ResetTimer()
			for i := 0; i < b.N; i++ {
				g.SetInput(0, uint32(i*37)&0xeff)
				g.RunFrame()
				g.TakeAudio()
				wall += 16.7
			}
		})
	}
}
