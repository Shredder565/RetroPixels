package wiiu

// zlib125: the zlib library games link against, on Go's own flate code.
// A z_stream lives in the program's memory: next_in 0x00, avail_in 0x04,
// total_in 0x08, next_out 0x0c, avail_out 0x10, total_out 0x14, msg 0x18,
// state 0x1c, ..., adler 0x30.

import (
	"bytes"
	"compress/flate"
	"compress/gzip"
	"compress/zlib"
	"hash/adler32"
	"hash/crc32"
	"io"
)

const (
	zOK        = 0
	zStreamEnd = 1
	zStreamErr = 0xfffffffe // -2
	zDataErr   = 0xfffffffd // -3
	zBufErr    = 0xfffffffb // -5
	zFinish    = 4
)

// zInflate is one inflate stream's state: all the input so far, and the
// output it gives (worked out again when more input arrives).
type zInflate struct {
	window int32
	in     []byte
	out    []byte
	given  int
	ended  bool
	failed bool
}

func (z *zInflate) run() {
	var r io.Reader
	src := bytes.NewReader(z.in)
	var err error
	switch w := z.window; {
	case w < 0:
		r = flate.NewReader(src)
	case w > 31 || w > 15 && len(z.in) >= 2 && z.in[0] == 0x1f && z.in[1] == 0x8b:
		var g *gzip.Reader
		if g, err = gzip.NewReader(src); err == nil {
			r = g
		}
	case w > 15 && w < 32 && w&16 != 0:
		var g *gzip.Reader
		if g, err = gzip.NewReader(src); err == nil {
			r = g
		}
	default:
		r, err = zlib.NewReader(src)
	}
	if err != nil {
		if err != io.ErrUnexpectedEOF && err != io.EOF {
			z.failed = true
		}
		return
	}
	out, err := io.ReadAll(r)
	z.out = out
	switch err {
	case nil:
		z.ended = true
	case io.ErrUnexpectedEOF:
	default:
		z.failed = true
	}
}

func init() {
	l := "zlib125"
	reg(l, "zlibVersion", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, m.cstrAlloc("1.2.5")) })
	initInflate := func(m *Machine, s uint32, window int32) {
		if m.zstreams == nil {
			m.zstreams = map[uint32]*zInflate{}
		}
		m.zstreams[s] = &zInflate{window: window}
		m.Mem.Write32(s+0x08, 0)
		m.Mem.Write32(s+0x14, 0)
		m.Mem.Write32(s+0x18, 0)
		m.Mem.Write32(s+0x1c, s) // (any non-zero state)
	}
	reg(l, "inflateInit_", func(m *Machine, cpu *CPU, t *Thread) { initInflate(m, arg(cpu, 0), 15); ret(cpu, zOK) })
	reg(l, "inflateInit2_", func(m *Machine, cpu *CPU, t *Thread) {
		initInflate(m, arg(cpu, 0), int32(arg(cpu, 1)))
		ret(cpu, zOK)
	})
	reg(l, "inflateReset", func(m *Machine, cpu *CPU, t *Thread) {
		s := arg(cpu, 0)
		if z := m.zstreams[s]; z != nil {
			initInflate(m, s, z.window)
			ret(cpu, zOK)
			return
		}
		ret(cpu, zStreamErr)
	})
	reg(l, "inflateEnd", func(m *Machine, cpu *CPU, t *Thread) {
		delete(m.zstreams, arg(cpu, 0))
		m.Mem.Write32(arg(cpu, 0)+0x1c, 0)
		ret(cpu, zOK)
	})
	reg(l, "inflate", func(m *Machine, cpu *CPU, t *Thread) {
		s := arg(cpu, 0)
		z := m.zstreams[s]
		if z == nil {
			ret(cpu, zStreamErr)
			return
		}
		mem := m.Mem
		nextIn, availIn := mem.Read32(s), mem.Read32(s+4)
		if availIn > 0 {
			b := make([]byte, availIn)
			mem.ReadBytes(nextIn, b)
			z.in = append(z.in, b...)
			mem.Write32(s, nextIn+availIn)
			mem.Write32(s+4, 0)
			mem.Write32(s+8, mem.Read32(s+8)+availIn)
			if !z.ended {
				z.run()
			}
		}
		if z.failed {
			ret(cpu, zDataErr)
			return
		}
		nextOut, availOut := mem.Read32(s+0xc), mem.Read32(s+0x10)
		n := min(int(availOut), len(z.out)-z.given)
		if n > 0 {
			mem.WriteBytes(nextOut, z.out[z.given:z.given+n])
			z.given += n
			mem.Write32(s+0xc, nextOut+uint32(n))
			mem.Write32(s+0x10, availOut-uint32(n))
			mem.Write32(s+0x14, mem.Read32(s+0x14)+uint32(n))
		}
		switch {
		case z.ended && z.given == len(z.out):
			ret(cpu, zStreamEnd)
		case n == 0 && availIn == 0:
			ret(cpu, zBufErr)
		default:
			ret(cpu, zOK)
		}
	})
	// uncompress(dest, &destLen, source, sourceLen)
	reg(l, "uncompress", func(m *Machine, cpu *CPU, t *Thread) {
		dst, lenp, src, n := arg(cpu, 0), arg(cpu, 1), arg(cpu, 2), arg(cpu, 3)
		b := make([]byte, n)
		m.Mem.ReadBytes(src, b)
		r, err := zlib.NewReader(bytes.NewReader(b))
		if err != nil {
			ret(cpu, zDataErr)
			return
		}
		out, err := io.ReadAll(r)
		if err != nil {
			ret(cpu, zDataErr)
			return
		}
		if uint32(len(out)) > m.Mem.Read32(lenp) {
			ret(cpu, zBufErr)
			return
		}
		m.Mem.WriteBytes(dst, out)
		m.Mem.Write32(lenp, uint32(len(out)))
		ret(cpu, zOK)
	})
	// compress / compress2(dest, &destLen, source, sourceLen[, level])
	compress := func(m *Machine, cpu *CPU, level int) {
		dst, lenp, src, n := arg(cpu, 0), arg(cpu, 1), arg(cpu, 2), arg(cpu, 3)
		b := make([]byte, n)
		m.Mem.ReadBytes(src, b)
		var buf bytes.Buffer
		w, err := zlib.NewWriterLevel(&buf, level)
		if err != nil {
			w = zlib.NewWriter(&buf)
		}
		w.Write(b)
		w.Close()
		if uint32(buf.Len()) > m.Mem.Read32(lenp) {
			ret(cpu, zBufErr)
			return
		}
		m.Mem.WriteBytes(dst, buf.Bytes())
		m.Mem.Write32(lenp, uint32(buf.Len()))
		ret(cpu, zOK)
	}
	reg(l, "compress", func(m *Machine, cpu *CPU, t *Thread) { compress(m, cpu, -1) })
	reg(l, "compress2", func(m *Machine, cpu *CPU, t *Thread) { compress(m, cpu, int(int32(arg(cpu, 4)))) })
	reg(l, "compressBound", func(m *Machine, cpu *CPU, t *Thread) {
		n := arg(cpu, 0)
		ret(cpu, n+n>>12+n>>14+n>>25+13)
	})
	// checksums: (running value, data, length)
	reg(l, "crc32", func(m *Machine, cpu *CPU, t *Thread) {
		c, p, n := arg(cpu, 0), arg(cpu, 1), arg(cpu, 2)
		if p == 0 {
			ret(cpu, 0)
			return
		}
		b := make([]byte, n)
		m.Mem.ReadBytes(p, b)
		ret(cpu, crc32.Update(c, crc32.IEEETable, b))
	})
	reg(l, "adler32", func(m *Machine, cpu *CPU, t *Thread) {
		a, p, n := arg(cpu, 0), arg(cpu, 1), arg(cpu, 2)
		if p == 0 {
			ret(cpu, 1)
			return
		}
		b := make([]byte, n)
		m.Mem.ReadBytes(p, b)
		ret(cpu, adler32Update(a, b))
	})
}

// adler32Update continues an Adler-32 sum.
func adler32Update(a uint32, b []byte) uint32 {
	if a == 1 {
		return adler32.Checksum(b)
	}
	s1, s2 := a&0xffff, a>>16
	for _, c := range b {
		s1 = (s1 + uint32(c)) % 65521
		s2 = (s2 + s1) % 65521
	}
	return s2<<16 | s1
}
