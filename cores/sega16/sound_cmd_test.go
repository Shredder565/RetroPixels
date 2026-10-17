package sega16

import (
	"os"
	"strconv"
	"strings"
	"testing"
)

// TestSoundCommands runs the sound board alone (the 68000 held) and sends
// it commands at given frames: SEGA16_CMDS="100:4a,400:44", like the MAME
// script that writes the same commands to MAME's sound latch.
func TestSoundCommands(t *testing.T) {
	spec := os.Getenv("SEGA16_CMDS")
	if spec == "" {
		t.Skip("SEGA16_CMDS not set")
	}
	c := loadSet(t, "altbeast")
	b := c.Board().(*board)
	cmds := map[int]uint8{}
	last := 0
	for _, p := range strings.Split(spec, ",") {
		fr, cmd, _ := strings.Cut(p, ":")
		f, _ := strconv.Atoi(fr)
		v, _ := strconv.ParseUint(cmd, 16, 8)
		cmds[f] = uint8(v)
		last = max(last, f)
	}
	tail := 300
	if n, err := strconv.Atoi(os.Getenv("SEGA16_TAIL")); err == nil {
		tail = n
	}
	var audio []float32
	for f := 1; f <= last+tail; f++ {
		for line := 0; line < lines; line++ {
			b.snd.runTo(float64(line+1) * z80PerLine)
		}
		b.snd.zcyc -= float64(lines) * z80PerLine
		if v, ok := cmds[f]; ok {
			b.snd.command(v)
		}
		audio = append(audio, b.TakeAudio()...)
	}
	writeWAV(t, os.Getenv("SEGA16_WAV"), audio, 48000)
}
