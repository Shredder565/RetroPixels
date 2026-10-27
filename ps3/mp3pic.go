package ps3

// The console's MP3 decoder for the MultiStream audio engine is a firmware
// file, /dev_flash/sys/external/flashMP3.pic: SPU code the engine loads into
// its SPU and calls. RetroPixels has no firmware, so it makes its own file:
// a small SPU ELF whose functions are "stop" instructions that RetroPixels
// runs in Go (spuStop sees codes picStopBase+n).

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"math"
	"os"
	"strings"
)

const (
	picPath     = "/dev_flash/sys/external/flashMP3.pic"
	picStopBase = 0x3F00 // stop codes 0x3F00.. are the plugin's functions
	picFuncs    = 16
)

// picFile builds the plugin: function n is at 8*n: stop picStopBase+n; bi $0.
func picFile() []byte {
	var text bytes.Buffer
	for n := 0; n < picFuncs; n++ {
		binary.Write(&text, binary.BigEndian, uint32(picStopBase+n)) // stop
		binary.Write(&text, binary.BigEndian, uint32(0x35000000))    // bi $0
	}
	code := text.Bytes()
	shstr := []byte("\x00.text\x00.bss\x00.shstrtab\x00")

	const ehSize, phSize, shSize = 52, 32, 40
	textOff := uint32(0x80)
	strOff := textOff + uint32(len(code))
	shOff := (strOff + uint32(len(shstr)) + 15) &^ 15
	var b bytes.Buffer
	w := func(v ...any) {
		for _, x := range v {
			binary.Write(&b, binary.BigEndian, x)
		}
	}
	// ELF header
	b.Write([]byte{0x7f, 'E', 'L', 'F', 1, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0})
	w(uint16(2), uint16(23), uint32(1), uint32(0), uint32(ehSize), shOff, uint32(0),
		uint16(ehSize), uint16(phSize), uint16(1), uint16(shSize), uint16(4), uint16(3))
	// program header: the code
	w(uint32(1), textOff, uint32(0), uint32(0), uint32(len(code)), uint32(len(code)+0x3000), uint32(5), uint32(0x80))
	for b.Len() < int(textOff) {
		b.WriteByte(0)
	}
	b.Write(code)
	b.Write(shstr)
	for b.Len() < int(shOff) {
		b.WriteByte(0)
	}
	// section headers: null, .text, .bss, .shstrtab
	w(make([]byte, shSize))
	w(uint32(1), uint32(1), uint32(6), uint32(0), textOff, uint32(len(code)), uint32(0), uint32(0), uint32(16), uint32(0))
	w(uint32(7), uint32(8), uint32(3), uint32(len(code)), strOff, uint32(0x3000), uint32(0), uint32(0), uint32(16), uint32(0))
	w(uint32(12), uint32(3), uint32(0), uint32(0), strOff, uint32(len(shstr)), uint32(0), uint32(0), uint32(1), uint32(0))
	return b.Bytes()
}

// virtualFile is a file RetroPixels provides itself (nil if none).
func virtualFile(path string) []byte {
	if strings.EqualFold(path, picPath) {
		return picFile()
	}
	return nil
}

const (
	picOut       = 0x1000 // where in the plugin's memory a frame's PCM goes
	picOutStride = 0x1000 // per channel
)

var picCalls int

// mp3Dump (PS3_MP3DUMP=dir): each music stream's frames and sound
var mp3Dump = os.Getenv("PS3_MP3DUMP")

// mp3Sine (PS3_MP3SINE): the plugin makes a 1 kHz tone instead (debugging)
var mp3Sine = os.Getenv("PS3_MP3SINE") != ""

// picCall runs plugin function n (the SPU called into the plugin). The
// engine calls function 0 to decode one frame:
//
//	r3 the frame (in local store), r4 -> bytes used, r5 -> PCM bytes made
//	per channel, r9 the stream's context (0x1380 bytes, which the engine
//	saves and restores per stream).
//
// It returns where the PCM is (16-bit, channels 0x1000 apart), 0 when
// there isn't a whole frame, -1 on bad data.
func (m *Machine) picCall(s *spuThread, n uint32) {
	c := s.c
	base := (c.PC - 4 - 8*n) & lsMask
	if n != 0 {
		m.Logf("MP3 plugin: unknown function %d called from %05x", n, c.GPR[0][0])
		c.GPR[3] = Vec{}
		return
	}
	in := c.GPR[3][0] & lsMask
	picCalls++
	// find the next frame (skipping anything that isn't one)
	skip := uint32(0)
	var hd mp3Header
	ok := false
	for ; skip < 4096; skip++ {
		var b [4]byte
		for k := range b {
			b[k] = c.LS[(in+skip+uint32(k))&lsMask]
		}
		if hd, ok = mp3ParseHeader(b[:]); ok {
			break
		}
	}
	if !ok {
		c.GPR[3] = splat(0xFFFFFFFF)
		return
	}
	frame := make([]byte, hd.size)
	for k := range frame {
		frame[k] = c.LS[(in+skip+uint32(k))&lsMask]
	}
	ctx := c.GPR[9][0] & lsMask
	d := &m.mp3
	stereo := hd.channels == 2 || ctx+mp3StateSize > lsSize
	if stereo {
		d = &m.mp3Stereo // (no room in the context: one stereo stream at a time)
	} else {
		d.load(c.LS[ctx : ctx+mp3StateSize])
	}
	var pcm [2][1152]float32
	_, samples, err := d.decode(frame, &pcm)
	if !stereo {
		d.save(c.LS[ctx : ctx+mp3StateSize])
		c.lsWrote(ctx, mp3StateSize)
	}
	if err != nil {
		c.GPR[3] = splat(0xFFFFFFFF)
		return
	}
	if mp3Dump != "" && !stereo {
		f, _ := os.OpenFile(fmt.Sprintf("%s/stream%d.raw", mp3Dump, d.id), os.O_APPEND|os.O_CREATE|os.O_WRONLY, 0o644)
		b := make([]byte, 4*samples)
		for i := 0; i < samples; i++ {
			binary.LittleEndian.PutUint32(b[4*i:], math.Float32bits(pcm[0][i]))
		}
		f.Write(b)
		f.Close()
		f, _ = os.OpenFile(fmt.Sprintf("%s/stream%d.mp3", mp3Dump, d.id), os.O_APPEND|os.O_CREATE|os.O_WRONLY, 0o644)
		f.Write(frame)
		f.Close()
		f, _ = os.OpenFile(fmt.Sprintf("%s/stream%d.txt", mp3Dump, d.id), os.O_APPEND|os.O_CREATE|os.O_WRONLY, 0o644)
		fmt.Fprintf(f, "%.4f %d\n", float64(m.tb)/TBFreq, samples)
		f.Close()
	}
	if mp3Sine {
		f := 1000.0
		if d.id%2 == 0 {
			f = 1500
		}
		for i := 0; i < samples; i++ {
			pcm[0][i] = 0.25 * float32(math.Sin(2*math.Pi*f*float64(d.sinePos)/float64(hd.rate)))
			pcm[1][i] = pcm[0][i]
			d.sinePos++
		}
	}
	out := base + picOut
	for k := 0; k < hd.channels; k++ {
		o := out + uint32(k)*picOutStride
		for i := 0; i < samples; i++ {
			v := int16(max(-32768, min(32767, pcm[k][i]*32768)))
			c.LS[o+2*uint32(i)] = byte(uint16(v) >> 8)
			c.LS[o+2*uint32(i)+1] = byte(v)
		}
		c.lsWrote(o, 2*samples)
	}
	c.setLS32(c.GPR[4][0]&lsMask, skip+uint32(hd.size))
	c.setLS32(c.GPR[5][0]&lsMask, uint32(2*samples))
	c.GPR[3] = Vec{out, 0, 0, 0}
}
