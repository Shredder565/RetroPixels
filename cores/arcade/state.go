package arcade

import (
	"encoding/binary"
	"errors"
	"math"
)

// Save-state helpers for boards: a fixed layout written and read in the same
// order. A StateR that runs out of bytes sets Err (and returns zeros), so a
// board reads everything and checks Err once.

// ErrBadState is the error for a damaged state or one from another board.
var ErrBadState = errors.New("this save state is damaged or from a different game")

// StateW writes a state.
type StateW struct{ B []byte }

func (w *StateW) U8(v uint8)    { w.B = append(w.B, v) }
func (w *StateW) U16(v uint16)  { w.B = binary.LittleEndian.AppendUint16(w.B, v) }
func (w *StateW) U32(v uint32)  { w.B = binary.LittleEndian.AppendUint32(w.B, v) }
func (w *StateW) Int(v int)     { w.B = binary.LittleEndian.AppendUint64(w.B, uint64(int64(v))) }
func (w *StateW) F64(v float64) { w.B = binary.LittleEndian.AppendUint64(w.B, math.Float64bits(v)) }
func (w *StateW) Bool(v bool) {
	if v {
		w.U8(1)
	} else {
		w.U8(0)
	}
}

// Bytes writes v as is (its length is not written).
func (w *StateW) Bytes(v []byte) { w.B = append(w.B, v...) }

// Blob writes v with its length.
func (w *StateW) Blob(v []byte) {
	w.U32(uint32(len(v)))
	w.Bytes(v)
}

func (w *StateW) Str(s string) { w.Blob([]byte(s)) }

func (w *StateW) U16s(v []uint16) {
	for _, x := range v {
		w.U16(x)
	}
}
func (w *StateW) U32s(v []uint32) {
	for _, x := range v {
		w.U32(x)
	}
}
func (w *StateW) F64s(v []float64) {
	for _, x := range v {
		w.F64(x)
	}
}
func (w *StateW) Ints(v []int) {
	for _, x := range v {
		w.Int(x)
	}
}

// StateR reads a state.
type StateR struct {
	B   []byte
	Err error
}

func (r *StateR) take(n int) []byte {
	if r.Err != nil || n < 0 || len(r.B) < n {
		r.Err = ErrBadState
		return make([]byte, max(n, 0))
	}
	v := r.B[:n]
	r.B = r.B[n:]
	return v
}

func (r *StateR) U8() uint8      { return r.take(1)[0] }
func (r *StateR) U16() uint16    { return binary.LittleEndian.Uint16(r.take(2)) }
func (r *StateR) U32() uint32    { return binary.LittleEndian.Uint32(r.take(4)) }
func (r *StateR) Int() int       { return int(int64(binary.LittleEndian.Uint64(r.take(8)))) }
func (r *StateR) F64() float64   { return math.Float64frombits(binary.LittleEndian.Uint64(r.take(8))) }
func (r *StateR) Bool() bool     { return r.U8() != 0 }
func (r *StateR) Bytes(v []byte) { copy(v, r.take(len(v))) }

// Blob reads what StateW.Blob wrote.
func (r *StateR) Blob() []byte {
	n := int(r.U32())
	if n > len(r.B) {
		r.Err = ErrBadState
		return nil
	}
	return append([]byte(nil), r.take(n)...)
}

func (r *StateR) Str() string { return string(r.Blob()) }

func (r *StateR) U16s(v []uint16) {
	for i := range v {
		v[i] = r.U16()
	}
}
func (r *StateR) U32s(v []uint32) {
	for i := range v {
		v[i] = r.U32()
	}
}
func (r *StateR) F64s(v []float64) {
	for i := range v {
		v[i] = r.F64()
	}
}
func (r *StateR) Ints(v []int) {
	for i := range v {
		v[i] = r.Int()
	}
}

// Done returns Err, or ErrBadState when bytes are left over.
func (r *StateR) Done() error {
	if r.Err == nil && len(r.B) != 0 {
		return ErrBadState
	}
	return r.Err
}
