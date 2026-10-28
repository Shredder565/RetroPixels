package model3

// An MPEG-1/2 Layer I and II audio decoder for the MPEG music board. It is a
// port of the Layer I/II path of minimp3 (by Lion, CC0 / public domain),
// which Supermodel uses, written out without SIMD.

const (
	mpegHdrSize          = 4
	mpegMaxFreeFormat    = 2304
	mpegMaxSyncMatches   = 10
	mpegMaxSamplesPerFrm = 1152 * 2
	mpegModeMono         = 3
	mpegModeJointStereo  = 1
)

type mpegBits struct {
	buf        []byte
	pos, limit int
}

func (b *mpegBits) get(n int) uint32 {
	s := uint(b.pos & 7)
	shl := n + int(s)
	p := b.pos >> 3
	b.pos += n
	if b.pos > b.limit {
		return 0
	}
	next := uint32(b.buf[p]) & (255 >> s)
	p++
	var cache uint32
	for shl -= 8; shl > 0; shl -= 8 {
		cache |= next << uint(shl)
		next = uint32(b.buf[p])
		p++
	}
	return cache | next>>uint(-shl)
}

func hdrIsMono(h []byte) bool        { return h[3]&0xC0 == 0xC0 }
func hdrIsFreeFormat(h []byte) bool  { return h[2]&0xF0 == 0 }
func hdrIsCRC(h []byte) bool         { return h[1]&1 == 0 }
func hdrTestPadding(h []byte) bool   { return h[2]&2 != 0 }
func hdrTestMPEG1(h []byte) bool     { return h[1]&8 != 0 }
func hdrTestNotMPEG25(h []byte) bool { return h[1]&0x10 != 0 }
func hdrStereoMode(h []byte) int     { return int(h[3] >> 6 & 3) }
func hdrStereoModeExt(h []byte) int  { return int(h[3] >> 4 & 3) }
func hdrLayer(h []byte) int          { return int(h[1] >> 1 & 3) }
func hdrBitrate(h []byte) int        { return int(h[2] >> 4) }
func hdrSampleRate(h []byte) int     { return int(h[2] >> 2 & 3) }
func hdrIsFrame576(h []byte) bool    { return h[1]&14 == 2 }
func hdrIsLayer1(h []byte) bool      { return h[1]&6 == 6 }

func hdrValid(h []byte) bool {
	return h[0] == 0xff && (h[1]&0xF0 == 0xf0 || h[1]&0xFE == 0xe2) &&
		hdrLayer(h) != 0 && hdrBitrate(h) != 15 && hdrSampleRate(h) != 3
}

func hdrCompare(h1, h2 []byte) bool {
	return hdrValid(h2) && (h1[1]^h2[1])&0xFE == 0 && (h1[2]^h2[2])&0x0C == 0 &&
		hdrIsFreeFormat(h1) == hdrIsFreeFormat(h2)
}

var mpegHalfRate = [2][3][15]uint8{
	{{0, 4, 8, 12, 16, 20, 24, 28, 32, 40, 48, 56, 64, 72, 80}, {0, 4, 8, 12, 16, 20, 24, 28, 32, 40, 48, 56, 64, 72, 80}, {0, 16, 24, 28, 32, 40, 48, 56, 64, 72, 80, 88, 96, 112, 128}},
	{{0, 16, 20, 24, 28, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160}, {0, 16, 24, 28, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192}, {0, 16, 32, 48, 64, 80, 96, 112, 128, 144, 160, 176, 192, 208, 224}},
}

func hdrBitrateKbps(h []byte) int {
	m := 0
	if hdrTestMPEG1(h) {
		m = 1
	}
	return 2 * int(mpegHalfRate[m][hdrLayer(h)-1][hdrBitrate(h)])
}

func hdrSampleRateHz(h []byte) int {
	hz := [3]int{44100, 48000, 32000}[hdrSampleRate(h)]
	if !hdrTestMPEG1(h) {
		hz >>= 1
	}
	if !hdrTestNotMPEG25(h) {
		hz >>= 1
	}
	return hz
}

func hdrFrameSamples(h []byte) int {
	if hdrIsLayer1(h) {
		return 384
	}
	if hdrIsFrame576(h) {
		return 576
	}
	return 1152
}

func hdrFrameBytes(h []byte, freeFormatSize int) int {
	fb := hdrFrameSamples(h) * hdrBitrateKbps(h) * 125 / hdrSampleRateHz(h)
	if hdrIsLayer1(h) {
		fb &^= 3
	}
	if fb == 0 {
		return freeFormatSize
	}
	return fb
}

func hdrPadding(h []byte) int {
	if !hdrTestPadding(h) {
		return 0
	}
	if hdrIsLayer1(h) {
		return 4
	}
	return 1
}

type l12ScaleInfo struct {
	scf                     [3 * 64]float32
	totalBands, stereoBands int
	bitalloc                [64]uint8
	scfcod                  [64]uint8
}

type l12Alloc struct{ tabOffset, codeTabWidth, bandCount int }

func l12SubbandAllocTable(h []byte, sci *l12ScaleInfo) []l12Alloc {
	mode := hdrStereoMode(h)
	stereoBands := 32
	if mode == mpegModeMono {
		stereoBands = 0
	} else if mode == mpegModeJointStereo {
		stereoBands = hdrStereoModeExt(h)<<2 + 4
	}
	var alloc []l12Alloc
	nbands := 0
	if hdrIsLayer1(h) {
		alloc = []l12Alloc{{76, 4, 32}}
		nbands = 32
	} else if !hdrTestMPEG1(h) {
		alloc = []l12Alloc{{60, 4, 4}, {44, 3, 7}, {44, 2, 19}}
		nbands = 30
	} else {
		srIdx := hdrSampleRate(h)
		kbps := hdrBitrateKbps(h)
		if mode != mpegModeMono {
			kbps >>= 1
		}
		if kbps == 0 {
			kbps = 192
		}
		alloc = []l12Alloc{{0, 4, 3}, {16, 4, 8}, {32, 3, 12}, {40, 2, 7}}
		nbands = 27
		if kbps < 56 {
			alloc = []l12Alloc{{44, 4, 2}, {44, 3, 10}}
			nbands = 8
			if srIdx == 2 {
				nbands = 12
			}
		} else if kbps >= 96 && srIdx != 1 {
			nbands = 30
		}
	}
	sci.totalBands = nbands
	if stereoBands > nbands {
		stereoBands = nbands
	}
	sci.stereoBands = stereoBands
	return alloc
}

var l12Deq = func() [18 * 3]float32 {
	var t [18 * 3]float32
	div := []float32{3, 7, 15, 31, 63, 127, 255, 511, 1023, 2047, 4095, 8191, 16383, 32767, 65535, 3, 5, 9}
	for i, x := range div {
		t[i*3] = 9.53674316e-07 / x
		t[i*3+1] = 7.56931807e-07 / x
		t[i*3+2] = 6.00777173e-07 / x
	}
	return t
}()

func l12ReadScalefactors(bs *mpegBits, pba, scfcod []uint8, bands int, scf []float32) {
	k := 0
	for i := 0; i < bands; i++ {
		var s float32
		ba := int(pba[i])
		mask := 0
		if ba != 0 {
			mask = 4 + (19>>scfcod[i])&3
		}
		for m := 4; m != 0; m >>= 1 {
			if mask&m != 0 {
				b := int(bs.get(6))
				s = l12Deq[ba*3-6+b%3] * float32(int32(1<<21)>>uint(b/3))
			}
			scf[k] = s
			k++
		}
	}
}

var l12BitallocCodeTab = []uint8{
	0, 17, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
	0, 17, 18, 3, 19, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 16,
	0, 17, 18, 3, 19, 4, 5, 16,
	0, 17, 18, 16,
	0, 17, 18, 19, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
	0, 17, 18, 3, 19, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
	0, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
}

func l12ReadScaleInfo(h []byte, bs *mpegBits, sci *l12ScaleInfo) {
	alloc := l12SubbandAllocTable(h, sci)
	k, baBits, ai := 0, 0, 0
	tab := l12BitallocCodeTab
	for i := 0; i < sci.totalBands; i++ {
		if i == k {
			k += alloc[ai].bandCount
			baBits = alloc[ai].codeTabWidth
			tab = l12BitallocCodeTab[alloc[ai].tabOffset:]
			ai++
		}
		ba := tab[bs.get(baBits)]
		sci.bitalloc[2*i] = ba
		if i < sci.stereoBands {
			ba = tab[bs.get(baBits)]
		}
		if sci.stereoBands != 0 {
			sci.bitalloc[2*i+1] = ba
		} else {
			sci.bitalloc[2*i+1] = 0
		}
	}
	for i := 0; i < 2*sci.totalBands; i++ {
		switch {
		case sci.bitalloc[i] == 0:
			sci.scfcod[i] = 6
		case hdrIsLayer1(h):
			sci.scfcod[i] = 2
		default:
			sci.scfcod[i] = uint8(bs.get(2))
		}
	}
	l12ReadScalefactors(bs, sci.bitalloc[:], sci.scfcod[:], sci.totalBands*2, sci.scf[:])
	for i := sci.stereoBands; i < sci.totalBands; i++ {
		sci.bitalloc[2*i+1] = 0
	}
}

func l12DequantizeGranule(grbuf []float32, bs *mpegBits, sci *l12ScaleInfo, groupSize int) int {
	for j := 0; j < 4; j++ {
		dst := groupSize * j
		choff := 576
		for i := 0; i < 2*sci.totalBands; i++ {
			ba := int(sci.bitalloc[i])
			if ba != 0 {
				if ba < 17 {
					half := int32(1)<<uint(ba-1) - 1
					for k := 0; k < groupSize; k++ {
						grbuf[dst+k] = float32(int32(bs.get(ba)) - half)
					}
				} else {
					mod := uint32(2<<uint(ba-17)) + 1
					code := bs.get(int(mod + 2 - mod>>3))
					for k := 0; k < groupSize; k++ {
						grbuf[dst+k] = float32(int32(code%mod) - int32(mod/2))
						code /= mod
					}
				}
			}
			dst += choff
			choff = 18 - choff
		}
	}
	return groupSize * 4
}

func l12ApplyScf384(sci *l12ScaleInfo, scf []float32, dst []float32) {
	sb := sci.stereoBands * 18
	n := (sci.totalBands - sci.stereoBands) * 18
	copy(dst[576+sb:576+sb+n], dst[sb:sb+n])
	for i := 0; i < sci.totalBands; i++ {
		d := dst[i*18:]
		s := scf[i*6:]
		for k := 0; k < 12; k++ {
			d[k] *= s[0]
			d[k+576] *= s[3]
		}
	}
}

var dctSec = [24]float32{
	10.19000816, 0.50060302, 0.50241929, 3.40760851, 0.50547093, 0.52249861, 2.05778098, 0.51544732, 0.56694406, 1.48416460, 0.53104258, 0.64682180, 1.16943991, 0.55310392, 0.78815460, 0.97256821, 0.58293498, 1.06067765, 0.83934963, 0.62250412, 1.72244716, 0.74453628, 0.67480832, 5.10114861,
}

func mpegDCTII(grbuf []float32, n int) {
	for k := 0; k < n; k++ {
		var t [32]float32
		y := grbuf[k:]
		for i := 0; i < 8; i++ {
			x0 := y[i*18]
			x1 := y[(15-i)*18]
			x2 := y[(16+i)*18]
			x3 := y[(31-i)*18]
			t0 := x0 + x3
			t1 := x1 + x2
			t2 := (x1 - x2) * dctSec[3*i+0]
			t3 := (x0 - x3) * dctSec[3*i+1]
			t[i] = t0 + t1
			t[i+8] = (t0 - t1) * dctSec[3*i+2]
			t[i+16] = t3 + t2
			t[i+24] = (t3 - t2) * dctSec[3*i+2]
		}
		for i := 0; i < 4; i++ {
			x := t[i*8 : i*8+8]
			x0, x1, x2, x3, x4, x5, x6, x7 := x[0], x[1], x[2], x[3], x[4], x[5], x[6], x[7]
			xt := x0 - x7
			x0 += x7
			x7 = x1 - x6
			x1 += x6
			x6 = x2 - x5
			x2 += x5
			x5 = x3 - x4
			x3 += x4
			x4 = x0 - x3
			x0 += x3
			x3 = x1 - x2
			x1 += x2
			x[0] = x0 + x1
			x[4] = (x0 - x1) * 0.70710677
			x5 = x5 + x6
			x6 = (x6 + x7) * 0.70710677
			x7 = x7 + xt
			x3 = (x3 + x4) * 0.70710677
			x5 -= x7 * 0.198912367
			x7 += x5 * 0.382683432
			x5 -= x7 * 0.198912367
			x0 = xt - x6
			xt += x6
			x[1] = (xt + x7) * 0.50979561
			x[2] = (x4 + x3) * 0.54119611
			x[3] = (x0 - x5) * 0.60134488
			x[5] = (x0 + x5) * 0.89997619
			x[6] = (x4 - x3) * 1.30656302
			x[7] = (xt - x7) * 2.56291556
		}
		yi := 0
		for i := 0; i < 7; i++ {
			y[yi+0*18] = t[i]
			y[yi+1*18] = t[16+i] + t[24+i] + t[24+i+1]
			y[yi+2*18] = t[8+i] + t[8+i+1]
			y[yi+3*18] = t[16+i+1] + t[24+i] + t[24+i+1]
			yi += 4 * 18
		}
		y[yi+0*18] = t[7]
		y[yi+1*18] = t[16+7] + t[24+7]
		y[yi+2*18] = t[8+7]
		y[yi+3*18] = t[24+7]
	}
}

func mpegScalePCM(sample float32) int16 {
	if sample >= 32766.5 {
		return 32767
	}
	if sample <= -32767.5 {
		return -32768
	}
	s := int16(sample + 0.5)
	if s < 0 {
		s--
	}
	return s
}

func mpegSynthPair(pcm []int16, nch int, z []float32) {
	var a float32
	a = (z[14*64] - z[0]) * 29
	a += (z[1*64] + z[13*64]) * 213
	a += (z[12*64] - z[2*64]) * 459
	a += (z[3*64] + z[11*64]) * 2037
	a += (z[10*64] - z[4*64]) * 5153
	a += (z[5*64] + z[9*64]) * 6574
	a += (z[8*64] - z[6*64]) * 37489
	a += z[7*64] * 75038
	pcm[0] = mpegScalePCM(a)

	z = z[2:]
	a = z[14*64] * 104
	a += z[12*64] * 1567
	a += z[10*64] * 9727
	a += z[8*64] * 64019
	a += z[6*64] * -9975
	a += z[4*64] * -45
	a += z[2*64] * 146
	a += z[0*64] * -5
	pcm[16*nch] = mpegScalePCM(a)
}

var mpegWin = [15 * 16]float32{
	-1, 26, -31, 208, 218, 401, -519, 2063, 2000, 4788, -5517, 7134, 5959, 35640, -39336, 74992,
	-1, 24, -35, 202, 222, 347, -581, 2080, 1952, 4425, -5879, 7640, 5288, 33791, -41176, 74856,
	-1, 21, -38, 196, 225, 294, -645, 2087, 1893, 4063, -6237, 8092, 4561, 31947, -43006, 74630,
	-1, 19, -41, 190, 227, 244, -711, 2085, 1822, 3705, -6589, 8492, 3776, 30112, -44821, 74313,
	-1, 17, -45, 183, 228, 197, -779, 2075, 1739, 3351, -6935, 8840, 2935, 28289, -46617, 73908,
	-1, 16, -49, 176, 228, 153, -848, 2057, 1644, 3004, -7271, 9139, 2037, 26482, -48390, 73415,
	-2, 14, -53, 169, 227, 111, -919, 2032, 1535, 2663, -7597, 9389, 1082, 24694, -50137, 72835,
	-2, 13, -58, 161, 224, 72, -991, 2001, 1414, 2330, -7910, 9592, 70, 22929, -51853, 72169,
	-2, 11, -63, 154, 221, 36, -1064, 1962, 1280, 2006, -8209, 9750, -998, 21189, -53534, 71420,
	-2, 10, -68, 147, 215, 2, -1137, 1919, 1131, 1692, -8491, 9863, -2122, 19478, -55178, 70590,
	-3, 9, -73, 139, 208, -29, -1210, 1870, 970, 1388, -8755, 9935, -3300, 17799, -56778, 69679,
	-3, 8, -79, 132, 200, -57, -1283, 1817, 794, 1095, -8998, 9966, -4533, 16155, -58333, 68692,
	-4, 7, -85, 125, 189, -83, -1356, 1759, 605, 814, -9219, 9959, -5818, 14548, -59838, 67629,
	-4, 7, -91, 117, 177, -106, -1428, 1698, 402, 545, -9416, 9916, -7154, 12980, -61289, 66494,
	-5, 6, -97, 111, 163, -127, -1498, 1634, 185, 288, -9585, 9838, -8540, 11455, -62684, 65290,
}

// mpegSynth: xl is the offset of the left samples in grbuf, pcm the offset of
// the output, lins the offset in syn.
func mpegSynth(grbuf []float32, xl int, pcm []int16, po int, nch int, syn []float32, lins int) {
	xr := xl + 576*(nch-1)
	dstl := po
	dstr := po + nch - 1
	zlin := lins + 15*64
	w := 0

	syn[zlin+4*15] = grbuf[xl+18*16]
	syn[zlin+4*15+1] = grbuf[xr+18*16]
	syn[zlin+4*15+2] = grbuf[xl]
	syn[zlin+4*15+3] = grbuf[xr]

	syn[zlin+4*31] = grbuf[xl+1+18*16]
	syn[zlin+4*31+1] = grbuf[xr+1+18*16]
	syn[zlin+4*31+2] = grbuf[xl+1]
	syn[zlin+4*31+3] = grbuf[xr+1]

	mpegSynthPair(pcm[dstr:], nch, syn[lins+4*15+1:])
	mpegSynthPair(pcm[dstr+32*nch:], nch, syn[lins+4*15+64+1:])
	mpegSynthPair(pcm[dstl:], nch, syn[lins+4*15:])
	mpegSynthPair(pcm[dstl+32*nch:], nch, syn[lins+4*15+64:])

	for i := 14; i >= 0; i-- {
		var a, b [4]float32
		syn[zlin+4*i] = grbuf[xl+18*(31-i)]
		syn[zlin+4*i+1] = grbuf[xr+18*(31-i)]
		syn[zlin+4*i+2] = grbuf[xl+1+18*(31-i)]
		syn[zlin+4*i+3] = grbuf[xr+1+18*(31-i)]
		syn[zlin+4*(i+16)] = grbuf[xl+1+18*(1+i)]
		syn[zlin+4*(i+16)+1] = grbuf[xr+1+18*(1+i)]
		syn[zlin+4*(i-16)+2] = grbuf[xl+18*(1+i)]
		syn[zlin+4*(i-16)+3] = grbuf[xr+18*(1+i)]

		for k := 0; k < 8; k++ {
			w0, w1 := mpegWin[w], mpegWin[w+1]
			w += 2
			vz := syn[zlin+4*i-k*64:]
			vy := syn[zlin+4*i-(15-k)*64:]
			for j := 0; j < 4; j++ {
				bb := vz[j]*w1 + vy[j]*w0
				switch {
				case k == 0:
					b[j] = bb
					a[j] = vz[j]*w0 - vy[j]*w1
				case k&1 == 1:
					b[j] += bb
					a[j] += vy[j]*w1 - vz[j]*w0
				default:
					b[j] += bb
					a[j] += vz[j]*w0 - vy[j]*w1
				}
			}
		}

		pcm[dstr+(15-i)*nch] = mpegScalePCM(a[1])
		pcm[dstr+(17+i)*nch] = mpegScalePCM(b[1])
		pcm[dstl+(15-i)*nch] = mpegScalePCM(a[0])
		pcm[dstl+(17+i)*nch] = mpegScalePCM(b[0])
		pcm[dstr+(47-i)*nch] = mpegScalePCM(a[3])
		pcm[dstr+(49+i)*nch] = mpegScalePCM(b[3])
		pcm[dstl+(47-i)*nch] = mpegScalePCM(a[2])
		pcm[dstl+(49+i)*nch] = mpegScalePCM(b[2])
	}
}

type mpegDecoder struct {
	qmfState        [15 * 2 * 32]float32
	freeFormatBytes int
	header          [4]byte

	grbuf [2 * 576]float32
	syn   [(18 + 15) * 2 * 32]float32
	sci   l12ScaleInfo
}

func (d *mpegDecoder) init() { d.header[0] = 0 }

func (d *mpegDecoder) synthGranule(nbands, nch int, pcm []int16, po int) {
	for i := 0; i < nch; i++ {
		mpegDCTII(d.grbuf[576*i:], nbands)
	}
	copy(d.syn[:15*64], d.qmfState[:15*64])
	for i := 0; i < nbands; i += 2 {
		mpegSynth(d.grbuf[:], i, pcm, po+32*nch*i, nch, d.syn[:], i*64)
	}
	if nch == 1 {
		for i := 0; i < 15*64; i += 2 {
			d.qmfState[i] = d.syn[nbands*64+i]
		}
	} else {
		copy(d.qmfState[:15*64], d.syn[nbands*64:nbands*64+15*64])
	}
}

func hdrIsNullOrTag(h []byte) bool {
	return (h[0] == 'T' && h[1] == 'A' && h[2] == 'G' && h[3] == 0) || (h[0] == 0 && h[1] == 0 && h[2] == 0 && h[3] == 0)
}

func mpegMatchFrame(hdr []byte, frameBytes int) bool {
	i := 0
	for nmatch := 0; nmatch < mpegMaxSyncMatches; nmatch++ {
		i += hdrFrameBytes(hdr[i:], frameBytes) + hdrPadding(hdr[i:])
		if i+mpegHdrSize > len(hdr) {
			return nmatch > 0
		}
		if hdrIsNullOrTag(hdr[i:]) {
			return nmatch > 0
		}
		if !hdrCompare(hdr, hdr[i:]) {
			return false
		}
	}
	return true
}

func mpegFindFrame(mp3 []byte, freeFormatBytes *int) (offset, frameBytes int) {
	n := len(mp3)
	for i := 0; i < n-mpegHdrSize; i++ {
		h := mp3[i:]
		if !hdrValid(h) {
			continue
		}
		fb := hdrFrameBytes(h, *freeFormatBytes)
		fap := fb + hdrPadding(h)
		for k := mpegHdrSize; fb == 0 && k < mpegMaxFreeFormat && i+2*k < n-mpegHdrSize; k++ {
			if hdrCompare(h, h[k:]) {
				f := k - hdrPadding(h)
				nextfb := f + hdrPadding(h[k:])
				if i+k+nextfb+mpegHdrSize > n || !hdrCompare(h, h[k+nextfb:]) {
					continue
				}
				fap = k
				fb = f
				*freeFormatBytes = f
			}
		}
		if (fb != 0 && i+fap <= n && mpegMatchFrame(h, fb)) || (i == 0 && fap == n) {
			return i, fap
		}
		*freeFormatBytes = 0
	}
	return n, 0
}

// decodeFrame decodes the frame at the start of mp3 (after skipping to the
// next good one) into pcm. It returns the samples per channel made, the
// bytes used and the channels.
func (d *mpegDecoder) decodeFrame(mp3 []byte, pcm []int16) (samples, frameBytes, channels int) {
	i, frameSize := 0, 0
	n := len(mp3)
	if n > 4 && d.header[0] == 0xff && hdrCompare(d.header[:], mp3) {
		frameSize = hdrFrameBytes(mp3, d.freeFormatBytes) + hdrPadding(mp3)
		if frameSize != n && (frameSize+mpegHdrSize > n || !hdrCompare(mp3, mp3[frameSize:])) {
			frameSize = 0
		}
	}
	if frameSize == 0 {
		d.qmfState = [15 * 2 * 32]float32{}
		d.freeFormatBytes = 0
		d.header = [4]byte{}
		i, frameSize = mpegFindFrame(mp3, &d.freeFormatBytes)
		if frameSize == 0 || i+frameSize > n {
			return 0, i, 0
		}
	}
	hdr := mp3[i:]
	copy(d.header[:], hdr[:4])
	frameBytes = i + frameSize
	channels = 2
	if hdrIsMono(hdr) {
		channels = 1
	}
	layer := 4 - hdrLayer(hdr)
	if layer == 3 {
		return 0, frameBytes, channels // Layer III isn't used by the music boards
	}
	bs := &mpegBits{buf: hdr[mpegHdrSize:frameSize], limit: (frameSize - mpegHdrSize) * 8}
	if hdrIsCRC(hdr) {
		bs.get(16)
	}
	sci := &d.sci
	l12ReadScaleInfo(hdr, bs, sci)
	d.grbuf = [2 * 576]float32{}
	po := 0
	gi := 0
	for igr := 0; igr < 3; igr++ {
		gi += l12DequantizeGranule(d.grbuf[gi:], bs, sci, layer|1)
		if gi == 12 {
			gi = 0
			l12ApplyScf384(sci, sci.scf[igr:], d.grbuf[:])
			d.synthGranule(12, channels, pcm, po)
			d.grbuf = [2 * 576]float32{}
			po += 384 * channels
		}
		if bs.pos > bs.limit {
			d.init()
			return 0, frameBytes, channels
		}
	}
	return hdrFrameSamples(d.header[:]), frameBytes, channels
}
