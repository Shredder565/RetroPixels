package model3

import (
	"fmt"
	"os"

	"retropixels/cores/m68k"
)

var debugSound = os.Getenv("M3_SOUNDDEBUG") != ""

// The Digital Sound Board, type 2: a 68000 at 11 MHz that hears the same
// MIDI bytes as the sound board and plays MPEG Layer II music (32 kHz) from
// its ROM. As in Supermodel, the 68000 gets each command byte with an
// interrupt and has a 1 kHz timer interrupt; it starts and stops the music
// by writing commands to the MPEG decoder, whose state machine is here.
//
// The command handling, volumes and stereo modes follow Supermodel's
// Src/Model3/DSB.cpp (Copyright 2003-2026 The Supermodel Team), so this file
// is under the GNU General Public License, version 3 or (at your option) any
// later version: see LICENSE-GPL-3.0.txt and NOTICE.txt.

const (
	dsbHz          = 11000000
	dsbTimerPeriod = dsbHz / 1000
	dsbMPEGRate    = 32000
)

const (
	mpegIdle = iota
	mpegGot14
	mpeg14_0
	mpeg14_1
	mpegGot24
	mpeg24_0
	mpeg24_1
	mpegGot74
	mpegGotA0
	mpegGotA1
	mpegGotA3
	mpegGotA4
	mpegGotA5
	mpegGotA7
	mpegGotB0
	mpegGotB1
	mpegGotB2
	mpegGotB4
	mpegGotB5
	mpegGotB6
)

const (
	stereoBoth = iota
	stereoMonoLeft
	stereoMonoRight
)

type dsb2 struct {
	cpu  *m68k.CPU
	prog []byte
	mpeg []byte
	ram  []byte

	fifo         [256]uint8
	fifoW, fifoR uint8
	cmdLatch     uint8

	mpegState          int
	mpegStart, mpegEnd uint32
	playing            bool
	volume             [2]uint8
	stereo             int

	cyclesThisFrame int64
	nextTimer       int64
	frameFrac       float64

	// the music decoder (Supermodel's MpegDec)
	dec        mpegDecoder
	decStart   int // where the playing data starts and ends in the ROM
	decEnd     int
	decPos     int // absolute position in the ROM
	decLoop    bool
	decStopped bool
	decLoaded  bool
	pcm        [mpegMaxSamplesPerFrm]int16
	pcmN       int // samples per channel in pcm
	pcmPos     int
	pcmCh      int

	// resampler from 32 kHz to 44.1 kHz
	prevL, prevR, nextL, nextR int16
	frac                       float64
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
	for i := range d.ram {
		d.ram[i] = 0
	}
	d.decStopped = true
	d.prevL, d.prevR, d.nextL, d.nextR = 0, 0, 0, 0
	d.frac = 0
	d.fifo = [256]uint8{}
	d.fifoW, d.fifoR = 0, 0
	d.mpegState = mpegIdle
	d.mpegStart, d.mpegEnd = 0, 0
	d.playing = false
	d.volume = [2]uint8{0xFF, 0xFF}
	d.stereo = stereoBoth
	d.cpu.SetIRQ(0)
	d.cpu.Cycles = 0
	d.cpu.Reset()
	d.cyclesThisFrame = 0
	d.nextTimer = dsbTimerPeriod
	d.frameFrac = 0
}

func (d *dsb2) sendCommand(v uint8) {
	if debugSound {
		fmt.Printf("cmd %02X pc %06X\n", v, d.cpu.PC)
	}
	d.fifo[d.fifoW] = v
	d.fifoW++
}

// run runs the 68000 for n cycles and returns the cycles it took.
func (d *dsb2) run(n int64) int64 {
	start := d.cpu.Cycles
	d.cpu.Run(start + n)
	return d.cpu.Cycles - start
}

// runFrame runs the board for one video frame and mixes n samples (44.1
// kHz) of music into out (interleaved left, right).
func (d *dsb2) runFrame(out []float32, n int) {
	for d.fifoR != d.fifoW {
		d.cmdLatch = d.fifo[d.fifoR]
		d.fifoR++
		d.cpu.SetIRQ(1)
		d.cyclesThisFrame += d.run(500)
	}
	d.frameFrac += dsbHz / 57.52416
	period := int64(d.frameFrac)
	d.frameFrac -= float64(period)
	for d.cyclesThisFrame < period {
		if d.cyclesThisFrame >= d.nextTimer {
			d.cpu.SetIRQ(2)
			d.nextTimer += dsbTimerPeriod
		}
		end := d.nextTimer
		if period < end {
			end = period
		}
		d.cyclesThisFrame += d.run(end - d.cyclesThisFrame)
	}
	d.cyclesThisFrame -= period
	d.nextTimer -= period

	var volL, volR float32
	src := d.stereo
	switch src {
	case stereoMonoLeft:
		volL, volR = float32(d.volume[0]), float32(d.volume[0])
	case stereoMonoRight:
		volL, volR = float32(d.volume[1]), float32(d.volume[1])
	default:
		volL, volR = float32(d.volume[0]), float32(d.volume[1])
	}
	volL /= 255
	volR /= 255
	const step = float64(dsbMPEGRate) / soundRate
	for i := 0; i < n; i++ {
		f := float32(d.frac)
		l := float32(d.prevL)*(1-f) + float32(d.nextL)*f
		r := float32(d.prevR)*(1-f) + float32(d.nextR)*f
		switch src {
		case stereoMonoLeft:
			r = l
		case stereoMonoRight:
			l = r
		}
		out[2*i] += l * volL
		out[2*i+1] += r * volR
		d.frac += step
		for d.frac >= 1 {
			d.frac--
			d.prevL, d.prevR = d.nextL, d.nextR
			d.nextL, d.nextR = d.decodeSample()
		}
	}
}

// ---------------------------------------------------------------- music

func (d *dsb2) setMemory(start, end uint32, loop bool) {
	d.dec.init()
	d.decStart = int(start)
	d.decEnd = int(end)
	d.decPos = d.decStart
	d.pcmN, d.pcmPos = 0, 0
	d.decLoop = loop
	d.decStopped = false
	d.decLoaded = true
}

// updateMemory changes where the music loops back to, keeping the place it
// is playing.
func (d *dsb2) updateMemory(start uint32, length uint32, loop bool) {
	d.decStart = int(start)
	d.decEnd = int(start) + int(int32(length))
	d.decLoop = loop
}

func (d *dsb2) endOfBuffer() bool { return d.decPos >= d.decEnd-mpegHdrSize }

// decodeSample gives the next 32 kHz stereo sample of the music.
func (d *dsb2) decodeSample() (int16, int16) {
	if d.decStopped || !d.decLoaded {
		return 0, 0
	}
	for tries := 0; d.pcmPos >= d.pcmN; tries++ {
		if tries > 4 {
			return 0, 0
		}
		end := d.decEnd
		if end > len(d.mpeg) {
			end = len(d.mpeg)
		}
		var data []byte
		if d.decPos >= 0 && d.decPos < end {
			data = d.mpeg[d.decPos:end]
		}
		n, used, ch := d.dec.decodeFrame(data, d.pcm[:])
		d.decPos += used
		d.pcmN, d.pcmPos, d.pcmCh = n, 0, ch
		if d.endOfBuffer() || (n == 0 && used == 0) {
			if d.decLoop {
				d.decPos = d.decStart
			} else if d.pcmN == 0 {
				d.decStopped = true
				return 0, 0
			}
		}
	}
	i := d.pcmPos * d.pcmCh
	d.pcmPos++
	return d.pcm[i], d.pcm[i+d.pcmCh-1]
}

func (d *dsb2) writeMPEGFIFO(b uint8) {
	if debugSound {
		fmt.Printf("fifo %02X state %d\n", b, d.mpegState)
	}
	startPlay := func() {
		if debugSound {
			fmt.Printf("DSB play %06X-%06X\n", d.mpegStart, d.mpegEnd)
		}
		d.playing = true
		d.setMemory(d.mpegStart, d.mpegEnd, false)
	}
	switch d.mpegState {
	case mpegIdle:
		switch b {
		case 0x14, 0x15:
			d.mpegState = mpegGot14
		case 0x24, 0x25:
			d.mpegState = mpegGot24
		case 0x74, 0x75:
			startPlay()
			d.mpegState = mpegIdle
		case 0x84, 0x85:
			d.decStopped = true
			d.playing = false
		case 0xa0:
			d.mpegState = mpegGotA0
		case 0xa1:
			d.mpegState = mpegGotA1
		case 0xa3:
			d.mpegState = mpegGotA3
		case 0xa4:
			d.mpegState = mpegGotA4
		case 0xa5:
			d.mpegState = mpegGotA5
		case 0xa7:
			d.mpegState = mpegGotA7
		case 0xb0:
			d.mpegState = mpegGotB0
		case 0xb1:
			d.mpegState = mpegGotB1
		case 0xb2:
			d.mpegState = mpegGotB2
		case 0xb4:
			d.mpegState = mpegGotB4
		case 0xb5:
			d.mpegState = mpegGotB5
		case 0xb6:
			d.mpegState = mpegGotB6
		}
	case mpegGot14:
		d.mpegStart = d.mpegStart&0x00FFFF | uint32(b)<<16
		d.mpegState++
	case mpeg14_0:
		d.mpegStart = d.mpegStart&0xFF00FF | uint32(b)<<8
		d.mpegState++
	case mpeg14_1:
		d.mpegStart = d.mpegStart&0xFFFF00 | uint32(b)
		d.mpegState = mpegIdle
		if d.playing {
			d.updateMemory(d.mpegStart, d.mpegEnd-d.mpegStart, true)
		}
	case mpegGot24:
		d.mpegEnd = d.mpegEnd&0x00FFFF | uint32(b)<<16
		d.mpegState++
	case mpeg24_0:
		d.mpegEnd = d.mpegEnd&0xFF00FF | uint32(b)<<8
		d.mpegState++
	case mpeg24_1:
		d.mpegEnd = d.mpegEnd&0xFFFF00 | uint32(b)
		d.stereo = stereoBoth
		d.mpegState = mpegIdle
	case mpegGotA0:
		if b != 0 {
			d.stereo = stereoMonoLeft
		} else {
			d.stereo = stereoBoth
		}
		d.mpegState = mpegIdle
	case mpegGotA4:
		d.mpegState = mpegIdle
		if b == 0x75 {
			startPlay()
		}
	case mpegGotB1:
		if b != 0 {
			d.stereo = stereoMonoRight
		} else {
			d.stereo = stereoBoth
		}
		d.mpegState = mpegIdle
	case mpegGotB4:
		d.mpegState = mpegIdle
		if b == 0x96 {
			d.decStopped = true
			d.playing = false
		}
	case mpegGotB6, mpegGotB0:
		d.volume[0] = b
		d.mpegState = mpegIdle
	case mpegGotA7, mpegGotA1:
		d.volume[1] = b
		d.mpegState = mpegIdle
	case mpegGotA5, mpegGotB5, mpegGotB2, mpegGotA3:
		d.mpegState = mpegIdle
	}
}

// ---------------------------------------------------------------- 68000 bus

func (d *dsb2) Read8(a uint32) uint8 {
	switch {
	case a < 0x20000:
		return d.prog[a]
	case a == 0xc00001:
		return d.cmdLatch
	case a == 0xc00003: // command valid
		return 1
	case a == 0xe80001: // MPEG decoder status
		return 1
	case a >= 0xf00000 && a < 0xf10000:
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
		d.writeMPEGFIFO(v)
	}
}

func (d *dsb2) Write16(a uint32, v uint16) {
	if a >= 0xf00000 && a < 0xf20000 {
		d.ram[a&0x1fffe] = uint8(v >> 8)
		d.ram[a&0x1fffe+1] = uint8(v)
	}
}

// Ack: the interrupts use the autovectors and are dropped when taken.
func (d *dsb2) Ack(level int) int {
	d.cpu.SetIRQ(0)
	return -1
}
