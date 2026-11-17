package model3

import (
	"fmt"
	"os"

	"retropixels/cores/m68k"
)

var debugSound = os.Getenv("M3_SOUNDDEBUG") != ""

// The Digital Sound Board, type 2: an 11 MHz 68000 with 128 KB of program
// ROM and 128 KB of RAM that hears the same MIDI bytes as the sound board and
// plays 32 kHz MPEG Layer II music from up to 16 MB of ROM through an MPEG
// decoder chip.
//
// The 68000 reads each command byte (level 1 interrupt) from 0xC00001 and
// has a 1 kHz tick (level 2). It drives the decoder by writing command
// bytes, some followed by arguments, to 0xE00003: set the start or end
// address (three bytes each), play, stop, the left and right volumes and the
// stereo routing.
//
// Written for RetroPixels from the board's documented layout and the
// commands its program sends.

const (
	dsbHz       = 11000000
	dsbTickEach = dsbHz / 1000
	dsbMusicHz  = 32000
)

// what the decoder does with a command and its arguments
type dsbCmd struct {
	args int
	do   func(d *dsb2, a []uint8)
}

var dsbCmds = func() map[uint8]dsbCmd {
	m := map[uint8]dsbCmd{}
	both := func(c uint8, cmd dsbCmd) { m[c], m[c|1] = cmd, cmd }
	addr := func(a []uint8) uint32 { return uint32(a[0])<<16 | uint32(a[1])<<8 | uint32(a[2]) }
	both(0x14, dsbCmd{3, func(d *dsb2, a []uint8) { // start address
		d.start = addr(a)
		if d.playing { // a new loop start for the playing tune
			d.loopFrom(d.start, d.end, true)
		}
	}})
	both(0x24, dsbCmd{3, func(d *dsb2, a []uint8) { // end address
		d.end = addr(a)
		d.route = routeStereo
	}})
	both(0x74, dsbCmd{0, func(d *dsb2, _ []uint8) { d.play() }})
	both(0x84, dsbCmd{0, func(d *dsb2, _ []uint8) { d.stop() }})
	m[0xa4] = dsbCmd{1, func(d *dsb2, a []uint8) {
		if a[0] == 0x75 {
			d.play()
		}
	}}
	m[0xb4] = dsbCmd{1, func(d *dsb2, a []uint8) {
		if a[0] == 0x96 {
			d.stop()
		}
	}}
	m[0xa0] = dsbCmd{1, func(d *dsb2, a []uint8) { d.setRoute(a[0], routeLeft) }}
	m[0xb1] = dsbCmd{1, func(d *dsb2, a []uint8) { d.setRoute(a[0], routeRight) }}
	left := dsbCmd{1, func(d *dsb2, a []uint8) { d.vol[0] = a[0] }}
	right := dsbCmd{1, func(d *dsb2, a []uint8) { d.vol[1] = a[0] }}
	m[0xb0], m[0xb6] = left, left
	m[0xa1], m[0xa7] = right, right
	for _, c := range []uint8{0xa3, 0xa5, 0xb2, 0xb5} { // one argument, no effect here
		m[c] = dsbCmd{1, func(*dsb2, []uint8) {}}
	}
	return m
}()

const (
	routeStereo = iota
	routeLeft   // both speakers play the left channel
	routeRight  // both play the right
)

type dsb2 struct {
	cpu  *m68k.CPU
	prog []byte
	mpeg []byte
	ram  []byte

	cmdQ   []uint8 // MIDI bytes waiting for the 68000
	cmdNow uint8

	// the decoder's command input
	cmd     *dsbCmd
	args    []uint8
	start   uint32
	end     uint32
	playing bool
	vol     [2]uint8
	route   int

	cycles   int64 // ahead of (positive) or behind the frame
	nextTick int64
	frameAcc float64

	// the music
	dec      mpegDecoder
	loopA    int // the stretch of ROM being played, and whether it loops
	loopB    int
	loop     bool
	at       int
	silent   bool
	pcm      [mpegMaxSamplesPerFrm]int16
	pcmLen   int // samples per channel
	pcmAt    int
	pcmCh    int
	resample float64
	cur, nxt [2]int16
}

func newDSB2(prog, mpeg []byte) *dsb2 {
	d := &dsb2{prog: prog, mpeg: mpeg, ram: make([]byte, 0x20000)}
	if len(d.prog) < 0x20000 {
		p := make([]byte, 0x20000)
		copy(p, d.prog)
		d.prog = p
	}
	d.cpu = m68k.New(d)
	return d
}

func (d *dsb2) reset() {
	clear(d.ram)
	d.cmdQ = d.cmdQ[:0]
	d.cmd, d.args = nil, d.args[:0]
	d.start, d.end = 0, 0
	d.playing = false
	d.vol = [2]uint8{0xff, 0xff}
	d.route = routeStereo
	d.silent = true
	d.pcmLen, d.pcmAt = 0, 0
	d.resample = 0
	d.cur, d.nxt = [2]int16{}, [2]int16{}
	d.cpu.SetIRQ(0)
	d.cpu.Cycles = 0
	d.cpu.Reset()
	d.cycles = 0
	d.nextTick = dsbTickEach
	d.frameAcc = 0
}

func (d *dsb2) sendCommand(v uint8) {
	if debugSound {
		fmt.Printf("cmd %02X pc %06X\n", v, d.cpu.PC)
	}
	d.cmdQ = append(d.cmdQ, v)
}

// runFor runs the 68000 for about n cycles and returns how many it ran.
func (d *dsb2) runFor(n int64) int64 {
	from := d.cpu.Cycles
	d.cpu.Run(from + n)
	return d.cpu.Cycles - from
}

// runFrame runs the board for one video frame and adds n samples (44.1 kHz)
// of music to out (interleaved left, right).
func (d *dsb2) runFrame(out []float32, n int) {
	// each command byte interrupts the 68000, which takes it at once
	for _, v := range d.cmdQ {
		d.cmdNow = v
		d.cpu.SetIRQ(1)
		d.cycles += d.runFor(500)
	}
	d.cmdQ = d.cmdQ[:0]
	d.frameAcc += dsbHz / 57.52416
	frame := int64(d.frameAcc)
	d.frameAcc -= float64(frame)
	for d.cycles < frame {
		if d.cycles >= d.nextTick {
			d.cpu.SetIRQ(2)
			d.nextTick += dsbTickEach
		}
		d.cycles += d.runFor(min(d.nextTick, frame) - d.cycles)
	}
	d.cycles -= frame
	d.nextTick -= frame
	d.mix(out, n)
}

// mix adds n output samples of the music, resampled from 32 kHz, with the
// volumes and routing the program set.
func (d *dsb2) mix(out []float32, n int) {
	gl, gr := float32(d.vol[0])/255, float32(d.vol[1])/255
	switch d.route {
	case routeLeft:
		gr = gl
	case routeRight:
		gl = gr
	}
	for i := 0; i < n; i++ {
		f := float32(d.resample)
		l := float32(d.cur[0]) + (float32(d.nxt[0])-float32(d.cur[0]))*f
		r := float32(d.cur[1]) + (float32(d.nxt[1])-float32(d.cur[1]))*f
		switch d.route {
		case routeLeft:
			r = l
		case routeRight:
			l = r
		}
		out[2*i] += l * gl
		out[2*i+1] += r * gr
		d.resample += float64(dsbMusicHz) / soundRate
		for d.resample >= 1 {
			d.resample--
			d.cur = d.nxt
			d.nxt = d.nextSample()
		}
	}
}

// ---- the decoder chip ----

func (d *dsb2) setRoute(v uint8, mono int) {
	if v != 0 {
		d.route = mono
	} else {
		d.route = routeStereo
	}
}

func (d *dsb2) play() {
	if debugSound {
		fmt.Printf("DSB play %06X-%06X\n", d.start, d.end)
	}
	d.playing = true
	d.dec.init()
	d.loopA, d.loopB, d.loop = int(d.start), int(d.end), false
	d.at = d.loopA
	d.pcmLen, d.pcmAt = 0, 0
	d.silent = false
}

func (d *dsb2) stop() {
	d.playing = false
	d.silent = true
}

// loopFrom sets the stretch the tune loops over, without moving where it is.
func (d *dsb2) loopFrom(a, b uint32, loop bool) {
	d.loopA, d.loopB, d.loop = int(a), int(b), loop
}

// writeDecoder takes a byte the 68000 writes to the decoder.
func (d *dsb2) writeDecoder(b uint8) {
	if debugSound {
		fmt.Printf("decoder %02X\n", b)
	}
	if d.cmd == nil {
		c, ok := dsbCmds[b]
		if !ok {
			return
		}
		d.cmd, d.args = &c, d.args[:0]
	} else {
		d.args = append(d.args, b)
	}
	if len(d.args) == d.cmd.args {
		c := d.cmd
		d.cmd = nil
		c.do(d, d.args)
	}
}

// nextSample is the music's next 32 kHz sample pair.
func (d *dsb2) nextSample() [2]int16 {
	if d.silent {
		return [2]int16{}
	}
	for tries := 0; d.pcmAt >= d.pcmLen; tries++ {
		if tries > 4 {
			return [2]int16{}
		}
		var data []byte
		if end := min(d.loopB, len(d.mpeg)); d.at >= 0 && d.at < end {
			data = d.mpeg[d.at:end]
		}
		n, used, ch := d.dec.decodeFrame(data, d.pcm[:])
		d.at += used
		d.pcmLen, d.pcmAt, d.pcmCh = n, 0, ch
		if d.at >= d.loopB-mpegHdrSize || (n == 0 && used == 0) {
			switch {
			case d.loop:
				d.at = d.loopA
			case n == 0:
				d.silent = true
				return [2]int16{}
			}
		}
	}
	i := d.pcmAt * d.pcmCh
	d.pcmAt++
	return [2]int16{d.pcm[i], d.pcm[i+d.pcmCh-1]}
}

// ---- the 68000's bus ----

func (d *dsb2) Read8(a uint32) uint8 {
	switch {
	case a < 0x20000:
		return d.prog[a]
	case a == 0xc00001:
		return d.cmdNow
	case a == 0xc00003, a == 0xe80001: // a command is there; the decoder is ready
		return 1
	case a >= 0xf00000 && a < 0xf20000:
		return d.ram[a&0x1ffff]
	}
	return 0
}

func (d *dsb2) Read16(a uint32) uint16 {
	switch {
	case a < 0x20000:
		return be16(d.prog, a)
	case a >= 0xf00000 && a < 0xf20000:
		return be16(d.ram, a&0x1fffe)
	}
	return 0
}

func (d *dsb2) Write8(a uint32, v uint8) {
	switch {
	case a >= 0xf00000 && a < 0xf20000:
		d.ram[a&0x1ffff] = v
	case a == 0xe00003:
		d.writeDecoder(v)
	}
}

func (d *dsb2) Write16(a uint32, v uint16) {
	if a >= 0xf00000 && a < 0xf20000 {
		d.ram[a&0x1fffe] = uint8(v >> 8)
		d.ram[a&0x1fffe+1] = uint8(v)
	}
}

// Ack: the interrupts are autovectored and end when taken.
func (d *dsb2) Ack(level int) int {
	d.cpu.SetIRQ(0)
	return -1
}
