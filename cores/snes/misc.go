package snes

import (
	"encoding/binary"
	"math"
	"time"
)

// The smaller chips of web/emu/snes_misc.js: OBC1 and the Sharp S-RTC clock.

// obc1: 8 KB battery RAM at $6000-$7FFF with a window ($7FF0-$7FF4) onto
// sprite entries kept in it.
type obc1 struct {
	c   *cart
	own []byte // the chip's own RAM when the header gives less than 8 KB
}

func newOBC1(s *SNES) coproc {
	o := &obc1{c: s.cart}
	if o.c.sramSize < 0x2000 {
		o.own = make([]byte, 0x2000)
	}
	s.claim(0x00, 0x3f, 0x6000, 0x7fff, true)
	return o
}

func (o *obc1) reset() {}

func (o *obc1) ram(i int) int {
	if o.own != nil {
		return int(o.own[i&0x1fff])
	}
	return int(o.c.sram[i&o.c.sramMask])
}

func (o *obc1) set(i int, v uint8) {
	if o.own != nil {
		o.own[i&0x1fff] = v
		return
	}
	o.c.sram[i&o.c.sramMask] = v
}

func (o *obc1) base() int {
	if o.ram(0x1ff5)&1 != 0 {
		return 0x1800
	}
	return 0x1c00
}

func (o *obc1) index() int { return o.ram(0x1ff6) & 0x7f }

func (o *obc1) read(bank, off int) int {
	if off < 0x6000 || off >= 0x8000 {
		return -1
	}
	a := off - 0x6000
	if a >= 0x1ff0 && a <= 0x1ff3 {
		return o.ram(o.base() + o.index()<<2 + a&3)
	}
	if a == 0x1ff4 {
		return o.ram(o.base() + 0x200 + o.index()>>2)
	}
	return o.ram(a)
}

func (o *obc1) write(bank, off int, v uint8) bool {
	if off < 0x6000 || off >= 0x8000 {
		return false
	}
	a := off - 0x6000
	switch {
	case a >= 0x1ff0 && a <= 0x1ff3:
		o.set(o.base()+o.index()<<2+a&3, v)
	case a == 0x1ff4:
		i, sh := o.base()+0x200+o.index()>>2, (o.index()&3)*2
		o.set(i, uint8(o.ram(i)&^(3<<sh)|int(v&3)<<sh))
	default:
		o.set(a, v)
	}
	return true
}

func (o *obc1) getSRAM() []byte {
	if o.own != nil {
		return append([]byte(nil), o.own...)
	}
	return append([]byte(nil), o.c.sram[:o.c.sramSize]...)
}

func (o *obc1) state(s *sio) {
	if o.own != nil {
		s.Bytes(o.own)
	}
}

// srtc: the Sharp S-RTC, read and set 4 bits at a time through $2800 / $2801.
// The time is kept as an offset from the computer's clock, stored after the
// battery RAM.
type srtc struct {
	s      *SNES
	c      *cart
	offset int
	mode   int
	index  int
	regs   [13]uint8
}

func newSRTC(s *SNES, o *chipOpts) coproc {
	r := &srtc{s: s, c: s.cart}
	n := r.c.sramSize
	if o != nil && len(o.sram) >= n+8 && o.sram[n+4] == 0x53 {
		r.offset = int(int32(binary.LittleEndian.Uint32(o.sram[n:])))
	}
	s.claim(0x00, 0x3f, 0x2000, 0x2fff, true)
	r.reset()
	return r
}

func (r *srtc) reset() {
	r.mode, r.index = 0, -1
	r.regs = [13]uint8{}
}

func (r *srtc) latch() {
	d := time.Unix(int64(int(math.Floor(r.s.now()/1000))+r.offset), 0).UTC()
	g := &r.regs
	sec, mn, h, day, y := d.Second(), d.Minute(), d.Hour(), d.Day(), d.Year()
	vals := [13]int{sec % 10, sec / 10, mn % 10, mn / 10, h % 10, h / 10, day % 10, day / 10, int(d.Month()), y % 10, y / 10 % 10, y/100 - 10, int(d.Weekday())}
	for i, v := range vals {
		g[i] = uint8(v)
	}
}

func (r *srtc) commit() {
	g := &r.regs
	year := (int(g[11])+10)*100 + int(g[10])*10 + int(g[9])
	day := int(g[6]) + int(g[7])*10
	if day == 0 {
		day = 1
	}
	t := time.Date(year, time.Month(max(0, int(g[8])-1)+1), day, int(g[4])+int(g[5])*10, int(g[2])+int(g[3])*10, int(g[0])+int(g[1])*10, 0, time.UTC)
	r.offset = int(t.Unix()) - int(math.Floor(r.s.now()/1000))
	g[12] = uint8(t.Weekday())
}

func (r *srtc) read(bank, off int) int {
	if off != 0x2800 {
		return -1
	}
	if r.mode != 1 {
		return 0
	}
	if r.index < 0 {
		r.latch()
		r.index = 0
		return 0x0f
	}
	if r.index > 12 {
		r.index = -1
		return 0x0f
	}
	v := r.regs[r.index]
	r.index++
	return int(v)
}

func (r *srtc) write(bank, off int, v uint8) bool {
	if off != 0x2801 {
		return false
	}
	v &= 15
	switch {
	case v == 0x0d:
		r.mode, r.index = 1, -1
		return true
	case v == 0x0e:
		r.mode = 2
		return true
	case v == 0x0f:
		return true
	}
	if r.mode == 2 {
		switch v {
		case 0x00:
			r.mode, r.index = 3, 0
		case 0x04:
			r.mode = 0
			r.regs = [13]uint8{}
			r.regs[8], r.regs[6], r.regs[11] = 1, 1, 9
			r.commit()
		default:
			r.mode = 0
		}
		return true
	}
	if r.mode == 3 && r.index >= 0 && r.index < 12 {
		r.regs[r.index] = v
		r.index++
		if r.index == 12 {
			r.commit()
			r.index = 13
		}
	}
	return true
}

func (r *srtc) getSRAM() []byte {
	var out []byte
	if r.c.sramSize != 0 {
		out = append(out, r.c.sram[:r.c.sramSize]...)
	}
	tail := make([]byte, 8)
	binary.LittleEndian.PutUint32(tail, uint32(int32(r.offset)))
	tail[4] = 0x53
	return append(out, tail...)
}

func (r *srtc) state(s *sio) {
	s.Ints(&r.offset, &r.mode, &r.index)
	s.Bytes(r.regs[:])
}
