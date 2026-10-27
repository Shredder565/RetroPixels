package ps3

// The console's MP3 decoder for the MultiStream audio engine is a firmware
// file, /dev_flash/sys/external/flashMP3.pic: SPU code the engine loads into
// its SPU and calls. RetroPixels has no firmware, so it makes its own file:
// a small SPU ELF whose functions are "stop" instructions that RetroPixels
// runs in Go (spuStop sees codes picStopBase+n).

import (
	"bytes"
	"encoding/binary"
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

// mp3FrameLen parses an MPEG audio frame header: the frame's length in
// bytes, samples per channel and channel count (0 if it isn't one).
func mp3FrameLen(h []byte) (n, samples, channels int) {
	if len(h) < 4 || h[0] != 0xFF || h[1]&0xE0 != 0xE0 {
		return 0, 0, 0
	}
	ver := h[1] >> 3 & 3 // 3 MPEG-1, 2 MPEG-2, 0 MPEG-2.5
	layer := h[1] >> 1 & 3
	bri := int(h[2] >> 4)
	sri := int(h[2] >> 2 & 3)
	pad := int(h[2] >> 1 & 1)
	if ver == 1 || layer != 1 || bri == 0 || bri == 15 || sri == 3 {
		return 0, 0, 0 // only Layer III
	}
	rates1 := [...]int{0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320}
	rates2 := [...]int{0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160}
	srs := [...]int{44100, 48000, 32000}
	sr := srs[sri]
	channels = 2
	if h[3]>>6 == 3 {
		channels = 1
	}
	if ver == 3 {
		return 144*rates1[bri]*1000/sr + pad, 1152, channels
	}
	if ver == 2 {
		sr /= 2
	} else {
		sr /= 4
	}
	return 72*rates2[bri]*1000/sr + pad, 576, channels
}

const (
	picOut       = 0x1000 // where in the plugin's memory a frame's PCM goes
	picOutStride = 0x1000 // per channel
)

// picCall runs plugin function n (the SPU called into the plugin). The
// engine calls function 0 to decode one frame:
//
//	r3 the frame (in local store), r4 -> bytes used, r5 -> PCM bytes made
//	per channel, r6.. work areas the engine saves and restores per stream.
//
// It returns where the PCM is (channels 0x1000 apart), 0 when there isn't a
// whole frame, -1 on bad data. For now the PCM is silence (16-bit).
func (m *Machine) picCall(s *spuThread, n uint32) {
	c := s.c
	base := (c.PC - 4 - 8*n) & lsMask
	if n != 0 {
		m.Logf("MP3 plugin: unknown function %d called from %05x", n, c.GPR[0][0])
		c.GPR[3] = Vec{}
		return
	}
	in := c.GPR[3][0] & lsMask
	// find the next frame (skipping anything that isn't one)
	skip := uint32(0)
	var flen, samples, ch int
	for ; skip < 4096; skip++ {
		h := c.LS[(in+skip)&lsMask:]
		if flen, samples, ch = mp3FrameLen(h[:min(4, len(h))]); flen > 0 {
			break
		}
	}
	if flen == 0 {
		c.GPR[3] = splat(0xFFFFFFFF)
		return
	}
	out := base + picOut
	pcm := uint32(samples * 2)
	for k := 0; k < ch; k++ {
		o := out + uint32(k)*picOutStride
		clear(c.LS[o : o+pcm])
	}
	c.setLS32(c.GPR[4][0]&lsMask, skip+uint32(flen))
	c.setLS32(c.GPR[5][0]&lsMask, pcm)
	c.GPR[3] = Vec{out, 0, 0, 0}
}
