package sms

import (
	"encoding/binary"
	"errors"
	"math"

	"retropixels/cores/sn76489"
	"retropixels/cores/z80"
)

// Save states: a fixed layout written and read in the same order (the whole
// machine, including the sound filters, so a loaded state carries on exactly
// as the saved game would have).

var errBadState = errors.New("sms: this save state is damaged or from a different game")

const stateMagic = "RPSMS\x01"

type stateW struct{ b []byte }

func (w *stateW) u8(v uint8)    { w.b = append(w.b, v) }
func (w *stateW) i64(v int)     { w.b = binary.LittleEndian.AppendUint64(w.b, uint64(int64(v))) }
func (w *stateW) f64(v float64) { w.b = binary.LittleEndian.AppendUint64(w.b, math.Float64bits(v)) }
func (w *stateW) bool(v bool) {
	if v {
		w.u8(1)
	} else {
		w.u8(0)
	}
}
func (w *stateW) bytes(v []byte) { w.b = append(w.b, v...) }

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
func (r *stateR) i64() int       { return int(int64(binary.LittleEndian.Uint64(r.take(8)))) }
func (r *stateR) f64() float64   { return math.Float64frombits(binary.LittleEndian.Uint64(r.take(8))) }
func (r *stateR) bool() bool     { return r.u8() != 0 }
func (r *stateR) bytes(v []byte) { copy(v, r.take(len(v))) }

// SaveState returns the whole machine's state.
func (s *SMS) SaveState() ([]byte, error) {
	w := &stateW{b: make([]byte, 0, 0x10000)}
	w.bytes([]byte(stateMagic))
	w.b = s.cpu.AppendState(w.b)
	w.b = s.psg.AppendState(w.b)
	v := &s.vdp
	w.bytes(v.vram[:])
	w.bytes(v.cram[:])
	w.bytes(v.reg[:])
	w.i64(v.addr)
	w.i64(v.code)
	w.bool(v.latch)
	w.u8(v.buf)
	w.u8(v.status)
	w.bool(v.lineIrq)
	w.i64(v.lineCnt)
	w.i64(v.vscroll)
	w.bytes(s.ram[:])
	w.bytes(s.cartRam[:])
	w.bool(s.cartRamUsed)
	w.bool(s.cmRam)
	w.u8(s.ctrl)
	for _, b := range s.slot {
		w.i64(b)
	}
	w.u8(s.port3F)
	w.u8(s.hLatch)
	w.i64(s.height)
	w.i64(s.line)
	w.i64(s.cyc)
	w.i64(s.lineStart)
	w.i64(s.psgCyc)
	w.f64(s.sampleAcc)
	w.i64(s.sampleN)
	w.bool(s.pauseWas)
	w.f64(s.rsPos)
	w.f64(s.prev)
	w.f64(s.lp)
	w.f64(s.hp)
	w.f64(s.hpPrev)
	w.i64(len(s.audio))
	for _, a := range s.audio {
		w.b = binary.LittleEndian.AppendUint32(w.b, math.Float32bits(a))
	}
	return w.b, nil
}

// LoadState restores a state from SaveState. A damaged state leaves the
// machine as it was.
func (s *SMS) LoadState(b []byte) error {
	r := &stateR{b: b}
	if string(r.take(len(stateMagic))) != stateMagic {
		return errBadState
	}
	// check it all reads before changing anything
	t := *s
	cpu := z80.New(bus{s})
	rest, err := cpu.LoadState(r.b)
	if err != nil {
		return errBadState
	}
	psg := sn76489.New()
	if r.b, err = psg.LoadState(rest); err != nil {
		return errBadState
	}
	v := &t.vdp
	r.bytes(v.vram[:])
	r.bytes(v.cram[:])
	r.bytes(v.reg[:])
	v.addr = r.i64() & 0x3fff
	v.code = r.i64() & 3
	v.latch = r.bool()
	v.buf = r.u8()
	v.status = r.u8()
	v.lineIrq = r.bool()
	v.lineCnt = r.i64() & 0xff
	v.vscroll = r.i64() & 0xff
	r.bytes(t.ram[:])
	r.bytes(t.cartRam[:])
	t.cartRamUsed = r.bool()
	t.cmRam = r.bool()
	t.ctrl = r.u8()
	for i := range t.slot {
		t.slot[i] = r.i64() & 0xff
	}
	t.port3F = r.u8()
	t.hLatch = r.u8()
	t.height = r.i64()
	t.line = r.i64()
	t.cyc = r.i64()
	t.lineStart = r.i64()
	t.psgCyc = r.i64()
	t.sampleAcc = r.f64()
	t.sampleN = r.i64()
	t.pauseWas = r.bool()
	t.rsPos = r.f64()
	t.prev = r.f64()
	t.lp = r.f64()
	t.hp = r.f64()
	t.hpPrev = r.f64()
	n := r.i64()
	if r.err != nil || n < 0 || n > len(r.b)/4 || (t.height != 192 && t.height != 224) {
		return errBadState
	}
	audio := make([]float32, n, max(n, 4096))
	for i := range audio {
		audio[i] = math.Float32frombits(binary.LittleEndian.Uint32(r.take(4)))
	}
	if r.err != nil || len(r.b) != 0 {
		return errBadState
	}
	*s.cpu, *s.psg = *cpu, *psg
	t.cpu, t.psg = s.cpu, s.psg
	t.audio = audio
	*s = t
	s.vdp.sms = s
	for i := range s.vdp.pal {
		s.vdp.updatePal(i)
	}
	return nil
}
