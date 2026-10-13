// LZMA and LZMA2 decoders for the 7z extractor.
//
// Written from the published LZMA format description: an adaptive binary
// range coder drives a model of literals, matches and repeated matches that
// copy bytes out of a sliding dictionary window. LZMA2 wraps LZMA in small
// chunks that can be stored, reset the model, or reset the dictionary.

package sevenzip

import (
	"errors"
	"fmt"
	"io"
)

var errCorrupt = errors.New("the compressed data is damaged")

// ---------------- range decoder ----------------

// rangeDecoder reads the arithmetic-coded bit stream. It keeps its own input
// buffer so the hot path never goes through an interface call. When the input
// runs out it feeds zeros and remembers the error; callers check err after a
// batch of work.
type rangeDecoder struct {
	rng, code uint32
	r         io.Reader
	buf       []byte
	pos, end  int
	consumed  int64 // bytes of input used before buf[0]
	err       error
}

func newRangeDecoder(r io.Reader) *rangeDecoder {
	return &rangeDecoder{r: r, buf: make([]byte, 64<<10)}
}

func (rc *rangeDecoder) readByte() byte {
	if rc.pos < rc.end {
		b := rc.buf[rc.pos]
		rc.pos++
		return b
	}
	return rc.fill()
}

// fill refills the input buffer and returns its first byte.
func (rc *rangeDecoder) fill() byte {
	if rc.err != nil {
		return 0
	}
	rc.consumed += int64(rc.end)
	rc.pos, rc.end = 0, 0
	for {
		n, err := rc.r.Read(rc.buf)
		if n > 0 {
			rc.end = n
			rc.pos = 1
			return rc.buf[0]
		}
		if err != nil {
			if err == io.EOF {
				err = io.ErrUnexpectedEOF
			}
			rc.err = err
			return 0
		}
	}
}

// offset is how many input bytes have been consumed so far.
func (rc *rangeDecoder) offset() int64 { return rc.consumed + int64(rc.pos) }

// init starts a new range-coded block: a zero byte then the 32-bit code.
func (rc *rangeDecoder) init() error {
	first := rc.readByte()
	rc.rng = 0xFFFFFFFF
	rc.code = 0
	for i := 0; i < 4; i++ {
		rc.code = rc.code<<8 | uint32(rc.readByte())
	}
	if rc.err != nil {
		return rc.err
	}
	if first != 0 || rc.code == rc.rng {
		return errCorrupt
	}
	return nil
}

// bit decodes one bit with the adaptive probability *p (11-bit, shift 5).
// It is written without data-dependent branches, which are unpredictable
// here, so it stays small enough to inline.
func (rc *rangeDecoder) bit(p *uint16) uint32 {
	v := uint32(*p)
	bound := (rc.rng >> 11) * v
	b := b2u(rc.code >= bound)
	m := -b // all ones for a 1 bit
	rc.code -= bound & m
	rc.rng = bound ^ ((rc.rng-bound)^bound)&m
	up, down := v+(2048-v)>>5, v-v>>5
	*p = uint16(up ^ (up^down)&m)
	if rc.rng < 1<<24 {
		rc.normalize()
	}
	return b
}

func b2u(c bool) uint32 {
	if c {
		return 1
	}
	return 0
}

func (rc *rangeDecoder) normalize() {
	rc.rng <<= 8
	rc.code = rc.code<<8 | uint32(rc.readByte())
}

// direct decodes n bits with fixed probability one half.
func (rc *rangeDecoder) direct(n uint) uint32 {
	rng, code := rc.rng, rc.code
	var res uint32
	for ; n > 0; n-- {
		rng >>= 1
		// code < 2*rng here, so the top bit of code-rng says which half.
		code -= rng
		t := -(code >> 31) // all ones if code was below rng
		code += rng & t
		res = res<<1 + t + 1
		if rng < 1<<24 {
			rng <<= 8
			code = code<<8 | uint32(rc.readByte())
		}
	}
	rc.rng, rc.code = rng, code
	return res
}

// tree decodes an n-bit number most significant bit first, using probs[1:1<<n].
func (rc *rangeDecoder) tree(probs []uint16, n uint) uint32 {
	return rc.walk(probs, 1, 1<<n) - 1<<n
}

// walk continues a bit-tree decode from node m until it reaches end or
// beyond. This is the hottest loop, so the steps of bit are written out here
// with the coder state held in locals.
func (rc *rangeDecoder) walk(probs []uint16, m, end uint32) uint32 {
	rng, code := rc.rng, rc.code
	for m < end {
		v := uint32(probs[m])
		bound := (rng >> 11) * v
		b := b2u(code >= bound)
		mk := -b
		code -= bound & mk
		rng = bound ^ ((rng-bound)^bound)&mk
		up, down := v+(2048-v)>>5, v-v>>5
		probs[m] = uint16(up ^ (up^down)&mk)
		if rng < 1<<24 {
			rng <<= 8
			code = code<<8 | uint32(rc.readByte())
		}
		m = m<<1 | b
	}
	rc.rng, rc.code = rng, code
	return m
}

// reverseTree decodes an n-bit number least significant bit first.
func (rc *rangeDecoder) reverseTree(probs []uint16, n uint) uint32 {
	m := uint32(1)
	var res uint32
	for i := uint(0); i < n; i++ {
		b := rc.bit(&probs[m])
		m = m<<1 | b
		res |= b << i
	}
	return res
}

// ---------------- dictionary window ----------------

// window is the circular dictionary that is also the output buffer. Bytes are
// decoded into buf[pos], and the reader hands them out before pos wraps.
type window struct {
	buf   []byte
	pos   int
	have  int    // bytes usable for back-references (at most len(buf))
	total uint64 // bytes produced since the last dictionary reset
}

// maxWindow caps the dictionary memory; 7-Zip's strongest preset uses 64 MiB.
const maxWindow = 1 << 30

func newWindow(dictSize uint32, outSize int64) (*window, error) {
	n := int64(dictSize)
	if outSize >= 0 && outSize < n {
		n = outSize
	}
	if n < 4096 {
		n = 4096
	}
	if n > maxWindow {
		return nil, fmt.Errorf("this .7z needs a %d MB dictionary, more than RetroPixels allows", n>>20)
	}
	return &window{buf: make([]byte, n)}, nil
}

func (w *window) reset() {
	w.have = 0
	w.total = 0
}

// back returns the byte dist+1 positions behind the write position.
func (w *window) back(dist uint32) byte {
	i := w.pos - int(dist) - 1
	if i < 0 {
		i += len(w.buf)
	}
	return w.buf[i]
}

func (w *window) put(b byte) {
	w.buf[w.pos] = b
	w.pos++
	w.total++
	if w.have < len(w.buf) {
		w.have++
	}
}

// copyMatch repeats n bytes starting dist+1 bytes back. The caller makes sure
// pos+n does not pass the end of buf.
func (w *window) copyMatch(dist uint32, n int) {
	src := w.pos - int(dist) - 1
	if src < 0 {
		src += len(w.buf)
	}
	w.total += uint64(n)
	if w.have += n; w.have > len(w.buf) {
		w.have = len(w.buf)
	}
	buf := w.buf
	if int(dist) < 8 && src < w.pos {
		// Short overlapping run: plain byte loop.
		for i := 0; i < n; i++ {
			buf[w.pos+i] = buf[src+i]
		}
		w.pos += n
		return
	}
	for n > 0 {
		k := n
		if src+k > len(buf) {
			k = len(buf) - src
		}
		if int(dist)+1 < k {
			k = int(dist) + 1
		}
		copy(buf[w.pos:w.pos+k], buf[src:src+k])
		w.pos += k
		src += k
		if src == len(buf) {
			src = 0
		}
		n -= k
	}
}

// ---------------- LZMA model ----------------

const (
	numStates        = 12
	endPosModelIndex = 14
	numFullDistances = 128
	alignBits        = 4
)

type lenDecoder struct {
	choice, choice2 uint16
	low             [16][8]uint16
	mid             [16][8]uint16
	high            [256]uint16
}

func (l *lenDecoder) reset() {
	l.choice, l.choice2 = 1024, 1024
	fill(l.high[:])
	for i := range l.low {
		fill(l.low[i][:])
		fill(l.mid[i][:])
	}
}

// decode returns a match length (2 to 273).
func (l *lenDecoder) decode(rc *rangeDecoder, posState uint32) int {
	if rc.bit(&l.choice) == 0 {
		return 2 + int(rc.tree(l.low[posState][:], 3))
	}
	if rc.bit(&l.choice2) == 0 {
		return 10 + int(rc.tree(l.mid[posState][:], 3))
	}
	return 18 + int(rc.tree(l.high[:], 8))
}

func fill(p []uint16) {
	for i := range p {
		p[i] = 1024
	}
}

type lzmaDecoder struct {
	lc, lp, pb uint
	literal    []uint16
	isMatch    [numStates << 4]uint16
	isRep      [numStates]uint16
	isRepG0    [numStates]uint16
	isRepG1    [numStates]uint16
	isRepG2    [numStates]uint16
	isRep0Long [numStates << 4]uint16
	posSlot    [4][64]uint16
	posSpec    [1 + numFullDistances - endPosModelIndex]uint16
	align      [1 << alignBits]uint16
	matchLen   lenDecoder
	repLen     lenDecoder

	state  uint32
	rep    [4]uint32 // recent distances, zero-based (0 means "previous byte")
	remLen int       // bytes of the current match not yet copied
	eos    bool      // end-of-stream marker seen
}

// setProps sets lc/lp/pb from the packed properties byte.
func (d *lzmaDecoder) setProps(b byte) error {
	if b >= 9*5*5 {
		return errors.New("bad LZMA properties")
	}
	d.lc = uint(b % 9)
	b /= 9
	d.lp = uint(b % 5)
	d.pb = uint(b / 5)
	n := 0x300 << (d.lc + d.lp)
	if cap(d.literal) >= n {
		d.literal = d.literal[:n]
	} else {
		d.literal = make([]uint16, n)
	}
	return nil
}

// resetState puts every probability back to one half and clears the history.
func (d *lzmaDecoder) resetState() {
	fill(d.literal)
	fill(d.isMatch[:])
	fill(d.isRep[:])
	fill(d.isRepG0[:])
	fill(d.isRepG1[:])
	fill(d.isRepG2[:])
	fill(d.isRep0Long[:])
	for i := range d.posSlot {
		fill(d.posSlot[i][:])
	}
	fill(d.posSpec[:])
	fill(d.align[:])
	d.matchLen.reset()
	d.repLen.reset()
	d.state = 0
	d.rep = [4]uint32{}
	d.remLen = 0
	d.eos = false
}

// distance decodes the distance of a new match of the given length.
func (d *lzmaDecoder) distance(rc *rangeDecoder, length int) uint32 {
	ls := length - 2
	if ls > 3 {
		ls = 3
	}
	slot := rc.tree(d.posSlot[ls][:], 6)
	if slot < 4 {
		return slot
	}
	nbits := uint(slot>>1) - 1
	dist := (2 | slot&1) << nbits
	if slot < endPosModelIndex {
		// The reverse tree for this slot starts at dist-slot in posSpec,
		// with the tree's index 1 as its first used entry.
		return dist + rc.reverseTree(d.posSpec[dist-slot:], nbits)
	}
	dist += rc.direct(nbits-alignBits) << alignBits
	return dist + rc.reverseTree(d.align[:], alignBits)
}

// decode runs the model until the window reaches limit, the end marker is
// seen, or the data is found to be bad.
func (d *lzmaDecoder) decode(rc *rangeDecoder, w *window, limit int) error {
	if d.remLen > 0 {
		n := limit - w.pos
		if n > d.remLen {
			n = d.remLen
		}
		w.copyMatch(d.rep[0], n)
		d.remLen -= n
	}
	pbMask := uint64(1)<<d.pb - 1
	lpMask := uint64(1)<<d.lp - 1
	lc := d.lc
	for w.pos < limit {
		posState := uint32(w.total & pbMask)
		st := d.state
		if rc.bit(&d.isMatch[st<<4|posState]) == 0 {
			// Literal byte.
			var prev uint32
			if w.have > 0 {
				prev = uint32(w.back(0))
			}
			base := 0x300 * int((w.total&lpMask)<<lc+uint64(prev>>(8-lc)))
			probs := d.literal[base : base+0x300]
			sym := uint32(1)
			if st >= 7 {
				// After a match the byte at rep0 guides the first bits.
				match := uint32(w.back(d.rep[0]))
				for sym < 0x100 {
					mb := match >> 7 & 1
					match <<= 1
					b := rc.bit(&probs[(1+mb)<<8+sym])
					sym = sym<<1 | b
					if b != mb {
						break
					}
				}
			}
			sym = rc.walk(probs, sym, 0x100)
			w.put(byte(sym))
			switch {
			case st < 4:
				d.state = 0
			case st < 10:
				d.state = st - 3
			default:
				d.state = st - 6
			}
			continue
		}

		var length int
		if rc.bit(&d.isRep[st]) == 0 {
			// New match.
			length = d.matchLen.decode(rc, posState)
			if st < 7 {
				d.state = 7
			} else {
				d.state = 10
			}
			dist := d.distance(rc, length)
			if dist == 0xFFFFFFFF {
				d.eos = true
				return nil
			}
			d.rep[3], d.rep[2], d.rep[1], d.rep[0] = d.rep[2], d.rep[1], d.rep[0], dist
		} else {
			if w.have == 0 {
				return errCorrupt
			}
			if rc.bit(&d.isRepG0[st]) == 0 {
				if rc.bit(&d.isRep0Long[st<<4|posState]) == 0 {
					// Single byte from rep0.
					if st < 7 {
						d.state = 9
					} else {
						d.state = 11
					}
					w.put(w.back(d.rep[0]))
					continue
				}
			} else {
				var dist uint32
				if rc.bit(&d.isRepG1[st]) == 0 {
					dist = d.rep[1]
				} else {
					if rc.bit(&d.isRepG2[st]) == 0 {
						dist = d.rep[2]
					} else {
						dist = d.rep[3]
						d.rep[3] = d.rep[2]
					}
					d.rep[2] = d.rep[1]
				}
				d.rep[1] = d.rep[0]
				d.rep[0] = dist
			}
			length = d.repLen.decode(rc, posState)
			if st < 7 {
				d.state = 8
			} else {
				d.state = 11
			}
		}
		if int64(d.rep[0]) >= int64(w.have) {
			return errCorrupt
		}
		n := limit - w.pos
		if n > length {
			n = length
		}
		w.copyMatch(d.rep[0], n)
		d.remLen = length - n
	}
	return nil
}

// ---------------- LZMA stream (7z method 030101) ----------------

type lzmaReader struct {
	rc        *rangeDecoder
	d         lzmaDecoder
	w         *window
	out       int   // next byte of w.buf to hand out
	remaining int64 // bytes still to decode
	err       error
}

// chunkLimit caps how much is decoded between reads so output flows steadily.
const chunkLimit = 1 << 20

func newLZMAReader(r io.Reader, props []byte, size int64) (io.Reader, error) {
	if len(props) < 5 {
		return nil, errors.New("bad LZMA properties")
	}
	dict := uint32(props[1]) | uint32(props[2])<<8 | uint32(props[3])<<16 | uint32(props[4])<<24
	w, err := newWindow(dict, size)
	if err != nil {
		return nil, err
	}
	z := &lzmaReader{rc: newRangeDecoder(r), w: w, remaining: size}
	if err := z.d.setProps(props[0]); err != nil {
		return nil, err
	}
	z.d.resetState()
	if size > 0 {
		if err := z.rc.init(); err != nil {
			return nil, err
		}
	}
	return z, nil
}

func (z *lzmaReader) Read(p []byte) (int, error) {
	for {
		if z.out < z.w.pos {
			n := copy(p, z.w.buf[z.out:z.w.pos])
			z.out += n
			return n, nil
		}
		if z.err != nil {
			return 0, z.err
		}
		if z.remaining <= 0 {
			return 0, io.EOF
		}
		if z.w.pos == len(z.w.buf) {
			z.w.pos, z.out = 0, 0
		}
		start := z.w.pos
		limit := len(z.w.buf)
		if limit-start > chunkLimit {
			limit = start + chunkLimit
		}
		if int64(limit-start) > z.remaining {
			limit = start + int(z.remaining)
		}
		err := z.d.decode(z.rc, z.w, limit)
		z.remaining -= int64(z.w.pos - start)
		switch {
		case err != nil:
			z.err = err
		case z.rc.err != nil && z.remaining > 0:
			z.err = fmt.Errorf("compressed data ends early: %w", z.rc.err)
		case z.d.eos && z.remaining > 0:
			z.err = errors.New("compressed data ends early")
		}
	}
}

// ---------------- LZMA2 stream (7z method 21) ----------------

type lzma2Reader struct {
	rc        *rangeDecoder
	d         lzmaDecoder
	w         *window
	out       int
	chunkLeft int   // unpacked bytes left in the current chunk
	chunkEnd  int64 // input offset where the current LZMA chunk ends
	stored    bool  // current chunk is uncompressed
	needDict  bool
	needProps bool
	inLZMA    bool // a compressed chunk is in progress
	ended     bool
	err       error
}

func newLZMA2Reader(r io.Reader, props []byte, size int64) (io.Reader, error) {
	if len(props) < 1 || props[0] > 40 {
		return nil, errors.New("bad LZMA2 properties")
	}
	dict := uint32(0xFFFFFFFF)
	if props[0] < 40 {
		dict = (2 | uint32(props[0])&1) << (props[0]/2 + 11)
	}
	w, err := newWindow(dict, size)
	if err != nil {
		return nil, err
	}
	return &lzma2Reader{rc: newRangeDecoder(r), w: w, needDict: true, needProps: true}, nil
}

func (z *lzma2Reader) Read(p []byte) (int, error) {
	for {
		if z.out < z.w.pos {
			n := copy(p, z.w.buf[z.out:z.w.pos])
			z.out += n
			return n, nil
		}
		if z.err != nil {
			return 0, z.err
		}
		if z.ended {
			return 0, io.EOF
		}
		if z.chunkLeft == 0 {
			if z.err = z.nextChunk(); z.err != nil {
				continue
			}
			if z.ended {
				continue
			}
		}
		if z.w.pos == len(z.w.buf) {
			z.w.pos, z.out = 0, 0
		}
		start := z.w.pos
		limit := len(z.w.buf)
		if limit-start > chunkLimit {
			limit = start + chunkLimit
		}
		if limit-start > z.chunkLeft {
			limit = start + z.chunkLeft
		}
		if z.stored {
			for z.w.pos < limit && z.rc.err == nil {
				if z.rc.pos == z.rc.end {
					z.rc.fill()
					if z.rc.err == nil {
						z.rc.pos-- // put back the byte fill returned
					}
					continue
				}
				n := copy(z.w.buf[z.w.pos:limit], z.rc.buf[z.rc.pos:z.rc.end])
				z.rc.pos += n
				z.w.pos += n
				z.w.total += uint64(n)
				if z.w.have += n; z.w.have > len(z.w.buf) {
					z.w.have = len(z.w.buf)
				}
			}
		} else {
			if err := z.d.decode(z.rc, z.w, limit); err != nil {
				z.err = err
			} else if z.d.eos {
				z.err = errCorrupt
			}
		}
		z.chunkLeft -= z.w.pos - start
		if z.rc.err != nil {
			z.err = fmt.Errorf("compressed data ends early: %w", z.rc.err)
		}
	}
}

// nextChunk reads the next LZMA2 chunk header.
func (z *lzma2Reader) nextChunk() error {
	if z.inLZMA {
		// A compressed chunk must use exactly its stated packed size.
		if z.rc.offset() != z.chunkEnd || z.d.remLen != 0 {
			return errCorrupt
		}
		z.inLZMA = false
	}
	ctrl := z.rc.readByte()
	if z.rc.err != nil {
		return fmt.Errorf("compressed data ends early: %w", z.rc.err)
	}
	switch {
	case ctrl == 0:
		z.ended = true
		return nil
	case ctrl == 1 || ctrl == 2:
		size := int(z.rc.readByte())<<8 | int(z.rc.readByte())
		if ctrl == 1 {
			z.w.reset()
			z.needDict = false
		} else if z.needDict {
			return errCorrupt
		}
		z.stored = true
		z.chunkLeft = size + 1
	case ctrl >= 0x80:
		size := int(ctrl&0x1F)<<16 | int(z.rc.readByte())<<8 | int(z.rc.readByte())
		packed := int(z.rc.readByte())<<8 | int(z.rc.readByte())
		mode := ctrl >> 5 & 3
		if mode == 3 {
			z.w.reset()
			z.needDict = false
		} else if z.needDict {
			return errCorrupt
		}
		if mode >= 2 {
			pb := z.rc.readByte()
			if err := z.d.setProps(pb); err != nil || z.d.lc+z.d.lp > 4 {
				return errors.New("bad LZMA2 properties")
			}
			z.needProps = false
		} else if z.needProps {
			return errCorrupt
		}
		if mode >= 1 {
			z.d.resetState()
		}
		z.stored = false
		z.chunkLeft = size + 1
		if err := z.rc.init(); err != nil {
			return err
		}
		// init consumed 5 of the packed bytes.
		z.chunkEnd = z.rc.offset() - 5 + int64(packed) + 1
		z.inLZMA = true
	default:
		return errCorrupt
	}
	if z.rc.err != nil {
		return fmt.Errorf("compressed data ends early: %w", z.rc.err)
	}
	return nil
}
