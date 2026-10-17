package ymf278b

import (
	"encoding/binary"
	"errors"
)

const stateVersion = 1

// ErrState is returned for a damaged state.
var ErrState = errors.New("ymf278b: bad state")

type stateW struct{ b []byte }

func (w *stateW) u32(v uint32) { w.b = binary.LittleEndian.AppendUint32(w.b, v) }
func (w *stateW) i(v int)      { w.b = binary.LittleEndian.AppendUint64(w.b, uint64(int64(v))) }
func (w *stateW) ints(v ...int) {
	for _, x := range v {
		w.i(x)
	}
}
func (w *stateW) bools(v ...bool) {
	for _, x := range v {
		if x {
			w.b = append(w.b, 1)
		} else {
			w.b = append(w.b, 0)
		}
	}
}

type stateR struct {
	b   []byte
	bad bool
}

func (r *stateR) take(n int) []byte {
	if len(r.b) < n {
		r.bad = true
		return make([]byte, n)
	}
	v := r.b[:n]
	r.b = r.b[n:]
	return v
}
func (r *stateR) u32() uint32 { return binary.LittleEndian.Uint32(r.take(4)) }
func (r *stateR) i() int      { return int(int64(binary.LittleEndian.Uint64(r.take(8)))) }
func (r *stateR) ints(v ...*int) {
	for _, p := range v {
		*p = r.i()
	}
}
func (r *stateR) bools(v ...*bool) {
	for _, p := range v {
		*p = r.take(1)[0] != 0
	}
}

// AppendState appends the chip's state (not the wave memory) to b.
func (c *Chip) AppendState(b []byte) []byte {
	w := &stateW{b: append(b, stateVersion)}
	w.b = append(w.b, c.regs[:]...)
	w.ints(c.pcmAddr, int(c.memAddr), int(c.egCount))
	for i := range c.slots {
		s := &c.slots[i]
		w.ints(s.wave, s.format, int(s.start), int(s.loop), int(s.end), int(s.pos), s.fnum, s.oct,
			s.tl, s.tlTarget, s.pan, s.lfo, s.vib, s.am, s.ar, s.d1r, s.dl, s.d2r, s.rc, s.rr,
			s.env, s.state, int(s.lfoPhase))
		w.bools(s.on, s.key, s.damp, s.lfoHold)
	}
	f := &c.fm
	w.b = append(w.b, f.regs[:]...)
	w.ints(f.addr[0], f.addr[1], int(f.four), int(f.bd), int(f.egCount), f.amPos, f.amCount,
		int(f.noise), int(f.t1), int(f.t2), f.t1n, f.t2n, f.tick, int(f.ctrl), int(f.st))
	w.bools(f.isNew, f.nts)
	for i := range f.ops {
		o := &f.ops[i]
		w.ints(int(o.phase), o.env, o.state, int(o.key), o.mult, o.ksl, o.tl, o.ar, o.dr, o.sl, o.rr, o.ws,
			int(o.out[0]), int(o.out[1]))
		w.bools(o.am, o.vib, o.egt, o.ksr)
	}
	for i := range f.ch {
		ch := &f.ch[i]
		w.ints(ch.fnum, ch.block, ch.fb, ch.cnt, int(ch.outs))
	}
	w.ints(c.fmAcc, int(c.fmLast[0][0]), int(c.fmLast[0][1]), int(c.fmLast[1][0]), int(c.fmLast[1][1]))
	return w.b
}

// LoadState reads what AppendState wrote and returns the rest of b.
func (c *Chip) LoadState(b []byte) ([]byte, error) {
	if len(b) == 0 || b[0] != stateVersion {
		return b, ErrState
	}
	r := &stateR{b: b[1:]}
	n := *c
	n.regs = [0x100]uint8(r.take(0x100))
	var memAddr, egCount int
	r.ints(&n.pcmAddr, &memAddr, &egCount)
	n.memAddr, n.egCount = uint32(memAddr), uint32(egCount)
	for i := range n.slots {
		s := &n.slots[i]
		var start, loop, end, pos, lfoPhase int
		r.ints(&s.wave, &s.format, &start, &loop, &end, &pos, &s.fnum, &s.oct,
			&s.tl, &s.tlTarget, &s.pan, &s.lfo, &s.vib, &s.am, &s.ar, &s.d1r, &s.dl, &s.d2r, &s.rc, &s.rr,
			&s.env, &s.state, &lfoPhase)
		s.start, s.loop, s.end, s.pos, s.lfoPhase = uint32(start), uint32(loop), uint32(end), uint64(pos), uint32(lfoPhase)
		r.bools(&s.on, &s.key, &s.damp, &s.lfoHold)
	}
	f := &n.fm
	f.regs = [0x200]uint8(r.take(0x200))
	var four, bd, eg, noise, t1, t2, ctrl, st int
	r.ints(&f.addr[0], &f.addr[1], &four, &bd, &eg, &f.amPos, &f.amCount,
		&noise, &t1, &t2, &f.t1n, &f.t2n, &f.tick, &ctrl, &st)
	f.four, f.bd, f.egCount, f.noise = uint8(four), uint8(bd), uint32(eg), uint32(noise)
	f.t1, f.t2, f.ctrl, f.st = uint8(t1), uint8(t2), uint8(ctrl), uint8(st)
	r.bools(&f.isNew, &f.nts)
	for i := range f.ops {
		o := &f.ops[i]
		var phase, key, o0, o1 int
		r.ints(&phase, &o.env, &o.state, &key, &o.mult, &o.ksl, &o.tl, &o.ar, &o.dr, &o.sl, &o.rr, &o.ws, &o0, &o1)
		o.phase, o.key, o.out = uint32(phase), uint8(key), [2]int32{int32(o0), int32(o1)}
		r.bools(&o.am, &o.vib, &o.egt, &o.ksr)
	}
	for i := range f.ch {
		ch := &f.ch[i]
		var outs int
		r.ints(&ch.fnum, &ch.block, &ch.fb, &ch.cnt, &outs)
		ch.outs = uint8(outs)
	}
	var a, b0, b1, b2, b3 int
	r.ints(&a, &b0, &b1, &b2, &b3)
	n.fmAcc, n.fmLast = a, [2][2]int32{{int32(b0), int32(b1)}, {int32(b2), int32(b3)}}
	in := func(v, hi int) bool { return v >= 0 && v <= hi }
	for i := range n.slots {
		s := &n.slots[i]
		if !in(s.lfo, 7) || !in(s.vib, 7) || !in(s.am, 7) || !in(s.format, 3) || !in(s.oct+8, 15) ||
			!in(s.fnum, 0x3ff) || !in(s.env, envMax) || !in(s.tl, 0x7f) || !in(s.tlTarget, 0x7f) || !in(s.pan, 15) {
			return b, ErrState
		}
	}
	for i := range f.ops {
		o := &f.ops[i]
		if !in(o.mult, 15) || !in(o.ksl, 3) || !in(o.tl, 63) || !in(o.ws, 7) || !in(o.env, envMax) ||
			!in(o.ar, 15) || !in(o.dr, 15) || !in(o.sl, 15) || !in(o.rr, 15) {
			return b, ErrState
		}
	}
	for i := range f.ch {
		if ch := &f.ch[i]; !in(ch.fnum, 0x3ff) || !in(ch.block, 7) || !in(ch.fb, 7) || !in(f.amPos, 209) {
			return b, ErrState
		}
	}
	if r.bad || n.fmAcc < 0 || n.fmAcc >= fmClocks || n.pcmAddr < 0 || n.pcmAddr > 0xff || f.addr[0]&^0xff != 0 || f.addr[1]&^0xff != 0 {
		return b, ErrState
	}
	*c = n
	return r.b, nil
}
