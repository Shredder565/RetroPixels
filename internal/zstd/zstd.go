// Package zstd decompresses Zstandard data (RFC 8878), written from the
// RFC for RetroPixels. It decodes whole frames held in memory, without
// dictionaries, which is what Wii U .wua archives need.
package zstd

import (
	"encoding/binary"
	"errors"
	"math/bits"
)

var errCorrupt = errors.New("zstd: corrupt data")

const (
	frameMagic = 0xFD2FB528
	maxBlock   = 128 << 10
)

// Decode decompresses every frame in src (skippable frames are skipped).
func Decode(src []byte) ([]byte, error) { return DecodeTo(nil, src) }

// DecodeTo appends the decompressed data to dst.
func DecodeTo(dst, src []byte) ([]byte, error) {
	for len(src) > 0 {
		if len(src) < 4 {
			return nil, errCorrupt
		}
		magic := binary.LittleEndian.Uint32(src)
		if magic&0xFFFFFFF0 == 0x184D2A50 {
			if len(src) < 8 {
				return nil, errCorrupt
			}
			n := int(binary.LittleEndian.Uint32(src[4:]))
			if 8+n > len(src) || n < 0 {
				return nil, errCorrupt
			}
			src = src[8+n:]
			continue
		}
		if magic != frameMagic {
			return nil, errors.New("zstd: not Zstandard data")
		}
		var err error
		var n int
		dst, n, err = decodeFrame(dst, src[4:])
		if err != nil {
			return nil, err
		}
		src = src[4+n:]
	}
	return dst, nil
}

// frame state that carries over from block to block
type decoder struct {
	out        []byte
	start      int // where this frame's output starts
	huf        *hufTable
	ll, of, ml *fseTable
	rep        [3]int
	lits       []byte
}

func decodeFrame(dst, src []byte) ([]byte, int, error) {
	if len(src) < 1 {
		return nil, 0, errCorrupt
	}
	fhd := src[0]
	p := 1
	fcsFlag := fhd >> 6
	single := fhd>>5&1 != 0
	checksum := fhd>>2&1 != 0
	if fhd>>3&1 != 0 {
		return nil, 0, errCorrupt // reserved bit
	}
	if !single {
		p++ // window descriptor (the whole output stays in memory)
	}
	p += [4]int{0, 1, 2, 4}[fhd&3]
	if fhd&3 != 0 {
		// a dictionary ID: only "no dictionary" (zero) is accepted
		id := uint32(0)
		for i := [4]int{0, 1, 2, 4}[fhd&3]; i > 0; i-- {
			if p-i >= len(src) {
				return nil, 0, errCorrupt
			}
			id = id<<8 | uint32(src[p-i])
		}
		if id != 0 {
			return nil, 0, errors.New("zstd: data needs a dictionary")
		}
	}
	fcsLen := [4]int{0, 2, 4, 8}[fcsFlag]
	if fcsFlag == 0 && single {
		fcsLen = 1
	}
	if p+fcsLen > len(src) {
		return nil, 0, errCorrupt
	}
	var fcs uint64
	switch fcsLen {
	case 1:
		fcs = uint64(src[p])
	case 2:
		fcs = uint64(binary.LittleEndian.Uint16(src[p:])) + 256
	case 4:
		fcs = uint64(binary.LittleEndian.Uint32(src[p:]))
	case 8:
		fcs = binary.LittleEndian.Uint64(src[p:])
	}
	p += fcsLen
	d := &decoder{out: dst, start: len(dst), rep: [3]int{1, 4, 8}}
	if fcsLen > 0 && fcs < 1<<30 {
		d.out = growTo(d.out, int(fcs))
	}
	for {
		if p+3 > len(src) {
			return nil, 0, errCorrupt
		}
		h := uint32(src[p]) | uint32(src[p+1])<<8 | uint32(src[p+2])<<16
		p += 3
		last := h&1 != 0
		size := int(h >> 3)
		switch h >> 1 & 3 {
		case 0: // raw
			if p+size > len(src) {
				return nil, 0, errCorrupt
			}
			d.out = append(d.out, src[p:p+size]...)
			p += size
		case 1: // one byte repeated
			if p >= len(src) || size > maxBlock {
				return nil, 0, errCorrupt
			}
			b := src[p]
			for i := 0; i < size; i++ {
				d.out = append(d.out, b)
			}
			p++
		case 2:
			if p+size > len(src) || size > maxBlock {
				return nil, 0, errCorrupt
			}
			if err := d.block(src[p : p+size]); err != nil {
				return nil, 0, err
			}
			p += size
		default:
			return nil, 0, errCorrupt
		}
		if last {
			break
		}
	}
	if checksum {
		p += 4 // (not checked)
		if p > len(src) {
			return nil, 0, errCorrupt
		}
	}
	if fcsLen > 0 && uint64(len(d.out)-d.start) != fcs {
		return nil, 0, errCorrupt
	}
	return d.out, p, nil
}

func growTo(b []byte, n int) []byte {
	if cap(b)-len(b) >= n {
		return b
	}
	nb := make([]byte, len(b), len(b)+n)
	copy(nb, b)
	return nb
}

// ---- compressed blocks ----

func (d *decoder) block(b []byte) error {
	n, err := d.literals(b)
	if err != nil {
		return err
	}
	return d.sequences(b[n:])
}

// literals decodes the literals section into d.lits and gives its size.
func (d *decoder) literals(b []byte) (int, error) {
	if len(b) < 1 {
		return 0, errCorrupt
	}
	typ := b[0] & 3
	sf := b[0] >> 2 & 3
	if typ < 2 {
		var size, hl int
		switch sf {
		case 0, 2:
			size, hl = int(b[0]>>3), 1
		case 1:
			if len(b) < 2 {
				return 0, errCorrupt
			}
			size, hl = int(b[0]>>4)|int(b[1])<<4, 2
		case 3:
			if len(b) < 3 {
				return 0, errCorrupt
			}
			size, hl = int(b[0]>>4)|int(b[1])<<4|int(b[2])<<12, 3
		}
		if size > maxBlock {
			return 0, errCorrupt
		}
		if typ == 0 {
			if hl+size > len(b) {
				return 0, errCorrupt
			}
			d.lits = append(d.lits[:0], b[hl:hl+size]...)
			return hl + size, nil
		}
		if hl >= len(b) {
			return 0, errCorrupt
		}
		d.lits = d.lits[:0]
		for i := 0; i < size; i++ {
			d.lits = append(d.lits, b[hl])
		}
		return hl + 1, nil
	}
	var regen, comp, hl int
	streams := 4
	switch sf {
	case 0, 1:
		if len(b) < 3 {
			return 0, errCorrupt
		}
		v := uint32(b[0]) | uint32(b[1])<<8 | uint32(b[2])<<16
		regen, comp, hl = int(v>>4&0x3ff), int(v>>14&0x3ff), 3
		if sf == 0 {
			streams = 1
		}
	case 2:
		if len(b) < 4 {
			return 0, errCorrupt
		}
		v := binary.LittleEndian.Uint32(b)
		regen, comp, hl = int(v>>4&0x3fff), int(v>>18&0x3fff), 4
	case 3:
		if len(b) < 5 {
			return 0, errCorrupt
		}
		v := uint64(binary.LittleEndian.Uint32(b)) | uint64(b[4])<<32
		regen, comp, hl = int(v>>4&0x3ffff), int(v>>22&0x3ffff), 5
	}
	if hl+comp > len(b) || regen > maxBlock {
		return 0, errCorrupt
	}
	data := b[hl : hl+comp]
	if typ == 2 {
		t, n, err := readHufTable(data)
		if err != nil {
			return 0, err
		}
		d.huf = t
		data = data[n:]
	} else if d.huf == nil {
		return 0, errCorrupt
	}
	d.lits = d.lits[:0]
	if streams == 1 {
		var err error
		if d.lits, err = d.huf.decode(d.lits, data, regen); err != nil {
			return 0, err
		}
		return hl + comp, nil
	}
	if len(data) < 6 {
		return 0, errCorrupt
	}
	s1, s2, s3 := int(binary.LittleEndian.Uint16(data)), int(binary.LittleEndian.Uint16(data[2:])), int(binary.LittleEndian.Uint16(data[4:]))
	data = data[6:]
	if s1+s2+s3 > len(data) {
		return 0, errCorrupt
	}
	parts := [4][]byte{data[:s1], data[s1 : s1+s2], data[s1+s2 : s1+s2+s3], data[s1+s2+s3:]}
	each := (regen + 3) / 4
	for i, part := range parts {
		n := each
		if i == 3 {
			n = regen - 3*each
		}
		if n < 0 {
			return 0, errCorrupt
		}
		var err error
		if d.lits, err = d.huf.decode(d.lits, part, n); err != nil {
			return 0, err
		}
	}
	return hl + comp, nil
}

// ---- bit streams ----

// backReader reads a bit stream backwards from its end (as FSE and
// Huffman streams are written): the last byte's top set bit marks the
// start. Reading past the beginning gives zeros (and pos goes negative).
type backReader struct {
	b   []byte
	pos int // bits left
}

func newBackReader(b []byte) (*backReader, error) {
	if len(b) == 0 || b[len(b)-1] == 0 {
		return nil, errCorrupt
	}
	return &backReader{b: b, pos: (len(b)-1)*8 + bits.Len8(b[len(b)-1]) - 1}, nil
}

// window gives 57 bits of the stream ending at bit end (bit end-1 first).
func (r *backReader) peek(n int) uint64 {
	if n == 0 {
		return 0
	}
	start := r.pos - n
	shift := 0
	if start < 0 {
		shift, start = -start, 0
	}
	byteAt := start >> 3
	var w uint64
	if byteAt+8 <= len(r.b) {
		w = binary.LittleEndian.Uint64(r.b[byteAt:])
	} else {
		for i := len(r.b) - 1; i >= byteAt; i-- {
			w = w<<8 | uint64(r.b[i])
		}
	}
	w >>= uint(start & 7)
	k := n - shift
	if k <= 0 {
		return 0
	}
	return (w & (1<<uint(k) - 1)) << uint(shift)
}

func (r *backReader) read(n int) uint64 {
	v := r.peek(n)
	r.pos -= n
	return v
}

// ---- FSE ----

type fseEntry struct {
	sym  uint8
	bits uint8
	base uint16
}

type fseTable struct {
	log int
	t   []fseEntry
}

// buildFSE makes the decoding table for normalized counts (-1 = "less than
// one").
func buildFSE(norm []int, log int) (*fseTable, error) {
	size := 1 << log
	t := &fseTable{log: log, t: make([]fseEntry, size)}
	high := size - 1
	next := make([]int, len(norm))
	for s, c := range norm {
		if c == -1 {
			if high < 0 {
				return nil, errCorrupt
			}
			t.t[high].sym = uint8(s)
			high--
			next[s] = 1
		} else {
			next[s] = c
		}
	}
	pos, step, mask := 0, size>>1+size>>3+3, size-1
	for s, c := range norm {
		for i := 0; i < c; i++ {
			t.t[pos].sym = uint8(s)
			pos = (pos + step) & mask
			for pos > high {
				pos = (pos + step) & mask
			}
		}
	}
	if pos != 0 {
		return nil, errCorrupt
	}
	for i := range t.t {
		s := t.t[i].sym
		n := next[s]
		next[s]++
		if n == 0 {
			return nil, errCorrupt
		}
		nb := log - (bits.Len(uint(n)) - 1)
		t.t[i].bits = uint8(nb)
		t.t[i].base = uint16(n<<uint(nb) - size)
	}
	return t, nil
}

// readFSE reads a table description (a forward bit stream); it gives the
// table and the bytes used.
func readFSE(b []byte, maxSym, maxLog int) (*fseTable, int, error) {
	pos := 0 // in bits
	read := func(n int) int {
		v := 0
		for i := 0; i < n; i++ {
			p := pos + i
			if p>>3 < len(b) && b[p>>3]>>(p&7)&1 != 0 {
				v |= 1 << i
			}
		}
		return v
	}
	peekN := read
	if len(b) < 1 {
		return nil, 0, errCorrupt
	}
	log := read(4) + 5
	pos += 4
	if log > maxLog {
		return nil, 0, errCorrupt
	}
	remaining := 1<<log + 1
	threshold := 1 << log
	nb := log + 1
	var norm []int
	for remaining > 1 && len(norm) <= maxSym {
		max := 2*threshold - 1 - remaining
		var count int
		if low := peekN(nb - 1); low < max {
			count = low
			pos += nb - 1
		} else {
			count = peekN(nb)
			if count >= threshold {
				count -= max
			}
			pos += nb
		}
		count--
		if count < 0 {
			remaining--
		} else {
			remaining -= count
		}
		norm = append(norm, count)
		if count == 0 {
			for {
				rep := read(2)
				pos += 2
				for i := 0; i < rep; i++ {
					norm = append(norm, 0)
				}
				if rep != 3 {
					break
				}
			}
		}
		for remaining < threshold && threshold > 1 {
			nb--
			threshold >>= 1
		}
		if pos > len(b)*8 {
			return nil, 0, errCorrupt
		}
	}
	if remaining != 1 || len(norm) > maxSym+1 {
		return nil, 0, errCorrupt
	}
	t, err := buildFSE(norm, log)
	return t, (pos + 7) / 8, err
}

// ---- Huffman ----

type hufTable struct {
	maxBits int
	sym     []uint8
	nbits   []uint8
}

func readHufTable(b []byte) (*hufTable, int, error) {
	if len(b) < 1 {
		return nil, 0, errCorrupt
	}
	h := int(b[0])
	var w []int
	used := 1
	if h < 128 {
		if 1+h > len(b) {
			return nil, 0, errCorrupt
		}
		data := b[1 : 1+h]
		used += h
		t, n, err := readFSE(data, 255, 6)
		if err != nil {
			return nil, 0, err
		}
		r, err := newBackReader(data[n:])
		if err != nil {
			return nil, 0, err
		}
		s1 := int(r.read(t.log))
		s2 := int(r.read(t.log))
		step := func(s int) int {
			e := t.t[s]
			return int(e.base) + int(r.read(int(e.bits)))
		}
		for len(w) < 255 {
			w = append(w, int(t.t[s1].sym))
			s1 = step(s1)
			if r.pos < 0 {
				w = append(w, int(t.t[s2].sym))
				break
			}
			w = append(w, int(t.t[s2].sym))
			s2 = step(s2)
			if r.pos < 0 {
				w = append(w, int(t.t[s1].sym))
				break
			}
		}
	} else {
		n := h - 127
		if 1+(n+1)/2 > len(b) {
			return nil, 0, errCorrupt
		}
		for i := 0; i < n; i++ {
			v := b[1+i/2]
			if i%2 == 0 {
				w = append(w, int(v>>4))
			} else {
				w = append(w, int(v&15))
			}
		}
		used += (n + 1) / 2
	}
	// the last weight makes the total a power of two
	total := 0
	for _, x := range w {
		if x > 12 {
			return nil, 0, errCorrupt
		}
		if x > 0 {
			total += 1 << (x - 1)
		}
	}
	if total == 0 {
		return nil, 0, errCorrupt
	}
	maxBits := bits.Len(uint(total))
	rest := 1<<maxBits - total
	if rest&(rest-1) != 0 || maxBits > 11 {
		return nil, 0, errCorrupt
	}
	w = append(w, bits.Len(uint(rest)))
	if len(w) > 256 {
		return nil, 0, errCorrupt
	}
	t := &hufTable{maxBits: maxBits, sym: make([]uint8, 1<<maxBits), nbits: make([]uint8, 1<<maxBits)}
	pos := 0
	for weight := 1; weight <= maxBits; weight++ {
		for s, x := range w {
			if x != weight {
				continue
			}
			n := 1 << (weight - 1)
			for i := 0; i < n; i++ {
				t.sym[pos+i] = uint8(s)
				t.nbits[pos+i] = uint8(maxBits + 1 - weight)
			}
			pos += n
		}
	}
	if pos != 1<<maxBits {
		return nil, 0, errCorrupt
	}
	return t, used, nil
}

func (t *hufTable) decode(dst, stream []byte, n int) ([]byte, error) {
	r, err := newBackReader(stream)
	if err != nil {
		return nil, err
	}
	for i := 0; i < n; i++ {
		v := r.peek(t.maxBits)
		dst = append(dst, t.sym[v])
		r.pos -= int(t.nbits[v])
	}
	if r.pos != 0 {
		return nil, errCorrupt
	}
	return dst, nil
}

// ---- sequences ----

var (
	llDefault = []int{4, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 2, 1, 1, 1, 1, 1, -1, -1, -1, -1}
	mlDefault = []int{1, 4, 3, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1, -1, -1}
	ofDefault = []int{1, 1, 1, 1, 1, 1, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1}

	llTab, mlTab, ofTab *fseTable

	llBase = []uint32{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 18, 20, 22, 24, 28, 32, 40, 48, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536}
	llBits = []uint8{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 3, 3, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}
	mlBase = []uint32{3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34,
		35, 37, 39, 41, 43, 47, 51, 59, 67, 83, 99, 131, 259, 515, 1027, 2051, 4099, 8195, 16387, 32771, 65539}
	mlBits = []uint8{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		1, 1, 1, 1, 2, 2, 3, 3, 4, 4, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}
)

func init() {
	llTab, _ = buildFSE(llDefault, 6)
	mlTab, _ = buildFSE(mlDefault, 6)
	ofTab, _ = buildFSE(ofDefault, 5)
}

// table reads one of the three symbol tables per its mode.
func table(mode byte, b []byte, prev **fseTable, def *fseTable, maxSym, maxLog int) (int, error) {
	switch mode {
	case 0:
		*prev = def
		return 0, nil
	case 1:
		if len(b) < 1 || int(b[0]) > maxSym {
			return 0, errCorrupt
		}
		*prev = &fseTable{log: 0, t: []fseEntry{{sym: b[0]}}}
		return 1, nil
	case 2:
		t, n, err := readFSE(b, maxSym, maxLog)
		if err != nil {
			return 0, err
		}
		*prev = t
		return n, nil
	}
	if *prev == nil {
		return 0, errCorrupt
	}
	return 0, nil
}

func (d *decoder) sequences(b []byte) error {
	if len(b) < 1 {
		return errCorrupt
	}
	n := int(b[0])
	p := 1
	switch {
	case n == 0:
		d.out = append(d.out, d.lits...)
		return nil
	case n == 255:
		if len(b) < 3 {
			return errCorrupt
		}
		n, p = int(b[1])+int(b[2])<<8+0x7f00, 3
	case n >= 128:
		if len(b) < 2 {
			return errCorrupt
		}
		n, p = (n-128)<<8+int(b[1]), 2
	}
	if p >= len(b) {
		return errCorrupt
	}
	modes := b[p]
	p++
	if modes&3 != 0 {
		return errCorrupt
	}
	k, err := table(modes>>6, b[p:], &d.ll, llTab, 35, 9)
	if err != nil {
		return err
	}
	p += k
	if k, err = table(modes>>4&3, b[p:], &d.of, ofTab, 31, 8); err != nil {
		return err
	}
	p += k
	if k, err = table(modes>>2&3, b[p:], &d.ml, mlTab, 52, 9); err != nil {
		return err
	}
	p += k
	r, err := newBackReader(b[p:])
	if err != nil {
		return err
	}
	ls := int(r.read(d.ll.log))
	os := int(r.read(d.of.log))
	ms := int(r.read(d.ml.log))
	lit := 0
	for i := 0; i < n; i++ {
		lc, oc, mc := d.ll.t[ls].sym, d.of.t[os].sym, d.ml.t[ms].sym
		if lc > 35 || mc > 52 || oc > 31 {
			return errCorrupt
		}
		ofv := 1<<oc + int(r.read(int(oc)))
		ml := int(mlBase[mc]) + int(r.read(int(mlBits[mc])))
		ll := int(llBase[lc]) + int(r.read(int(llBits[lc])))
		var off int
		if ofv > 3 {
			off = ofv - 3
			d.rep[2], d.rep[1], d.rep[0] = d.rep[1], d.rep[0], off
		} else {
			idx := ofv - 1
			if ll == 0 {
				idx++
			}
			if idx == 0 {
				off = d.rep[0]
			} else {
				if idx == 3 {
					off = d.rep[0] - 1
				} else {
					off = d.rep[idx]
				}
				if idx > 1 {
					d.rep[2] = d.rep[1]
				}
				d.rep[1], d.rep[0] = d.rep[0], off
			}
		}
		if i < n-1 {
			e := d.ll.t[ls]
			ls = int(e.base) + int(r.read(int(e.bits)))
			e = d.ml.t[ms]
			ms = int(e.base) + int(r.read(int(e.bits)))
			e = d.of.t[os]
			os = int(e.base) + int(r.read(int(e.bits)))
		}
		if lit+ll > len(d.lits) {
			return errCorrupt
		}
		d.out = append(d.out, d.lits[lit:lit+ll]...)
		lit += ll
		from := len(d.out) - off
		if off <= 0 || from < d.start {
			return errCorrupt
		}
		if off >= ml {
			d.out = append(d.out, d.out[from:from+ml]...)
		} else {
			for j := 0; j < ml; j++ {
				d.out = append(d.out, d.out[from+j])
			}
		}
	}
	if r.pos != 0 {
		return errCorrupt
	}
	d.out = append(d.out, d.lits[lit:]...)
	return nil
}
