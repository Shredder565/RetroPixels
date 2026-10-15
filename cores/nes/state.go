package nes

import (
	"encoding/binary"
	"errors"
	"math"
)

// sio saves or restores state: the same function describes the fields for
// both directions, so saving and loading can't drift apart.
type sio struct {
	save bool
	buf  []byte
	pos  int
	err  error
}

var errState = errors.New("nes: saved state doesn't match this game")

func (s *sio) take(n int) []byte {
	if s.err != nil {
		return nil
	}
	if s.pos+n > len(s.buf) {
		s.err = errState
		return nil
	}
	b := s.buf[s.pos : s.pos+n]
	s.pos += n
	return b
}

func (s *sio) u64(v *uint64) {
	if s.save {
		s.buf = binary.LittleEndian.AppendUint64(s.buf, *v)
		return
	}
	if b := s.take(8); b != nil {
		*v = binary.LittleEndian.Uint64(b)
	}
}

func (s *sio) Int(v *int) {
	x := uint64(int64(*v))
	s.u64(&x)
	if !s.save {
		*v = int(int64(x))
	}
}

func (s *sio) I64(v *int64) {
	x := uint64(*v)
	s.u64(&x)
	if !s.save {
		*v = int64(x)
	}
}

func (s *sio) F64(v *float64) {
	x := math.Float64bits(*v)
	s.u64(&x)
	if !s.save {
		*v = math.Float64frombits(x)
	}
}

func (s *sio) Bool(v *bool) {
	if s.save {
		b := byte(0)
		if *v {
			b = 1
		}
		s.buf = append(s.buf, b)
		return
	}
	if b := s.take(1); b != nil {
		*v = b[0] != 0
	}
}

func (s *sio) U8(v *uint8) {
	if s.save {
		s.buf = append(s.buf, *v)
		return
	}
	if b := s.take(1); b != nil {
		*v = b[0]
	}
}

func (s *sio) Ints(v ...*int) {
	for _, p := range v {
		s.Int(p)
	}
}

func (s *sio) Bools(v ...*bool) {
	for _, p := range v {
		s.Bool(p)
	}
}

// Bytes saves a fixed-size byte slice (restored in place).
func (s *sio) Bytes(v []byte) {
	n := len(v)
	s.Int(&n)
	if s.save {
		s.buf = append(s.buf, v...)
		return
	}
	if n != len(v) {
		s.err = errState
		return
	}
	if b := s.take(n); b != nil {
		copy(v, b)
	}
}

// IntSlice saves a fixed-size []int.
func (s *sio) IntSlice(v []int) {
	n := len(v)
	s.Int(&n)
	if !s.save && n != len(v) {
		s.err = errState
		return
	}
	for i := range v {
		s.Int(&v[i])
	}
}

func (s *sio) U16s(v []uint16) {
	for i := range v {
		x := int(v[i])
		s.Int(&x)
		v[i] = uint16(x)
	}
}

func (s *sio) F32s(v []float32) {
	for i := range v {
		x := float64(v[i])
		s.F64(&x)
		v[i] = float32(x)
	}
}
