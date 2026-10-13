// Branch-converter filters for the 7z extractor: x86 BCJ, 4-stream x86 BCJ2,
// ARM64 and Delta.
//
// Executables compress better when the targets of relative jumps and calls
// are stored as absolute addresses, because calls to the same function then
// look identical. The encoder makes them absolute; these decoders turn them
// back into relative ones.

package sevenzip

import (
	"bufio"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
)

// ---------------- generic in-place filter ----------------

// converter decodes buf in place. It returns how many leading bytes are
// finished; the rest are kept and offered again with more data appended.
// When final is true there is no more data and it must finish everything.
type converter interface {
	convert(buf []byte, final bool) int
}

type filterReader struct {
	r         io.Reader
	conv      converter
	buf       []byte
	out, done int // buf[out:done] is ready to hand out
	end       int // buf[done:end] still waits for conversion
	eof       bool
	err       error
}

func newFilterReader(r io.Reader, c converter) *filterReader {
	return &filterReader{r: r, conv: c, buf: make([]byte, 64<<10)}
}

func (f *filterReader) Read(p []byte) (int, error) {
	for {
		if f.out < f.done {
			n := copy(p, f.buf[f.out:f.done])
			f.out += n
			return n, nil
		}
		if f.eof {
			if f.err != nil {
				return 0, f.err
			}
			return 0, io.EOF
		}
		// Move the unconverted tail to the front and read more after it.
		f.end = copy(f.buf, f.buf[f.done:f.end])
		f.out, f.done = 0, 0
		for f.end < len(f.buf) && !f.eof {
			n, err := f.r.Read(f.buf[f.end:])
			f.end += n
			if err != nil {
				f.eof = true
				if err != io.EOF {
					f.err = err
				}
			}
			if n > 0 && f.end >= 4096 {
				break
			}
		}
		f.done = f.conv.convert(f.buf[:f.end], f.eof)
		if f.eof {
			f.done = f.end
		}
	}
}

// ---------------- x86 BCJ (7z method 03030103) ----------------

// x86Converter undoes the x86 call/jump filter. The encoder looks at every
// E8 (CALL) and E9 (JMP) byte followed by a 32-bit operand whose top byte is
// 00 or FF (a near target) and replaces the relative operand with an absolute
// one. To avoid touching data that only looks like code, it skips an opcode
// when one of the three bytes before it was itself an opcode byte that was
// left alone, unless exactly one such byte exists and its own operand's top
// byte (which overlaps this operand) is not 00/FF.
type x86Converter struct {
	pos      uint32 // stream position of buf[0], plus the start offset
	lastSeen int64  // stream position of the last E8/E9 considered, relative to pos
	mask     uint32 // bit k: the byte k+1 before lastSeen+1 was a skipped opcode
}

func near(b byte) bool { return b == 0 || b == 0xFF }

func (c *x86Converter) convert(buf []byte, final bool) int {
	i := 0
	last := c.lastSeen
	mask := c.mask
	for ; i+4 < len(buf); i++ {
		if buf[i]&0xFE != 0xE8 {
			continue
		}
		gap := int64(i) - last
		if gap > 3 {
			mask = 0
		} else {
			mask = mask << uint(gap-1) & 7
		}
		last = int64(i)
		if mask != 0 {
			// highest marked byte: 1, 2 or 3 bytes back
			back := 3
			if mask < 4 {
				back = int(mask)
			}
			if mask&(mask-1) != 0 || near(buf[i+4-back]) {
				mask = mask<<1&7 | 1
				continue
			}
		}
		if !near(buf[i+4]) {
			mask = mask<<1&7 | 1
			continue
		}
		v := binary.LittleEndian.Uint32(buf[i+1:])
		ip := c.pos + uint32(i) + 5
		var dest uint32
		for {
			dest = v - ip
			if mask == 0 {
				break
			}
			// If the byte of the result that lines up with the earlier
			// marked opcode is itself near, the encoder flipped it; redo.
			shift := 0
			switch {
			case mask >= 4:
				shift = 24
			case mask >= 2:
				shift = 16
			default:
				shift = 8
			}
			b := byte(dest >> (24 - shift))
			if !near(b) {
				break
			}
			v = dest ^ (1<<(32-shift) - 1)
		}
		// Sign-extend bit 24 into the top byte.
		dest &= 0x01FFFFFF
		if dest&0x01000000 != 0 {
			dest |= 0xFF000000
		}
		binary.LittleEndian.PutUint32(buf[i+1:], dest)
		i += 4
	}
	if final {
		i = len(buf)
	}
	c.pos += uint32(i)
	c.lastSeen = last - int64(i)
	c.mask = mask
	return i
}

// ---------------- ARM64 (7z method 0A) ----------------

// arm64Converter undoes the ARM64 filter on 4-byte aligned instructions:
// BL has a 26-bit word offset, and ADRP a 21-bit page offset of which only
// values within +-512 MiB are converted.
type arm64Converter struct {
	pos uint32
}

func (c *arm64Converter) convert(buf []byte, final bool) int {
	i := 0
	for ; i+4 <= len(buf); i += 4 {
		ins := binary.LittleEndian.Uint32(buf[i:])
		pc := c.pos + uint32(i)
		switch {
		case ins>>26 == 0x25: // BL
			off := ins - pc>>2
			binary.LittleEndian.PutUint32(buf[i:], 0x94000000|off&0x03FFFFFF)
		case ins&0x9F000000 == 0x90000000: // ADRP
			page := ins>>29&3 | ins>>3&0x001FFFFC
			if (page+0x00020000)&0x001C0000 != 0 {
				continue
			}
			page -= pc >> 12
			ins &= 0x9000001F
			ins |= (page & 3) << 29
			ins |= (page & 0x0003FFFC) << 3
			ins |= -(page & 0x00020000) & 0x00E00000
			binary.LittleEndian.PutUint32(buf[i:], ins)
		}
	}
	if final {
		i = len(buf)
	}
	c.pos += uint32(i)
	return i
}

// ---------------- Delta (7z method 03) ----------------

// deltaConverter undoes byte-wise delta coding: each byte was stored as the
// difference from the byte dist positions earlier.
type deltaConverter struct {
	dist int
	hist [256]byte
	j    byte
}

func (c *deltaConverter) convert(buf []byte, final bool) int {
	d := byte(c.dist)
	for i := range buf {
		b := buf[i] + c.hist[c.j-d]
		c.hist[c.j] = b
		c.j++
		buf[i] = b
	}
	return len(buf)
}

// ---------------- BCJ2 (7z method 0303011B) ----------------

// bcj2Reader joins the four BCJ2 streams. The main stream holds the code with
// every converted CALL/JMP operand removed; the call and jump streams hold the
// absolute targets (big-endian) of converted E8 and E9/Jcc instructions; a
// range-coded stream says, for each candidate opcode, whether it was
// converted. Candidates are E8, E9, and 0F 80..8F (conditional near jumps).
type bcj2Reader struct {
	main, call, jump *bufio.Reader
	rc               *rangeDecoder
	probs            [258]uint16 // 0-255: E8 by previous byte, 256: E9, 257: Jcc
	prev             byte
	written          int64 // bytes produced so far
	size             int64
	pend             [4]byte
	npend, ipend     int
	err              error
}

func newBCJ2Reader(in []io.Reader, size int64) (io.Reader, error) {
	if len(in) != 4 {
		return nil, errors.New("BCJ2 needs four input streams")
	}
	z := &bcj2Reader{
		main: bufio.NewReaderSize(in[0], 64<<10),
		call: bufio.NewReaderSize(in[1], 16<<10),
		jump: bufio.NewReaderSize(in[2], 16<<10),
		rc:   newRangeDecoder(in[3]),
		size: size,
	}
	fill(z.probs[:])
	z.rc.rng = 0xFFFFFFFF
	for i := 0; i < 5; i++ {
		z.rc.code = z.rc.code<<8 | uint32(z.rc.readByte())
	}
	if z.rc.err != nil && size > 0 {
		return nil, fmt.Errorf("BCJ2 stream ends early: %w", z.rc.err)
	}
	return z, nil
}

// flag decodes one converted/not-converted decision. Unlike LZMA the range is
// normalised before decoding, so no byte is read after the last decision.
func (z *bcj2Reader) flag(p *uint16) bool {
	rc := z.rc
	if rc.rng < 1<<24 {
		rc.rng <<= 8
		rc.code = rc.code<<8 | uint32(rc.readByte())
	}
	v := uint32(*p)
	bound := (rc.rng >> 11) * v
	if rc.code < bound {
		rc.rng = bound
		*p = uint16(v + (2048-v)>>5)
		return false
	}
	rc.rng -= bound
	rc.code -= bound
	*p = uint16(v - v>>5)
	return true
}

func (z *bcj2Reader) Read(p []byte) (int, error) {
	n := 0
	for n < len(p) {
		if z.ipend < z.npend {
			k := copy(p[n:], z.pend[z.ipend:z.npend])
			z.ipend += k
			n += k
			continue
		}
		if z.err != nil || z.written >= z.size {
			break
		}
		b, err := z.main.ReadByte()
		if err != nil {
			if err == io.EOF {
				err = errors.New("BCJ2 main stream ends early")
			}
			z.err = err
			break
		}
		p[n] = b
		n++
		z.written++
		isJump := b&0xFE == 0xE8 || (z.prev == 0x0F && b&0xF0 == 0x80)
		if !isJump || z.written == z.size {
			z.prev = b
			continue
		}
		var prob *uint16
		switch b {
		case 0xE8:
			prob = &z.probs[z.prev]
		case 0xE9:
			prob = &z.probs[256]
		default:
			prob = &z.probs[257]
		}
		if !z.flag(prob) {
			z.prev = b
			continue
		}
		src := z.jump
		if b == 0xE8 {
			src = z.call
		}
		var a [4]byte
		if _, err := io.ReadFull(src, a[:]); err != nil {
			z.err = errors.New("BCJ2 address stream ends early")
			break
		}
		dest := binary.BigEndian.Uint32(a[:]) - uint32(z.written+4)
		binary.LittleEndian.PutUint32(z.pend[:], dest)
		k := int64(4)
		if z.size-z.written < k {
			k = z.size - z.written
		}
		z.npend, z.ipend = int(k), 0
		z.written += k
		z.prev = byte(dest >> 24)
	}
	if z.rc.err != nil && z.err == nil {
		z.err = fmt.Errorf("BCJ2 stream ends early: %w", z.rc.err)
	}
	if n > 0 {
		return n, nil
	}
	if z.err != nil {
		return 0, z.err
	}
	return 0, io.EOF
}
