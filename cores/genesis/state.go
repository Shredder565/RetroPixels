package genesis

import (
	"bytes"
	"compress/flate"
	"encoding/binary"
	"errors"
	"io"
	"math"

	"retropixels/cores/m68k"
	"retropixels/cores/sn76489"
	"retropixels/cores/ym2612"
	"retropixels/cores/z80"
)

// Save states: a fixed layout written and read in the same order (the whole
// machine, including the sound filters, so a loaded state carries on exactly
// as the saved game would have).

var errBadState = errors.New("genesis: this save state is damaged or from a different game")

const stateMagic = "RPGEN\x01"

type stateW struct{ b []byte }

func (w *stateW) u8(v uint8)    { w.b = append(w.b, v) }
func (w *stateW) i64(v int64)   { w.b = binary.LittleEndian.AppendUint64(w.b, uint64(v)) }
func (w *stateW) int(v int)     { w.i64(int64(v)) }
func (w *stateW) f64(v float64) { w.b = binary.LittleEndian.AppendUint64(w.b, math.Float64bits(v)) }
func (w *stateW) bool(v bool) {
	if v {
		w.u8(1)
	} else {
		w.u8(0)
	}
}
func (w *stateW) bytes(v []byte) { w.b = append(w.b, v...) }
func (w *stateW) u16s(v []uint16) {
	for _, x := range v {
		w.b = binary.LittleEndian.AppendUint16(w.b, x)
	}
}

type stateR struct {
	b   []byte
	err error
}

func (r *stateR) take(n int) []byte {
	if r.err != nil || n < 0 || len(r.b) < n {
		r.err = errBadState
		return make([]byte, max(n, 0))
	}
	v := r.b[:n]
	r.b = r.b[n:]
	return v
}
func (r *stateR) u8() uint8      { return r.take(1)[0] }
func (r *stateR) i64() int64     { return int64(binary.LittleEndian.Uint64(r.take(8))) }
func (r *stateR) int() int       { return int(r.i64()) }
func (r *stateR) f64() float64   { return math.Float64frombits(binary.LittleEndian.Uint64(r.take(8))) }
func (r *stateR) bool() bool     { return r.u8() != 0 }
func (r *stateR) bytes(v []byte) { copy(v, r.take(len(v))) }
func (r *stateR) u16s(v []uint16) {
	b := r.take(len(v) * 2)
	for i := range v {
		v[i] = binary.LittleEndian.Uint16(b[i*2:])
	}
}

// SaveState returns the whole machine's state.
func (g *Genesis) SaveState() ([]byte, error) {
	w := &stateW{b: make([]byte, 0, 0x30000)}
	w.bytes([]byte(stateMagic))
	w.b = g.cpu.AppendState(w.b)
	w.b = g.z80.AppendState(w.b)
	w.b = g.ym.AppendState(w.b)
	w.b = g.psg.AppendState(w.b)
	v := &g.vdp
	w.bytes(v.vram[:])
	w.u16s(v.cram[:])
	w.u16s(v.vsram[:])
	w.bytes(v.reg[:])
	w.bytes(v.sat[:])
	w.int(v.addr)
	w.int(v.code)
	w.bool(v.pending)
	w.bool(v.fillPending)
	w.bool(v.vintPending)
	w.bool(v.hintPending)
	w.int(v.hcount)
	w.bool(v.sovr)
	w.bool(v.scol)
	w.int(v.odd)
	w.bool(v.prevOverflow)
	w.int(v.readBuf)
	w.bytes(g.ram[:])
	w.bytes(g.zram[:])
	c := g.cart
	w.bytes(c.sram[:])
	w.bytes(c.banks[:])
	w.bytes(c.rt[:])
	w.bool(c.sramMapped)
	w.bool(c.sramProtect)
	w.bool(c.sramUsed)
	w.int(c.jth)
	if e := c.ee; e != nil {
		w.u8(1)
		w.bytes(e.mem)
		for _, x := range []int{e.scl, e.sda, e.out, e.state, e.bit, e.shift, e.addr} {
			w.int(x)
		}
	} else {
		w.u8(0)
	}
	w.bool(g.three[0])
	w.bool(g.three[1])
	w.bytes(g.ioData[:])
	w.bytes(g.ioCtrl[:])
	w.bytes(g.thCount[:])
	w.i64(g.thTime[0])
	w.i64(g.thTime[1])
	w.int(g.busReq)
	w.int(g.zReset)
	w.int(g.zBank)
	w.i64(g.mclk)
	w.i64(g.zc)
	w.i64(g.ymNext)
	w.int(g.psgAcc)
	w.int(g.line)
	w.i64(g.lineStart)
	w.bool(g.vblank)
	w.int(g.hvLatch)
	w.int(g.frame)
	w.int(g.bootFrames)
	w.f64(g.rsPos)
	w.f64(g.lpL)
	w.f64(g.lpR)
	w.f64(g.prevL)
	w.f64(g.prevR)
	w.int(g.width)
	w.int(g.height)
	// The picture: a frame can leave some of the ones before showing (the
	// other field when interlaced, or lines of a different width), so the
	// whole buffer goes in, compressed.
	px := make([]byte, 0, len(g.screen)*3)
	for _, p := range g.screen {
		px = append(px, uint8(p>>16), uint8(p>>8), uint8(p))
	}
	var z bytes.Buffer
	zw, _ := flate.NewWriter(&z, flate.BestSpeed)
	zw.Write(px)
	zw.Close()
	w.int(z.Len())
	w.bytes(z.Bytes())
	return w.b, nil
}

// LoadState restores a state from SaveState. A damaged state leaves the
// machine as it was.
func (g *Genesis) LoadState(b []byte) error {
	r := &stateR{b: b}
	if string(r.take(len(stateMagic))) != stateMagic {
		return errBadState
	}
	// read it all into a copy before changing anything
	t := *g
	t.cart = &cart{}
	*t.cart = *g.cart
	if g.cart.ee != nil {
		e := *g.cart.ee
		e.mem = append([]uint8(nil), e.mem...)
		t.cart.ee = &e
	}
	var err error
	t.cpu = m68k.New(bus{g})
	t.cpu.TASWrites = false
	if r.b, err = t.cpu.LoadState(r.b); err != nil {
		return errBadState
	}
	t.z80 = z80.New(zbus{g})
	if r.b, err = t.z80.LoadState(r.b); err != nil {
		return errBadState
	}
	t.ym = ym2612.New()
	if r.b, err = t.ym.LoadState(r.b); err != nil {
		return errBadState
	}
	t.psg = sn76489.New()
	if r.b, err = t.psg.LoadState(r.b); err != nil {
		return errBadState
	}
	v := &t.vdp
	r.bytes(v.vram[:])
	r.u16s(v.cram[:])
	r.u16s(v.vsram[:])
	r.bytes(v.reg[:])
	r.bytes(v.sat[:])
	v.addr = r.int() & 0xffff
	v.code = r.int() & 0x3f
	v.pending = r.bool()
	v.fillPending = r.bool()
	v.vintPending = r.bool()
	v.hintPending = r.bool()
	v.hcount = r.int() & 0xff
	v.sovr = r.bool()
	v.scol = r.bool()
	v.odd = r.int() & 1
	v.prevOverflow = r.bool()
	v.readBuf = r.int() & 0xffff
	r.bytes(t.ram[:])
	r.bytes(t.zram[:])
	c := t.cart
	r.bytes(c.sram[:])
	r.bytes(c.banks[:])
	r.bytes(c.rt[:])
	c.sramMapped = r.bool()
	c.sramProtect = r.bool()
	c.sramUsed = r.bool()
	c.jth = r.int() & 1
	if hasEE := r.u8() != 0; hasEE != (c.ee != nil) {
		return errBadState
	}
	if e := c.ee; e != nil {
		r.bytes(e.mem)
		for _, p := range []*int{&e.scl, &e.sda, &e.out, &e.state, &e.bit, &e.shift, &e.addr} {
			*p = r.int()
		}
		e.scl, e.sda, e.out, e.shift, e.addr = e.scl&1, e.sda&1, e.out&1, e.shift&0xff, e.addr&e.mask
		if e.state < stIdle || e.state > stRead || e.bit < 0 || e.bit > 8 {
			return errBadState
		}
	}
	t.three[0] = r.bool()
	t.three[1] = r.bool()
	r.bytes(t.ioData[:])
	r.bytes(t.ioCtrl[:])
	r.bytes(t.thCount[:])
	t.thTime[0] = r.i64()
	t.thTime[1] = r.i64()
	t.busReq = r.int() & 1
	t.zReset = r.int() & 1
	t.zBank = r.int() & 0x1ff
	t.mclk = r.i64()
	t.zc = r.i64()
	t.ymNext = r.i64()
	t.psgAcc = r.int()
	t.line = r.int()
	t.lineStart = r.i64()
	t.vblank = r.bool()
	t.hvLatch = r.int()
	t.frame = r.int()
	t.bootFrames = r.int()
	t.rsPos = r.f64()
	t.lpL = r.f64()
	t.lpR = r.f64()
	t.prevL = r.f64()
	t.prevR = r.f64()
	t.width = r.int()
	t.height = r.int()
	if (t.width != 256 && t.width != 320) || t.height < 1 || t.height > 480 ||
		t.mclk < 0 || t.zc < 0 || t.ymNext < t.mclk-lineClk*313 || t.ymNext > t.mclk+lineClk*313 {
		return errBadState
	}
	zn := r.int()
	if zn < 0 || zn > len(r.b) {
		return errBadState
	}
	px, err := io.ReadAll(io.LimitReader(flate.NewReader(bytes.NewReader(r.take(zn))), int64(len(g.screen)*3+1)))
	if err != nil || len(px) != len(g.screen)*3 {
		return errBadState
	}
	if r.err != nil || len(r.b) != 0 {
		return errBadState
	}
	// all read: switch over
	*g = t
	g.vdp.md = g
	for i := range g.screen {
		g.screen[i] = uint32(px[i*3])<<16 | uint32(px[i*3+1])<<8 | uint32(px[i*3+2])
	}
	g.cart.remap()
	for i := 0; i < 64; i++ {
		g.vdp.updatePal(i)
	}
	g.audioL, g.audioR = g.audioL[:0], g.audioR[:0]
	return nil
}
