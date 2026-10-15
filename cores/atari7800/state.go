package atari7800

import (
	"encoding/binary"
	"errors"
)

// Save states are a flat list of values written and read back in the same
// order by each part of the machine (no reflection, so the format is fixed by
// the code and the same on every machine).

var errBadState = errors.New("atari7800: this save state is damaged or from a different game")

type stateW struct{ b []byte }

func (w *stateW) u8(v uint8)   { w.b = append(w.b, v) }
func (w *stateW) u32(v uint32) { w.b = binary.LittleEndian.AppendUint32(w.b, v) }
func (w *stateW) i64(v int64)  { w.b = binary.LittleEndian.AppendUint64(w.b, uint64(v)) }
func (w *stateW) int(v int)    { w.i64(int64(v)) }
func (w *stateW) bool(v bool) {
	if v {
		w.u8(1)
	} else {
		w.u8(0)
	}
}
func (w *stateW) bytes(v []byte) { w.u32(uint32(len(v))); w.b = append(w.b, v...) }
func (w *stateW) ints(v []int) {
	w.u32(uint32(len(v)))
	for _, x := range v {
		w.int(x)
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
func (r *stateR) u8() uint8   { return r.take(1)[0] }
func (r *stateR) u32() uint32 { return binary.LittleEndian.Uint32(r.take(4)) }
func (r *stateR) i64() int64  { return int64(binary.LittleEndian.Uint64(r.take(8))) }
func (r *stateR) int() int    { return int(r.i64()) }
func (r *stateR) bool() bool  { return r.u8() != 0 }

// count reads a slice length and checks it against the size the machine has.
func (r *stateR) count(want int) bool {
	n := int(r.u32())
	if r.err == nil && n != want {
		r.err = errBadState
	}
	return r.err == nil
}

// bytesInto fills dst (which must be the same length as what was saved).
func (r *stateR) bytesInto(dst []byte) {
	if r.count(len(dst)) {
		copy(dst, r.take(len(dst)))
	}
}

// bytes reads a slice of any length.
func (r *stateR) bytes() []byte {
	n := int(r.u32())
	if r.err != nil || n > len(r.b) {
		r.err = errBadState
		return nil
	}
	return append([]byte(nil), r.take(n)...)
}

func (r *stateR) intsInto(dst []int) {
	if r.count(len(dst)) {
		for i := range dst {
			dst[i] = r.int()
		}
	}
}
