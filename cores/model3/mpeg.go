package model3

// An MPEG-1/MPEG-2 audio decoder for Layers I and II: the music board's
// streams are Layer II. Written for RetroPixels from the format's published
// description (frame header, bit allocation tables, scalefactors, grouping,
// polyphase synthesis).

import "math"

const (
	mpegHdrSize          = 4
	mpegMaxSamplesPerFrm = 1152 * 2 // samples (both channels) in one frame
)

// ---- frame headers ----

type mpegHeader struct {
	lsf      bool // MPEG-2 (or 2.5) low sampling frequencies
	layer    int  // 1 or 2 (3 is recognised and skipped)
	crc      bool
	bitrate  int // kbit/s
	rate     int // Hz
	padding  bool
	mode     int // 0 stereo, 1 joint stereo, 2 dual channel, 3 mono
	modeExt  int
	channels int
	bytes    int // the whole frame
}

var mpegKbps = [2][3][15]int{
	{ // MPEG-1: Layer I, II, III
		{0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448},
		{0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384},
		{0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320},
	},
	{ // MPEG-2 and 2.5
		{0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256},
		{0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160},
		{0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160},
	},
}

var mpegRates = [3]int{44100, 48000, 32000}

// parseHeader reads a frame header; ok is false for anything that isn't
// one this decoder can size (free format included).
func parseHeader(b []byte) (h mpegHeader, ok bool) {
	if len(b) < 4 || b[0] != 0xff || b[1]&0xe0 != 0xe0 {
		return h, false
	}
	ver := b[1] >> 3 & 3 // 0 MPEG-2.5, 2 MPEG-2, 3 MPEG-1
	lay := int(b[1] >> 1 & 3)
	bri := int(b[2] >> 4)
	sri := int(b[2] >> 2 & 3)
	if ver == 1 || lay == 0 || bri == 0 || bri == 15 || sri == 3 {
		return h, false
	}
	h.lsf = ver != 3
	h.layer = 4 - lay
	h.crc = b[1]&1 == 0
	v := 0
	if h.lsf {
		v = 1
	}
	h.bitrate = mpegKbps[v][h.layer-1][bri]
	h.rate = mpegRates[sri]
	switch ver {
	case 2:
		h.rate /= 2
	case 0:
		h.rate /= 4
	}
	h.padding = b[2]&2 != 0
	h.mode = int(b[3] >> 6)
	h.modeExt = int(b[3] >> 4 & 3)
	h.channels = 2
	if h.mode == 3 {
		h.channels = 1
	}
	pad := 0
	if h.padding {
		pad = 1
	}
	switch {
	case h.layer == 1:
		h.bytes = (12*h.bitrate*1000/h.rate + pad) * 4
	case h.layer == 3 && h.lsf:
		h.bytes = 72*h.bitrate*1000/h.rate + pad
	default:
		h.bytes = 144*h.bitrate*1000/h.rate + pad
	}
	return h, true
}

func (h mpegHeader) samples() int {
	switch {
	case h.layer == 1:
		return 384
	case h.layer == 3 && h.lsf:
		return 576
	}
	return 1152
}

// sameStream says whether two headers belong to one stream (for telling a
// real frame from bytes that happen to look like a header).
func sameStream(a, b []byte) bool {
	return a[1]&0xfe == b[1]&0xfe && a[2]&0x0c == b[2]&0x0c
}

// ---- bits ----

type bitReader struct {
	b   []byte
	pos int // in bits
}

func (r *bitReader) read(n int) uint32 {
	var v uint32
	for ; n > 0; n-- {
		i := r.pos >> 3
		var bit uint32
		if i < len(r.b) {
			bit = uint32(r.b[i]>>(7-r.pos&7)) & 1
		}
		v = v<<1 | bit
		r.pos++
	}
	return v
}

// ---- Layer II allocation tables ----

// a quantization class: the number of steps, and whether three samples
// share one code
type qClass struct {
	levels  int
	bits    int // per sample, or per group of three
	grouped bool
}

func qc(levels int) qClass {
	switch levels {
	case 3:
		return qClass{3, 5, true}
	case 5:
		return qClass{5, 7, true}
	case 9:
		return qClass{9, 10, true}
	}
	b := 0
	for 1<<b <= levels {
		b++
	}
	return qClass{levels, b, false}
}

// a band: how many bits its allocation takes, and the class each allocation
// picks (index 0 is "no samples")
type allocBand struct {
	nbal    int
	classes []qClass
}

func classes(levels ...int) []qClass {
	out := make([]qClass, len(levels)+1)
	for i, l := range levels {
		out[i+1] = qc(l)
	}
	return out
}

func bands(spec ...any) []allocBand {
	var out []allocBand
	for i := 0; i < len(spec); i += 3 {
		n, nbal, cl := spec[i].(int), spec[i+1].(int), spec[i+2].([]qClass)
		for range n {
			out = append(out, allocBand{nbal, cl})
		}
	}
	return out
}

var (
	l2Hi  = classes(3, 7, 15, 31, 63, 127, 255, 511, 1023, 2047, 4095, 8191, 16383, 32767, 65535)
	l2Mid = classes(3, 5, 7, 9, 15, 31, 63, 127, 255, 511, 1023, 2047, 4095, 8191, 65535)
	l2Lo  = classes(3, 5, 7, 9, 15, 31, 65535)
	l2Top = classes(3, 5, 65535)
	l2C4  = classes(3, 5, 9, 15, 31, 63, 127, 255, 511, 1023, 2047, 4095, 8191, 16383, 32767)
	l2C3  = classes(3, 5, 9, 15, 31, 63, 127)
	l2L2  = classes(3, 5, 9)

	// MPEG-1: higher rates at 48 kHz (and middle ones at 44.1/32), higher
	// rates at 44.1/32 kHz, low rates at 48/44.1 kHz, low rates at 32 kHz
	l2TableA = bands(3, 4, l2Hi, 8, 4, l2Mid, 12, 3, l2Lo, 4, 2, l2Top)
	l2TableB = bands(3, 4, l2Hi, 8, 4, l2Mid, 12, 3, l2Lo, 7, 2, l2Top)
	l2TableC = bands(2, 4, l2C4, 6, 3, l2C3)
	l2TableD = bands(2, 4, l2C4, 10, 3, l2C3)
	// MPEG-2 low sampling frequencies
	l2TableLSF = bands(4, 4, l2C4, 7, 3, l2C3, 19, 2, l2L2)
)

func l2Table(h mpegHeader) []allocBand {
	if h.lsf {
		return l2TableLSF
	}
	perCh := h.bitrate
	if h.channels == 2 {
		perCh /= 2
	}
	switch {
	case perCh <= 48 && h.rate == 32000:
		return l2TableD
	case perCh <= 48:
		return l2TableC
	case perCh >= 96 && h.rate != 48000:
		return l2TableB
	}
	return l2TableA
}

// scalefactor i is 2 * 2^(-i/3)
var mpegScf = func() (t [64]float64) {
	for i := range 63 {
		t[i] = 2 * math.Pow(2, -float64(i)/3)
	}
	return
}()

// dequant turns a code (0..levels-1) into a value in (-1, 1).
func dequant(code, levels int) float64 {
	return float64(2*code-(levels-1)) / float64(levels)
}

// ---- the decoder ----

type mpegDecoder struct {
	v    [2][1024]float64 // synthesis history
	voff [2]int
	sb   [2][36][32]float64 // one frame's subband samples (up to 36 per band)
}

func (d *mpegDecoder) init() { *d = mpegDecoder{} }

// findFrame returns where the next frame starts in b, and its header.
func findFrame(b []byte) (int, mpegHeader, bool) {
	for i := 0; i+4 <= len(b); i++ {
		h, ok := parseHeader(b[i:])
		if !ok {
			continue
		}
		// a frame running to the end of the data counts; otherwise the
		// next frame must start where this one ends
		if j := i + h.bytes; j+4 <= len(b) && !(b[j] == 0xff && sameStream(b[i:], b[j:])) {
			continue
		}
		return i, h, true
	}
	return 0, mpegHeader{}, false
}

// decodeFrame decodes the first frame in data into pcm (interleaved when
// stereo). It returns the samples per channel, the bytes it used (any junk
// before the frame included) and the channels.
func (d *mpegDecoder) decodeFrame(data []byte, pcm []int16) (samples, used, channels int) {
	off, h, ok := findFrame(data)
	if !ok {
		return 0, max(0, len(data)-3), 0
	}
	if off+h.bytes > len(data) {
		return 0, len(data), 0
	}
	frame := data[off : off+h.bytes]
	used = off + h.bytes
	r := &bitReader{b: frame, pos: 32}
	if h.crc {
		r.read(16)
	}
	var n int
	switch h.layer {
	case 1:
		n = d.layer1(r, h)
	case 2:
		n = d.layer2(r, h)
	default:
		return 0, used, 0
	}
	for ch := 0; ch < h.channels; ch++ {
		d.synthesize(ch, n, pcm, h.channels)
	}
	return n * 32, used, h.channels
}

func jointBound(h mpegHeader, limit int) int {
	if h.mode == 1 {
		return min(limit, 4*(h.modeExt+1))
	}
	return limit
}

// layer1 reads a Layer I frame's subband samples (12 per band) and returns 12.
func (d *mpegDecoder) layer1(r *bitReader, h mpegHeader) int {
	nch := h.channels
	bound := jointBound(h, 32)
	var alloc [2][32]int
	var scf [2][32]float64
	for sb := range 32 {
		for ch := range nch {
			if ch == 1 && sb >= bound {
				alloc[1][sb] = alloc[0][sb]
				break
			}
			alloc[ch][sb] = int(r.read(4))
		}
	}
	for sb := range 32 {
		for ch := range nch {
			if alloc[ch][sb] != 0 {
				scf[ch][sb] = mpegScf[r.read(6)]
			}
		}
	}
	for s := range 12 {
		for sb := range 32 {
			for ch := range nch {
				a := alloc[ch][sb]
				if a == 0 || a == 15 {
					d.sb[ch][s][sb] = 0
					continue
				}
				if ch == 1 && sb >= bound {
					continue // set with channel 0
				}
				bits := a + 1
				levels := 1<<bits - 1
				code := int(r.read(bits))
				d.sb[ch][s][sb] = dequant(code, levels) * scf[ch][sb]
				if nch == 2 && sb >= bound {
					d.sb[1][s][sb] = dequant(code, levels) * scf[1][sb]
				}
			}
		}
	}
	return 12
}

// layer2 reads a Layer II frame's subband samples (36 per band) and returns 36.
func (d *mpegDecoder) layer2(r *bitReader, h mpegHeader) int {
	tab := l2Table(h)
	limit := len(tab)
	nch := h.channels
	bound := jointBound(h, limit)
	var alloc [2][32]qClass
	var has [2][32]bool
	for sb := range limit {
		for ch := range nch {
			a := int(r.read(tab[sb].nbal))
			alloc[ch][sb], has[ch][sb] = tab[sb].classes[a], a != 0
			if sb >= bound {
				if nch == 2 {
					alloc[1][sb], has[1][sb] = alloc[0][sb], has[0][sb]
				}
				break
			}
		}
	}
	var scfsi [2][32]uint32
	for sb := range limit {
		for ch := range nch {
			if has[ch][sb] {
				scfsi[ch][sb] = r.read(2)
			}
		}
	}
	var scf [2][32][3]float64
	for sb := range limit {
		for ch := range nch {
			if !has[ch][sb] {
				continue
			}
			s := &scf[ch][sb]
			switch scfsi[ch][sb] {
			case 0:
				s[0], s[1], s[2] = mpegScf[r.read(6)], mpegScf[r.read(6)], mpegScf[r.read(6)]
			case 1:
				s[0] = mpegScf[r.read(6)]
				s[1], s[2] = s[0], mpegScf[r.read(6)]
			case 2:
				s[0] = mpegScf[r.read(6)]
				s[1], s[2] = s[0], s[0]
			case 3:
				s[0] = mpegScf[r.read(6)]
				s[1] = mpegScf[r.read(6)]
				s[2] = s[1]
			}
		}
	}
	for gr := range 12 {
		part := gr / 4
		for sb := 0; sb < 32; sb++ {
			var shared [3]float64
			for ch := range nch {
				var v [3]float64
				if sb < limit && has[ch][sb] && (ch == 0 || sb < bound) {
					q := alloc[ch][sb]
					if q.grouped {
						code := int(r.read(q.bits))
						for k := range 3 {
							v[k] = dequant(code%q.levels, q.levels)
							code /= q.levels
						}
					} else {
						for k := range 3 {
							v[k] = dequant(int(r.read(q.bits)), q.levels)
						}
					}
					shared = v
				} else if ch == 1 && sb >= bound {
					v = shared // joint stereo: channel 0's values, channel 1's scale
				}
				for k := range 3 {
					x := v[k]
					if sb < limit {
						x *= scf[ch][sb][part]
					}
					d.sb[ch][3*gr+k][sb] = x
				}
			}
		}
	}
	return 36
}

// ---- synthesis ----

// mpegCos[i][k] is cos((16+i)(2k+1)pi/64): the matrix that turns 32
// subband samples into 64 values of the synthesis history.
var mpegCos = func() (t [64][32]float64) {
	for i := range 64 {
		for k := range 32 {
			t[i][k] = math.Cos(float64((16+i)*(2*k+1)) * math.Pi / 64)
		}
	}
	return
}()

// mpegWindow is the standard 512-tap synthesis window, in units of
// 1/65536. Its values are the format's published constants; these were
// recovered by measurement (fitting the window to the output of a decoder
// for test streams, each value landing within 0.14 of a whole unit) rather
// than copied from any decoder's source. The entries 64i+16 meet only zeros
// in the synthesis history (the cosine matrix's row 16 is all zero), so they
// are left at 0.
var mpegWindowTab = [512]int32{
	0, -1, -1, -1, -1, -1, -1, -2, -2, -2, -2, -3, -3, -4, -4, -5,
	0, -6, -7, -7, -8, -9, -10, -11, -13, -14, -16, -17, -19, -21, -24, -26,
	-29, -31, -35, -38, -41, -45, -49, -53, -58, -63, -68, -73, -79, -85, -91, -97,
	-104, -111, -117, -125, -132, -139, -147, -154, -161, -169, -176, -183, -190, -196, -202, -208,
	213, 218, 222, 225, 227, 228, 228, 227, 224, 221, 215, 208, 200, 189, 177, 163,
	0, 127, 106, 83, 57, 29, -2, -36, -72, -111, -153, -197, -244, -294, -347, -401,
	-459, -519, -581, -645, -711, -779, -848, -919, -991, -1064, -1137, -1210, -1283, -1356, -1428, -1498,
	-1567, -1634, -1698, -1759, -1817, -1870, -1919, -1962, -2001, -2032, -2057, -2075, -2085, -2087, -2080, -2063,
	2037, 2000, 1952, 1893, 1822, 1739, 1644, 1535, 1414, 1280, 1131, 970, 794, 605, 402, 185,
	0, -288, -545, -814, -1095, -1388, -1692, -2006, -2330, -2663, -3004, -3351, -3705, -4063, -4425, -4788,
	-5153, -5517, -5879, -6237, -6589, -6935, -7271, -7597, -7910, -8209, -8491, -8755, -8998, -9219, -9416, -9585,
	-9727, -9838, -9916, -9959, -9966, -9935, -9863, -9750, -9592, -9389, -9139, -8840, -8492, -8092, -7640, -7134,
	6574, 5959, 5288, 4561, 3776, 2935, 2037, 1082, 70, -998, -2122, -3300, -4533, -5818, -7154, -8540,
	0, -11455, -12980, -14548, -16155, -17799, -19478, -21189, -22929, -24694, -26482, -28289, -30112, -31947, -33791, -35640,
	-37489, -39336, -41176, -43006, -44821, -46617, -48390, -50137, -51853, -53534, -55178, -56778, -58333, -59838, -61289, -62684,
	-64019, -65290, -66494, -67629, -68692, -69679, -70590, -71420, -72169, -72835, -73415, -73908, -74313, -74630, -74856, -74992,
	75038, 74992, 74856, 74630, 74313, 73908, 73415, 72835, 72169, 71420, 70590, 69679, 68692, 67629, 66494, 65290,
	0, 62684, 61289, 59838, 58333, 56778, 55178, 53534, 51853, 50137, 48390, 46617, 44821, 43006, 41176, 39336,
	37489, 35640, 33791, 31947, 30112, 28289, 26482, 24694, 22929, 21189, 19478, 17799, 16155, 14548, 12980, 11455,
	9975, 8540, 7154, 5818, 4533, 3300, 2122, 998, -70, -1082, -2037, -2935, -3776, -4561, -5288, -5959,
	6574, 7134, 7640, 8092, 8492, 8840, 9139, 9389, 9592, 9750, 9863, 9935, 9966, 9959, 9916, 9838,
	0, 9585, 9416, 9219, 8998, 8755, 8491, 8209, 7910, 7597, 7271, 6935, 6589, 6237, 5879, 5517,
	5153, 4788, 4425, 4063, 3705, 3351, 3004, 2663, 2330, 2006, 1692, 1388, 1095, 814, 545, 288,
	45, -185, -402, -605, -794, -970, -1131, -1280, -1414, -1535, -1644, -1739, -1822, -1893, -1952, -2000,
	2037, 2063, 2080, 2087, 2085, 2075, 2057, 2032, 2001, 1962, 1919, 1870, 1817, 1759, 1698, 1634,
	0, 1498, 1428, 1356, 1283, 1210, 1137, 1064, 991, 919, 848, 779, 711, 645, 581, 519,
	459, 401, 347, 294, 244, 197, 153, 111, 72, 36, 2, -29, -57, -83, -106, -127,
	-146, -163, -177, -189, -200, -208, -215, -221, -224, -227, -228, -228, -227, -225, -222, -218,
	213, 208, 202, 196, 190, 183, 176, 169, 161, 154, 147, 139, 132, 125, 117, 111,
	0, 97, 91, 85, 79, 73, 68, 63, 58, 53, 49, 45, 41, 38, 35, 31,
	29, 26, 24, 21, 19, 17, 16, 14, 13, 11, 10, 9, 8, 7, 7, 6,
	5, 5, 4, 4, 3, 3, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1,
}

var mpegWindow = func() (w [512]float64) {
	for i, v := range mpegWindowTab {
		w[i] = float64(v) / 65536
	}
	return
}()

// synthesize turns n sets of 32 subband samples of a channel into n*32 PCM
// samples, written to pcm every step entries starting at ch.
func (d *mpegDecoder) synthesize(ch, n int, pcm []int16, step int) {
	v := &d.v[ch]
	for s := range n {
		in := &d.sb[ch][s]
		d.voff[ch] = (d.voff[ch] - 64) & 1023
		o := d.voff[ch]
		for i := range 64 {
			var acc float64
			c := &mpegCos[i]
			for k := range 32 {
				acc += c[k] * in[k]
			}
			v[(o+i)&1023] = acc
		}
		for j := range 32 {
			var acc float64
			for i := range 8 {
				acc += v[(o+128*i+j)&1023] * mpegWindow[64*i+j]
				acc += v[(o+128*i+96+j)&1023] * mpegWindow[64*i+32+j]
			}
			x := math.Round(acc * 32768)
			pcm[(s*32+j)*step+ch] = int16(max(-32768, min(32767, x)))
		}
	}
}
