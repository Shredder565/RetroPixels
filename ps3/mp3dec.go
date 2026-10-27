package ps3

// An MPEG audio Layer III (MP3) decoder: MPEG-1, MPEG-2 and MPEG-2.5, mono
// and stereo (with M/S and intensity stereo). The game's music is MP3,
// which the console decodes with a firmware plugin; RetroPixels decodes it
// itself (mp3pic.go).

import (
	"encoding/binary"
	"errors"
	"math"
)

// mp3Dec is one stream's decoder state.
type mp3Dec struct {
	res     []byte             // the bit reservoir: earlier frames' main data
	over    [2][32][18]float32 // the second halves of the last IMDCTs
	v       [2][1024]float32   // the synthesis filter's history
	sHist   [2][15][32]float32 // the last 15 subband sample sets (v can be remade from them)
	id      uint16             // which stream (debugging)
	sinePos int
	vPos    [2]int
	xr      [2][576]float32 // the current granule's lines
	is      [2][576]int32   // its Huffman values
	sf      [2][3][13]int   // scalefactors: long [0][sfb], short [win][sfb]
	sfLong  [2][22]int
	gr      [2][2]mp3Granule
	// MPEG-2 intensity stereo: the "illegal" position per band (the right
	// channel's largest scalefactor value)
	lsfIll  [2][22]int
	lsfIllS [2][3][13]int
}

type mp3Granule struct {
	part23, bigValues, globalGain, sfCompress int
	winSwitch, mixed                          bool
	blockType                                 int
	table                                     [3]int
	subGain                                   [3]int
	region0, region1                          int
	preflag, sfScale, count1Table             int
	scfsi                                     [4]bool
	nonzero                                   int // lines decoded (the rest are zero)
}

type mp3Header struct {
	lsf, mpeg25   bool // MPEG-2 / MPEG-2.5
	crc           bool
	bitrate, rate int
	sri           int // sample rate index into the band tables (0..8)
	pad           int
	mode, modeExt int
	channels      int
	size          int // the frame's length in bytes
	granules      int
	sideLen       int
}

var mp3Rates = [3]int{44100, 48000, 32000}
var mp3Bitrates = [2][15]int{
	{0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320},
	{0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160},
}

func mp3ParseHeader(b []byte) (h mp3Header, ok bool) {
	if len(b) < 4 || b[0] != 0xff || b[1]&0xe0 != 0xe0 {
		return h, false
	}
	ver := b[1] >> 3 & 3
	if ver == 1 || b[1]>>1&3 != 1 {
		return h, false // reserved, or not Layer III
	}
	bri, ri := int(b[2]>>4), int(b[2]>>2&3)
	if bri == 0 || bri == 15 || ri == 3 {
		return h, false // (free format isn't supported)
	}
	h.lsf = ver != 3
	h.mpeg25 = ver == 0
	h.crc = b[1]&1 == 0
	h.pad = int(b[2] >> 1 & 1)
	h.mode = int(b[3] >> 6)
	h.modeExt = int(b[3] >> 4 & 3)
	h.channels = 2
	if h.mode == 3 {
		h.channels = 1
	}
	h.rate = mp3Rates[ri]
	h.sri = ri
	if h.lsf {
		h.rate /= 2
		h.sri += 3
		if h.mpeg25 {
			h.rate /= 2
			h.sri += 3
		}
		h.bitrate = mp3Bitrates[1][bri] * 1000
		h.size = 72*h.bitrate/h.rate + h.pad
		h.granules = 1
		h.sideLen = 9
		if h.channels == 2 {
			h.sideLen = 17
		}
	} else {
		h.bitrate = mp3Bitrates[0][bri] * 1000
		h.size = 144*h.bitrate/h.rate + h.pad
		h.granules = 2
		h.sideLen = 17
		if h.channels == 2 {
			h.sideLen = 32
		}
	}
	return h, true
}

// bit reading, most significant bit first
type mp3Bits struct {
	b   []byte
	pos int // in bits
}

func (r *mp3Bits) bit() int {
	i := r.pos >> 3
	r.pos++
	if i >= len(r.b) {
		return 0
	}
	return int(r.b[i] >> (7 - uint(r.pos-1)&7) & 1)
}

func (r *mp3Bits) bits(n int) int {
	v := 0
	for ; n > 0; n-- {
		v = v<<1 | r.bit()
	}
	return v
}

// Huffman decoding trees, built from the tables: node pairs of children;
// a child >= 0 is a node, < 0 is a leaf -(value+1).
type mp3Tree []int32

func mp3BuildTree(n int, codes func(i int) (uint32, int)) mp3Tree {
	t := mp3Tree{0, 0}
	for i := 0; i < n; i++ {
		c, l := codes(i)
		node := 0
		for k := l - 1; k >= 0; k-- {
			slot := node + int(c>>uint(k)&1)
			if k == 0 {
				t[slot] = int32(-(i + 1))
				break
			}
			if t[slot] <= 0 {
				t[slot] = int32(len(t))
				t = append(t, 0, 0)
			}
			node = int(t[slot])
		}
	}
	return t
}

var mp3Trees [25]mp3Tree
var mp3QuadTree mp3Tree

// mp3Win: the synthesis window, and mp3Cos the synthesis matrix
var (
	mp3Win    [512]float32
	mp3Cos    [64][32]float32
	mp3ImdctL [18][36]float32
	mp3ImdctS [6][12]float32
	mp3Blocks [4][36]float32 // windows for block types 0..3 (2: short)
	mp3CS     [8]float32
	mp3CA     [8]float32
	mp3Pow43  [8207]float32
)

func init() {
	for t, h := range mp3Huffs {
		if h.n == 0 {
			continue
		}
		mp3Trees[t] = mp3BuildTree(h.n*h.n, func(i int) (uint32, int) { return uint32(h.code[i]), int(h.len[i]) })
	}
	mp3QuadTree = mp3BuildTree(16, func(i int) (uint32, int) { return uint32(mp3QuadCode[i]), int(mp3QuadLen[i]) })

	for i := 0; i <= 256; i++ {
		v := float32(mp3Window[i]) / 65536
		mp3Win[i] = v
		if i > 0 && i < 256 {
			if i&63 != 0 {
				mp3Win[512-i] = -v
			} else {
				mp3Win[512-i] = v
			}
		}
	}
	for i := 0; i < 64; i++ {
		for k := 0; k < 32; k++ {
			mp3Cos[i][k] = float32(math.Cos(float64((16+i)*(2*k+1)) * math.Pi / 64))
		}
	}
	for i := 0; i < 36; i++ {
		for k := 0; k < 18; k++ {
			mp3ImdctL[k][i] = float32(math.Cos(math.Pi / 72 * float64((2*i+1+18)*(2*k+1))))
		}
	}
	for i := 0; i < 12; i++ {
		for k := 0; k < 6; k++ {
			mp3ImdctS[k][i] = float32(math.Cos(math.Pi / 24 * float64((2*i+1+6)*(2*k+1))))
		}
	}
	for i := 0; i < 36; i++ {
		s := float32(math.Sin(math.Pi / 36 * (float64(i) + 0.5)))
		mp3Blocks[0][i] = s
		mp3Blocks[1][i] = s
		mp3Blocks[3][i] = s
	}
	for i := 18; i < 36; i++ {
		switch {
		case i < 24:
			mp3Blocks[1][i] = 1
		case i < 30:
			mp3Blocks[1][i] = float32(math.Sin(math.Pi / 12 * (float64(i-18) + 0.5)))
		default:
			mp3Blocks[1][i] = 0
		}
	}
	for i := 0; i < 18; i++ {
		switch {
		case i < 6:
			mp3Blocks[3][i] = 0
		case i < 12:
			mp3Blocks[3][i] = float32(math.Sin(math.Pi / 12 * (float64(i-6) + 0.5)))
		default:
			mp3Blocks[3][i] = 1
		}
	}
	for i := 0; i < 12; i++ {
		mp3Blocks[2][i] = float32(math.Sin(math.Pi / 12 * (float64(i) + 0.5)))
	}
	ci := [8]float64{-0.6, -0.535, -0.33, -0.185, -0.095, -0.041, -0.0142, -0.0037}
	for i, c := range ci {
		d := math.Sqrt(1 + c*c)
		mp3CS[i] = float32(1 / d)
		mp3CA[i] = float32(c / d)
	}
	for i := range mp3Pow43 {
		mp3Pow43[i] = float32(math.Pow(float64(i), 4.0/3))
	}
}

func (r *mp3Bits) huff(t mp3Tree) int {
	node := int32(0)
	for {
		c := t[node+int32(r.bit())]
		if c < 0 {
			return int(-c - 1)
		}
		if c == 0 {
			return 0 // (an unused code)
		}
		node = c
	}
}

var errMP3 = errors.New("mp3: bad frame")

// decode decodes one frame (the whole frame, from its header) into out:
// per channel, the samples (1152 or 576 a channel) in -1..1.
func (d *mp3Dec) decode(frame []byte, out *[2][1152]float32) (h mp3Header, samples int, err error) {
	h, ok := mp3ParseHeader(frame)
	if !ok || len(frame) < h.size {
		return h, 0, errMP3
	}
	frame = frame[:h.size]
	r := mp3Bits{b: frame, pos: 32}
	if h.crc {
		r.pos += 16
	}
	if r.pos/8+h.sideLen > len(frame) {
		return h, 0, errMP3
	}
	// side information
	var mainBegin int
	nch := h.channels
	if h.lsf {
		mainBegin = r.bits(8)
		r.pos += nch // private bits
	} else {
		mainBegin = r.bits(9)
		if nch == 1 {
			r.pos += 5
		} else {
			r.pos += 3
		}
		for ch := 0; ch < nch; ch++ {
			for k := 0; k < 4; k++ {
				d.gr[1][ch].scfsi[k] = r.bit() == 1
			}
		}
	}
	for g := 0; g < h.granules; g++ {
		for ch := 0; ch < nch; ch++ {
			gi := &d.gr[g][ch]
			gi.part23 = r.bits(12)
			gi.bigValues = r.bits(9)
			gi.globalGain = r.bits(8)
			if h.lsf {
				gi.sfCompress = r.bits(9)
			} else {
				gi.sfCompress = r.bits(4)
			}
			gi.winSwitch = r.bit() == 1
			if gi.winSwitch {
				gi.blockType = r.bits(2)
				gi.mixed = r.bit() == 1
				gi.table[0] = r.bits(5)
				gi.table[1] = r.bits(5)
				gi.table[2] = 0
				for w := 0; w < 3; w++ {
					gi.subGain[w] = r.bits(3)
				}
				if gi.blockType == 2 && !gi.mixed {
					gi.region0 = 8
				} else {
					gi.region0 = 7
				}
				gi.region1 = 36
			} else {
				gi.blockType, gi.mixed = 0, false
				for k := 0; k < 3; k++ {
					gi.table[k] = r.bits(5)
				}
				gi.region0 = r.bits(4)
				gi.region1 = r.bits(3)
			}
			if !h.lsf {
				gi.preflag = r.bit()
			}
			gi.sfScale = r.bit()
			gi.count1Table = r.bit()
			if g == 0 && !h.lsf {
				gi.scfsi = [4]bool{}
			}
		}
	}

	// main data: the reservoir, then this frame's
	start := r.pos / 8
	if start > len(frame) {
		return h, 0, errMP3
	}
	have := len(d.res)
	d.res = append(d.res, frame[start:]...)
	if mainBegin > have {
		// not enough of the earlier frames (the stream's start, or a seek):
		// keep the data for the next frame and output silence
		d.trimRes()
		n := 576 * h.granules
		for ch := 0; ch < nch; ch++ {
			clear(out[ch][:n])
		}
		return h, n, nil
	}
	m := mp3Bits{b: d.res, pos: (have - mainBegin) * 8}

	for g := 0; g < h.granules; g++ {
		for ch := 0; ch < nch; ch++ {
			gi := &d.gr[g][ch]
			end := m.pos + gi.part23
			part2 := m.pos
			if h.lsf {
				d.lsfScalefactors(&m, h, gi, ch)
			} else {
				d.scalefactors(&m, g, gi, ch)
			}
			part2 = m.pos - part2
			d.huffman(&m, h, gi, ch, end)
			m.pos = end
			d.requantize(h, gi, ch)
		}
		if h.mode == 1 && h.modeExt != 0 {
			d.stereo(h, &d.gr[g][0], &d.gr[g][1])
		}
		for ch := 0; ch < nch; ch++ {
			gi := &d.gr[g][ch]
			d.reorder(h, gi, ch)
			d.antialias(gi, ch)
			d.hybrid(gi, ch)
			d.synth(ch, out[ch][576*g:576*g+576])
		}
	}
	d.trimRes()
	return h, 576 * h.granules, nil
}

// trimRes keeps the last 511 bytes (the most main_data_begin reaches back).
func (d *mp3Dec) trimRes() {
	if n := len(d.res); n > 1024 {
		copy(d.res, d.res[n-1024:])
		d.res = d.res[:1024]
	}
}

func (d *mp3Dec) scalefactors(m *mp3Bits, g int, gi *mp3Granule, ch int) {
	s1, s2 := mp3Slen[0][gi.sfCompress], mp3Slen[1][gi.sfCompress]
	if gi.winSwitch && gi.blockType == 2 {
		sfb := 0
		if gi.mixed {
			for ; sfb < 8; sfb++ {
				d.sfLong[ch][sfb] = m.bits(s1)
			}
			sfb = 3
		}
		for ; sfb < 12; sfb++ {
			n := s1
			if sfb >= 6 {
				n = s2
			}
			for w := 0; w < 3; w++ {
				d.sf[ch][w][sfb] = m.bits(n)
			}
		}
		for w := 0; w < 3; w++ {
			d.sf[ch][w][12] = 0
		}
		return
	}
	bands := [5]int{0, 6, 11, 16, 21}
	for k := 0; k < 4; k++ {
		n := s1
		if k >= 2 {
			n = s2
		}
		if g == 1 && gi.scfsi[k] {
			continue // the same as the first granule's
		}
		for sfb := bands[k]; sfb < bands[k+1]; sfb++ {
			d.sfLong[ch][sfb] = m.bits(n)
		}
	}
	d.sfLong[ch][21] = 0
}

func (d *mp3Dec) lsfScalefactors(m *mp3Bits, h mp3Header, gi *mp3Granule, ch int) {
	var slen [4]int
	var tab int
	sfc := gi.sfCompress
	gi.preflag = 0
	if h.modeExt&1 != 0 && ch == 1 && h.mode == 1 {
		sfc >>= 1
		switch {
		case sfc < 180:
			slen = [4]int{sfc / 36, sfc % 36 / 6, sfc % 6, 0}
			tab = 3
		case sfc < 244:
			sfc -= 180
			slen = [4]int{sfc & 63 >> 4, sfc & 15 >> 2, sfc & 3, 0}
			tab = 4
		default:
			sfc -= 244
			slen = [4]int{sfc / 3, sfc % 3, 0, 0}
			tab = 5
		}
	} else {
		switch {
		case sfc < 400:
			slen = [4]int{(sfc >> 4) / 5, (sfc >> 4) % 5, sfc & 15 >> 2, sfc & 3}
		case sfc < 500:
			sfc -= 400
			slen = [4]int{(sfc >> 2) / 5, (sfc >> 2) % 5, sfc & 3, 0}
			tab = 1
		default:
			sfc -= 500
			slen = [4]int{sfc / 3, sfc % 3, 0, 0}
			tab = 2
			gi.preflag = 1
		}
	}
	kind := 0
	if gi.winSwitch && gi.blockType == 2 {
		kind = 1
		if gi.mixed {
			kind = 2
		}
	}
	counts := mp3LSFCounts[tab][kind]
	// the values in order, then spread over the bands
	var vals [40]int
	var maxv [40]int // the "illegal" intensity position for each
	n := 0
	for k := 0; k < 4; k++ {
		for i := 0; i < counts[k]; i++ {
			vals[n] = m.bits(slen[k])
			maxv[n] = 1<<uint(slen[k]) - 1
			n++
		}
	}
	i := 0
	if kind == 0 {
		for sfb := 0; sfb < 21; sfb++ {
			d.sfLong[ch][sfb] = vals[i]
			d.lsfIll[ch][sfb] = maxv[i]
			i++
		}
		d.sfLong[ch][21] = 0
		return
	}
	sfb := 0
	if kind == 2 {
		for ; sfb < 6; sfb++ {
			d.sfLong[ch][sfb] = vals[i]
			d.lsfIll[ch][sfb] = maxv[i]
			i++
		}
		sfb = 3
	}
	for ; sfb < 12; sfb++ {
		for w := 0; w < 3; w++ {
			d.sf[ch][w][sfb] = vals[i]
			d.lsfIllS[ch][w][sfb] = maxv[i]
			i++
		}
	}
	for w := 0; w < 3; w++ {
		d.sf[ch][w][12] = 0
	}
}

func (d *mp3Dec) huffman(m *mp3Bits, h mp3Header, gi *mp3Granule, ch int, end int) {
	is := &d.is[ch]
	clear(is[:])
	big := min(gi.bigValues*2, 576)
	var r1, r2 int
	if gi.winSwitch && gi.blockType == 2 {
		r1 = 36
		if h.sri == 8 {
			r1 = 72
		}
		r2 = 576
	} else {
		bl := &mp3BandLong[h.sri]
		r1 = bl[min(gi.region0+1, 22)]
		r2 = bl[min(gi.region0+gi.region1+2, 22)]
	}
	i := 0
	for ; i < big; i += 2 {
		t := gi.table[2]
		if i < r1 {
			t = gi.table[0]
		} else if i < r2 {
			t = gi.table[1]
		}
		if t == 0 || t == 4 || t == 14 {
			continue
		}
		tree := mp3Trees[min(t, 16)]
		n := mp3Huffs[min(t, 16)].n
		if t >= 24 {
			tree = mp3Trees[24]
			n = 16
		}
		v := m.huff(tree)
		x, y := v/n, v%n
		lb := mp3Linbits[t]
		if x == 15 && lb > 0 {
			x += m.bits(lb)
		}
		if x != 0 && m.bit() == 1 {
			x = -x
		}
		if y == 15 && lb > 0 {
			y += m.bits(lb)
		}
		if y != 0 && m.bit() == 1 {
			y = -y
		}
		is[i], is[i+1] = int32(x), int32(y)
	}
	// count1: quadruples of -1, 0, 1
	for i+4 <= 576 && m.pos < end {
		var v int
		if gi.count1Table == 1 {
			v = 15 - m.bits(4)
		} else {
			v = m.huff(mp3QuadTree)
		}
		q := [4]int{v >> 3 & 1, v >> 2 & 1, v >> 1 & 1, v & 1}
		for k := range q {
			if q[k] != 0 && m.bit() == 1 {
				q[k] = -1
			}
		}
		if m.pos > end {
			break // read past the granule's data: not a real quadruple
		}
		for k := range q {
			is[i+k] = int32(q[k])
		}
		i += 4
	}
	gi.nonzero = i
}

func (d *mp3Dec) requantize(h mp3Header, gi *mp3Granule, ch int) {
	xr := &d.xr[ch]
	is := &d.is[ch]
	mult := 0.5 * float64(1+gi.sfScale)
	gain := float64(gi.globalGain-210) * 0.25
	pow := func(i int32, e float64) float32 {
		if i == 0 {
			return 0
		}
		a := i
		if a < 0 {
			a = -a
		}
		if int(a) >= len(mp3Pow43) {
			a = int32(len(mp3Pow43) - 1)
		}
		v := mp3Pow43[a] * float32(math.Exp2(e))
		if i < 0 {
			return -v
		}
		return v
	}
	bl := &mp3BandLong[h.sri]
	bs := &mp3BandShort[h.sri]
	short := gi.winSwitch && gi.blockType == 2
	longEnd := 0
	if !short {
		longEnd = 576
	} else if gi.mixed {
		longEnd = 36
		if h.lsf {
			longEnd = bl[6]
		}
	}
	sfb := 0
	for i := 0; i < longEnd; i++ {
		for i >= bl[sfb+1] {
			sfb++
		}
		sf := 0
		if sfb < 22 {
			sf = d.sfLong[ch][sfb] + gi.preflag*mp3Pretab[sfb]
		}
		xr[i] = pow(is[i], gain-mult*float64(sf))
	}
	if longEnd == 576 {
		return
	}
	// short blocks: band by band, window by window
	s := 0
	if gi.mixed {
		s = 3
	}
	for i := longEnd; s < 13; s++ {
		w := bs[s+1] - bs[s]
		for win := 0; win < 3; win++ {
			e := gain - 2*float64(gi.subGain[win]) - mult*float64(d.sf[ch][win][s])
			for k := 0; k < w && i < 576; k++ {
				xr[i] = pow(is[i], e)
				i++
			}
		}
	}
}

// stereo: M/S and intensity stereo (for the joint stereo mode).
func (d *mp3Dec) stereo(h mp3Header, g0, g1 *mp3Granule) {
	ms := h.modeExt&2 != 0
	intensity := h.modeExt&1 != 0
	l, r := &d.xr[0], &d.xr[1]
	var isPos [576]int // per line: the intensity position, or -1
	for i := range isPos {
		isPos[i] = -1
	}
	if intensity {
		bl := &mp3BandLong[h.sri]
		bs := &mp3BandShort[h.sri]
		short := g1.winSwitch && g1.blockType == 2
		if !short {
			last := -1
			for i := 575; i >= 0; i-- {
				if r[i] != 0 {
					last = i
					break
				}
			}
			for sfb := 0; sfb < 22; sfb++ {
				if bl[sfb] <= last {
					continue
				}
				p, ill := d.isValue(h, sfb, -1)
				if p == ill {
					continue
				}
				for i := bl[sfb]; i < bl[sfb+1]; i++ {
					isPos[i] = p
				}
			}
		} else {
			first := 0
			if g1.mixed {
				first = 3
			}
			for win := 0; win < 3; win++ {
				// the last band of this window with something in it
				lastBand := -1
				for s := 12; s >= first && lastBand < 0; s-- {
					w := bs[s+1] - bs[s]
					st := 3*bs[s] + win*w
					for k := 0; k < w; k++ {
						if r[st+k] != 0 {
							lastBand = s
							break
						}
					}
				}
				if lastBand < first && g1.mixed {
					// (the long bands of a mixed block aren't intensity
					// coded unless the whole short part is)
				}
				for s := max(lastBand+1, first); s < 13; s++ {
					p, ill := d.isValue(h, s, win)
					if p == ill {
						continue
					}
					w := bs[s+1] - bs[s]
					st := 3*bs[s] + win*w
					for k := 0; k < w; k++ {
						isPos[st+k] = p
					}
				}
			}
		}
	}
	io := 0.0
	if h.lsf {
		io = math.Exp2(-0.25)
		if g1.sfCompress&1 != 0 {
			io = math.Exp2(-0.5)
		}
	}
	for i := 0; i < 576; i++ {
		p := isPos[i]
		switch {
		case p >= 0 && !h.lsf:
			a := float64(p) * math.Pi / 12
			sn, cs := math.Sin(a), math.Cos(a)
			v := l[i]
			l[i] = v * float32(sn/(sn+cs))
			r[i] = v * float32(cs/(sn+cs))
		case p >= 0:
			kl, kr := 1.0, 1.0
			if p&1 != 0 {
				kl = math.Pow(io, float64((p+1)/2))
			} else if p > 0 {
				kr = math.Pow(io, float64(p/2))
			}
			v := l[i]
			l[i] = v * float32(kl)
			r[i] = v * float32(kr)
		case ms:
			m, s := l[i], r[i]
			l[i] = (m + s) * math.Sqrt2 / 2
			r[i] = (m - s) * math.Sqrt2 / 2
		}
	}
}

// isValue: the right channel's intensity position for a band (win < 0:
// long), and the value that means "not intensity coded".
func (d *mp3Dec) isValue(h mp3Header, sfb, win int) (p, ill int) {
	if win < 0 {
		if sfb == 21 {
			sfb = 20
		}
		if h.lsf {
			return d.sfLong[1][sfb], d.lsfIll[1][sfb]
		}
		return d.sfLong[1][sfb], 7
	}
	if sfb == 12 {
		sfb = 11
	}
	if h.lsf {
		return d.sf[1][win][sfb], d.lsfIllS[1][win][sfb]
	}
	return d.sf[1][win][sfb], 7
}

// reorder puts short block lines in the order the IMDCT takes them:
// per band, line by line, the three windows interleaved.
func (d *mp3Dec) reorder(h mp3Header, gi *mp3Granule, ch int) {
	if !(gi.winSwitch && gi.blockType == 2) {
		return
	}
	xr := &d.xr[ch]
	bs := &mp3BandShort[h.sri]
	var tmp [576]float32
	s := 0
	if gi.mixed {
		s = 3
	}
	for ; s < 13; s++ {
		st := 3 * bs[s]
		w := bs[s+1] - bs[s]
		for k := 0; k < w; k++ {
			for win := 0; win < 3; win++ {
				tmp[st+3*k+win] = xr[st+win*w+k]
			}
		}
	}
	from := 0
	if gi.mixed {
		from = 3 * bs[3]
	}
	copy(xr[from:], tmp[from:])
}

func (d *mp3Dec) antialias(gi *mp3Granule, ch int) {
	sbs := 32
	if gi.winSwitch && gi.blockType == 2 {
		if !gi.mixed {
			return
		}
		sbs = 2
	}
	xr := &d.xr[ch]
	for sb := 1; sb < sbs; sb++ {
		for i := 0; i < 8; i++ {
			a, b := xr[18*sb-1-i], xr[18*sb+i]
			xr[18*sb-1-i] = a*mp3CS[i] - b*mp3CA[i]
			xr[18*sb+i] = b*mp3CS[i] + a*mp3CA[i]
		}
	}
}

// hybrid: the IMDCT, windowing and overlap of each subband.
func (d *mp3Dec) hybrid(gi *mp3Granule, ch int) {
	xr := &d.xr[ch]
	short := gi.winSwitch && gi.blockType == 2
	for sb := 0; sb < 32; sb++ {
		x := xr[18*sb : 18*sb+18]
		var out [36]float32
		if short && (!gi.mixed || sb >= 2) {
			for win := 0; win < 3; win++ {
				for i := 0; i < 12; i++ {
					var s float32
					for k := 0; k < 6; k++ {
						s += x[3*k+win] * mp3ImdctS[k][i]
					}
					out[6+6*win+i] += s * mp3Blocks[2][i]
				}
			}
		} else {
			bt := 0
			if gi.winSwitch && !short {
				bt = gi.blockType
			}
			nz := false
			for _, v := range x {
				if v != 0 {
					nz = true
					break
				}
			}
			if nz {
				for i := 0; i < 36; i++ {
					var s float32
					for k := 0; k < 18; k++ {
						s += x[k] * mp3ImdctL[k][i]
					}
					out[i] = s * mp3Blocks[bt][i]
				}
			}
		}
		ov := &d.over[ch][sb]
		for i := 0; i < 18; i++ {
			x[i] = out[i] + ov[i]
			ov[i] = out[18+i]
		}
		if sb&1 == 1 {
			for i := 1; i < 18; i += 2 {
				x[i] = -x[i]
			}
		}
	}
}

// synth: the polyphase synthesis filter bank, 32 subbands to 576 samples.
func (d *mp3Dec) synth(ch int, out []float32) {
	xr := &d.xr[ch]
	for ts := 0; ts < 18; ts++ {
		var s [32]float32
		for k := 0; k < 32; k++ {
			s[k] = xr[18*k+ts]
		}
		copy(d.sHist[ch][:], d.sHist[ch][1:])
		d.sHist[ch][14] = s
		d.synthSlot(ch, &s, out[32*ts:32*ts+32])
	}
}

// synthSlot runs the filter bank for one set of 32 subband samples.
func (d *mp3Dec) synthSlot(ch int, s *[32]float32, out []float32) {
	v := &d.v[ch]
	{
		d.vPos[ch] = (d.vPos[ch] - 64) & 1023
		p := d.vPos[ch]
		for i := 0; i < 64; i++ {
			var sum float32
			c := &mp3Cos[i]
			for k := 0; k < 32; k++ {
				sum += c[k] * s[k]
			}
			v[(p+i)&1023] = sum
		}
		for j := 0; j < 32; j++ {
			var sum float32
			for i := 0; i < 8; i++ {
				sum += v[(p+128*i+j)&1023] * mp3Win[64*i+j]
				sum += v[(p+128*i+96+j)&1023] * mp3Win[64*i+32+j]
			}
			if out != nil {
				out[j] = sum
			}
		}
	}
}

var mp3Streams uint16 // streams started (debugging)

// The state a stream keeps between frames, stored in the engine's
// per-stream context (mono streams: it has room for one channel).
const (
	mp3StateMagic = 0x52504d33 // "RPM3"
	mp3StateSize  = 8 + 512 + 576*4 + 15*32*4 + 4
)

func (d *mp3Dec) save(b []byte) {
	be := binary.BigEndian
	be.PutUint32(b, mp3StateMagic)
	res := d.res
	if len(res) > 512 {
		res = res[len(res)-512:]
	}
	be.PutUint16(b[4:], uint16(len(res)))
	be.PutUint16(b[6:], d.id)
	be.PutUint32(b[mp3StateSize-4:], uint32(d.sinePos))
	copy(b[8:], res)
	o := 8 + 512
	for sb := 0; sb < 32; sb++ {
		for i := 0; i < 18; i++ {
			be.PutUint32(b[o:], math.Float32bits(d.over[0][sb][i]))
			o += 4
		}
	}
	for t := 0; t < 15; t++ {
		for k := 0; k < 32; k++ {
			be.PutUint32(b[o:], math.Float32bits(d.sHist[0][t][k]))
			o += 4
		}
	}
}

// load restores a stream's state (a fresh one if b doesn't hold one).
func (d *mp3Dec) load(b []byte) {
	*d = mp3Dec{res: d.res[:0]}
	be := binary.BigEndian
	if be.Uint32(b) != mp3StateMagic {
		mp3Streams++
		d.id = mp3Streams
		return
	}
	d.id = be.Uint16(b[6:])
	d.sinePos = int(be.Uint32(b[mp3StateSize-4:]))
	n := min(int(be.Uint16(b[4:])), 512)
	d.res = append(d.res, b[8:8+n]...)
	o := 8 + 512
	for sb := 0; sb < 32; sb++ {
		for i := 0; i < 18; i++ {
			d.over[0][sb][i] = math.Float32frombits(be.Uint32(b[o:]))
			o += 4
		}
	}
	for t := 0; t < 15; t++ {
		for k := 0; k < 32; k++ {
			d.sHist[0][t][k] = math.Float32frombits(be.Uint32(b[o:]))
			o += 4
		}
		d.synthSlot(0, &d.sHist[0][t], nil)
	}
}
