package wiiu

// Latte surfaces: how the GPU lays out pictures (colour buffers, depth
// buffers, textures) in memory.
//
// The Wii U's GPU is an R700-family Radeon. Its surfaces are linear or
// tiled: 1D tiling arranges pixels in 8x8 "micro tiles"; 2D tiling also
// spreads micro tiles across the memory's 2 pipes and 4 banks. Game files
// keep their textures in the tiled layout, so this has to match the
// hardware's rules exactly (the same rules as AMD's public address library
// for R600-class chips, with the Wii U's 2 pipes, 4 banks and 256-byte
// pipe interleave).

const (
	numPipes        = 2
	numBanks        = 4
	pipeInterleave  = 256 // bytes
	rowSizeBytes    = 2048
	swapSizeBytes   = 256
	splitSizeBytes  = 2048
	microTilePixels = 64
)

// tile modes
const (
	tileLinearGeneral = 0
	tileLinearAligned = 1
	tile1DThin1       = 2
	tile1DThick       = 3
	tile2DThin1       = 4
	tile2DThin2       = 5
	tile2DThin4       = 6
	tile2DThick       = 7
	tile2BThin1       = 8
	tile2BThin2       = 9
	tile2BThin4       = 10
	tile2BThick       = 11
	tile3DThin1       = 12
	tile3DThick       = 13
	tile3BThin1       = 14
	tile3BThick       = 15
	tileLinearSpecial = 16
)

func tileThickness(mode uint32) uint32 {
	switch mode {
	case tile1DThick, tile2DThick, tile2BThick, tile3DThick, tile3BThick:
		return 4
	}
	return 1
}

func isMacroTiled(mode uint32) bool { return mode >= tile2DThin1 && mode <= tile3BThick }

func isThickMacro(mode uint32) bool {
	return mode == tile2DThick || mode == tile2BThick || mode == tile3DThick || mode == tile3BThick
}

func isBankSwapped(mode uint32) bool {
	switch mode {
	case tile2BThin1, tile2BThin2, tile2BThin4, tile2BThick, tile3BThin1, tile3BThick:
		return true
	}
	return false
}

func macroAspect(mode uint32) uint32 {
	switch mode {
	case tile2DThin2, tile2BThin2:
		return 2
	case tile2DThin4, tile2BThin4:
		return 4
	}
	return 1
}

func surfaceRotation(mode uint32) uint32 {
	switch mode {
	case tile2DThin1, tile2DThin2, tile2DThin4, tile2DThick, tile2BThin1, tile2BThin2, tile2BThin4, tile2BThick:
		return numPipes * ((numBanks >> 1) - 1)
	case tile3DThin1, tile3DThick, tile3BThin1, tile3BThick:
		return 1 // with fewer than 4 pipes
	}
	return 0
}

// pixelIndexInMicroTile: where pixel (x, y, z) of an 8x8 micro tile goes
// (bpp in bits).
func pixelIndexInMicroTile(x, y, z, bpp, mode uint32, depth bool) uint32 {
	var b0, b1, b2, b3, b4, b5, b6, b7, b8 uint32
	thick := tileThickness(mode)
	if depth {
		b0, b1, b2, b3, b4, b5 = x&1, y&1, x>>1&1, y>>1&1, x>>2&1, y>>2&1
	} else {
		switch bpp {
		case 8:
			b0, b1, b2, b3, b4, b5 = x&1, x>>1&1, x>>2&1, y>>1&1, y&1, y>>2&1
		case 16:
			b0, b1, b2, b3, b4, b5 = x&1, x>>1&1, x>>2&1, y&1, y>>1&1, y>>2&1
		case 64:
			b0, b1, b2, b3, b4, b5 = x&1, y&1, x>>1&1, x>>2&1, y>>1&1, y>>2&1
		case 128:
			b0, b1, b2, b3, b4, b5 = y&1, x&1, x>>1&1, x>>2&1, y>>1&1, y>>2&1
		default: // 32 and 96
			b0, b1, b2, b3, b4, b5 = x&1, x>>1&1, y&1, x>>2&1, y>>1&1, y>>2&1
		}
	}
	if thick > 1 {
		b6, b7 = z&1, z>>1&1
	}
	if thick == 8 {
		b8 = z >> 2 & 1
	}
	return b8<<8 | b7<<7 | b6<<6 | b5<<5 | b4<<4 | b3<<3 | b2<<2 | b1<<1 | b0
}

func pipeFromCoord(x, y uint32) uint32 { return (y>>3 ^ x>>3) & 1 }

func bankFromCoord(x, y uint32) uint32 {
	b0 := (y/(16*numPipes) ^ x>>3) & 1
	b1 := (y/(8*numPipes) ^ x>>4) & 1
	return b0 | b1<<1
}

var bankSwapOrder = [4]uint32{0, 1, 3, 2}

func bankSwappedWidth(mode, bpp, samples, pitch uint32) uint32 {
	if !isBankSwapped(mode) {
		return 0
	}
	bytesPerSample := 8 * bpp
	samplesPerTile := splitSizeBytes / max(bytesPerSample, 1)
	slicesPerTile := max(1, samples/max(samplesPerTile, 1))
	if isThickMacro(mode) {
		samples = 4
	}
	bytesPerTileSlice := samples * bytesPerSample / slicesPerTile
	factor := macroAspect(mode)
	swapTiles := max(1, (swapSizeBytes>>1)/bpp)
	swapWidth := swapTiles * 8 * numBanks
	heightBytes := samples * factor * numPipes * bpp / slicesPerTile
	swapMax := numPipes * numBanks * rowSizeBytes / max(heightBytes, 1)
	swapMin := pipeInterleave * 8 * numBanks / max(bytesPerTileSlice, 1)
	w := min(swapMax, max(swapMin, swapWidth))
	for w >= 2*pitch && w > 1 {
		w >>= 1
	}
	return w
}

// surfLayout is one mip level of a surface, as addressed.
type surfLayout struct {
	Base    uint32 // address of the level
	Pitch   uint32 // in elements (blocks for compressed formats)
	Height  uint32 // in elements, aligned
	Slices  uint32
	BPP     uint32 // bits per element
	Mode    uint32 // tile mode
	Samples uint32
	Depth   bool
	Swizzle uint32 // the surface's swizzle (bits 8-10: pipe and bank)
}

// addr returns the byte address of element (x, y) of slice z.
func (s *surfLayout) addr(x, y, z uint32) uint32 {
	return s.Base + s.offset(x, y, z, 0)
}

func (s *surfLayout) offset(x, y, z, sample uint32) uint32 {
	bpp := s.BPP
	switch s.Mode {
	case tileLinearGeneral, tileLinearAligned, tileLinearSpecial:
		sliceOff := s.Pitch * s.Height * (z + sample*s.Slices)
		return (y*s.Pitch + x + sliceOff) * bpp / 8
	case tile1DThin1, tile1DThick:
		thick := tileThickness(s.Mode)
		microBytes := (microTilePixels*thick*bpp + 7) / 8
		perRow := s.Pitch >> 3
		microOff := microBytes * (x>>3 + (y>>3)*perRow)
		sliceBytes := (s.Pitch*s.Height*thick*bpp + 7) / 8
		sliceOff := sliceBytes * (z / thick)
		pix := pixelIndexInMicroTile(x, y, z, bpp, s.Mode, s.Depth)
		return (bpp*pix)>>3 + microOff + sliceOff
	}
	return s.macroOffset(x, y, z, sample)
}

func (s *surfLayout) macroOffset(x, y, slice, sample uint32) uint32 {
	bpp, mode := s.BPP, s.Mode
	samples := max(s.Samples, 1)
	const groupBits, pipeBits, bankBits = 8, 1, 2
	thick := tileThickness(mode)
	microBits := samples * bpp * thick * 64
	microBytes := microBits >> 3
	pix := pixelIndexInMicroTile(x, y, slice, bpp, mode, s.Depth)
	var sampleOff, pixOff uint32
	if s.Depth {
		sampleOff, pixOff = bpp*sample, samples*bpp*pix
	} else {
		sampleOff, pixOff = sample*(microBits/samples), bpp*pix
	}
	elem := pixOff + sampleOff
	bytesPerSample := microBytes / samples
	var splits, sampleSlice uint32 = 1, 0
	if samples > 1 && microBytes > splitSizeBytes {
		perSlice := splitSizeBytes / bytesPerSample
		splits = samples / perSlice
		samples = perSlice
		tileSliceBits := microBits / splits
		sampleSlice = elem / tileSliceBits
		elem %= tileSliceBits
	}
	elem = (elem + 7) / 8
	pipe := pipeFromCoord(x, y)
	bank := bankFromCoord(x, y)
	bankPipe := pipe + numPipes*bank
	rot := surfaceRotation(mode)
	pipeSw, bankSw := s.Swizzle>>8&1, s.Swizzle>>9&3
	swz := pipeSw + numPipes*bankSw
	sliceIn := slice
	if isThickMacro(mode) {
		sliceIn >>= 2
	}
	bankPipe ^= numPipes*sampleSlice*((numBanks>>1)+1) ^ (swz + sliceIn*rot)
	bankPipe %= numPipes * numBanks
	pipe = bankPipe % numPipes
	bank = bankPipe / numPipes
	sliceBytes := (s.Height*s.Pitch*thick*bpp*samples + 7) / 8
	sliceOff := sliceBytes * ((sampleSlice + splits*slice) / thick)
	mtPitch := uint32(8 * numBanks)
	mtHeight := uint32(8 * numPipes)
	switch macroAspect(mode) {
	case 2:
		mtPitch >>= 1
		mtHeight *= 2
	case 4:
		mtPitch >>= 2
		mtHeight *= 4
	}
	perRow := s.Pitch / mtPitch
	mtBytes := (samples*thick*bpp*mtHeight*mtPitch + 7) / 8
	mx, my := x/mtPitch, y/mtHeight
	mtOff := (mx + perRow*my) * mtBytes
	if isBankSwapped(mode) {
		if w := bankSwappedWidth(mode, bpp, samples, s.Pitch); w != 0 {
			idx := mtPitch * mx / w
			bank ^= bankSwapOrder[idx&(numBanks-1)]
		}
	}
	const groupMask = 1<<groupBits - 1
	total := elem + (mtOff+sliceOff)>>(bankBits+pipeBits)
	hi := (total &^ groupMask) << (bankBits + pipeBits)
	lo := total & groupMask
	return bank<<(pipeBits+groupBits) | pipe<<groupBits | lo | hi
}

// surface formats (the GPU's, in the low 6 bits of a GX2 format)
const (
	fmtInvalid    = 0x00
	fmt8          = 0x01
	fmt4_4        = 0x02
	fmt3_3_2      = 0x03
	fmt16         = 0x05
	fmt16F        = 0x06
	fmt8_8        = 0x07
	fmt5_6_5      = 0x08
	fmt6_5_5      = 0x09
	fmt1_5_5_5    = 0x0a
	fmt4_4_4_4    = 0x0b
	fmt5_5_5_1    = 0x0c
	fmt32         = 0x0d
	fmt32F        = 0x0e
	fmt16_16      = 0x0f
	fmt16_16F     = 0x10
	fmt8_24       = 0x11
	fmt8_24F      = 0x12
	fmt24_8       = 0x13
	fmt24_8F      = 0x14
	fmt10_11_11   = 0x15
	fmt10_11_11F  = 0x16
	fmt11_11_10   = 0x17
	fmt11_11_10F  = 0x18
	fmt2_10_10_10 = 0x19
	fmt8_8_8_8    = 0x1a
	fmt10_10_10_2 = 0x1b
	fmtX24_8_32F  = 0x1c
	fmt32_32      = 0x1d
	fmt32_32F     = 0x1e
	fmt16x4       = 0x1f
	fmt16x4F      = 0x20
	fmt32x4       = 0x22
	fmt32x4F      = 0x23
	fmt1          = 0x25
	fmtGB_GR      = 0x27
	fmtBG_RG      = 0x28
	fmt32_AS_8    = 0x29
	fmt32_AS_8_8  = 0x2a
	fmt5_9_9_9    = 0x2b
	fmt8_8_8      = 0x2c
	fmt16_16_16   = 0x2d
	fmt16_16_16F  = 0x2e
	fmt32_32_32   = 0x2f
	fmt32_32_32F  = 0x30
	fmtBC1        = 0x31
	fmtBC2        = 0x32
	fmtBC3        = 0x33
	fmtBC4        = 0x34
	fmtBC5        = 0x35
)

// formatBits: bits per element (per 4x4 block for compressed formats).
func formatBits(f uint32) uint32 {
	switch f & 0x3f {
	case fmt8, fmt4_4, fmt3_3_2, fmt1:
		return 8
	case fmt16, fmt16F, fmt8_8, fmt5_6_5, fmt6_5_5, fmt1_5_5_5, fmt4_4_4_4, fmt5_5_5_1:
		return 16
	case fmtX24_8_32F, fmt32_32, fmt32_32F, fmt16x4, fmt16x4F, fmtBC1, fmtBC4:
		return 64
	case fmt32_32_32, fmt32_32_32F:
		return 96
	case fmt32x4, fmt32x4F, fmtBC2, fmtBC3, fmtBC5:
		return 128
	case fmt8_8_8:
		return 24
	case fmt16_16_16, fmt16_16_16F:
		return 48
	}
	return 32
}

func formatCompressed(f uint32) bool {
	b := f & 0x3f
	return b >= fmtBC1 && b <= fmtBC5
}

// surfInfo is a surface's whole layout (GX2CalcSurfaceSizeAndAlignment).
type surfInfo struct {
	Pitch, Height, Size, Align uint32
	Mode                       uint32
	Levels                     []surfLevel
}

type surfLevel struct {
	Offset        uint32 // from the start of the level-0 image (levels >= 1: from the mip chain's start)
	Pitch, Height uint32 // in elements
	Width         uint32 // in elements
	Mode          uint32
	Size          uint32
}

func nextPow2(v uint32) uint32 {
	n := uint32(1)
	for n < v {
		n <<= 1
	}
	return n
}

func alignUp(v, a uint32) uint32 {
	if a == 0 {
		return v
	}
	return (v + a - 1) / a * a
}

// levelAlign: pitch, height and base alignment for one level.
func levelAlign(mode, bpp, samples uint32, depth bool) (pitchA, heightA, baseA uint32) {
	thick := tileThickness(mode)
	switch {
	case mode == tileLinearGeneral:
		return 1, 1, 1
	case mode == tileLinearAligned || mode == tileLinearSpecial:
		return max(64, pipeInterleave*8/bpp), 1, pipeInterleave
	case mode == tile1DThin1 || mode == tile1DThick:
		// a row of micro tiles fills at least one pipe interleave
		pa := max(8, pipeInterleave*8/(8*thick*bpp*max(samples, 1)))
		return pa, 8, pipeInterleave
	}
	aspect := macroAspect(mode)
	mtW := 8 * numBanks / aspect
	mtH := aspect * 8 * numPipes
	pa := max(mtW, mtW*(pipeInterleave/bpp/(8*thick)/max(samples, 1)))
	ha := mtH
	mtBytes := max(samples, 1) * ((bpp*mtH*mtW + 7) >> 3)
	ba := max(mtBytes, (max(samples, 1)*ha*bpp*pa+7)>>3)
	ba = max(ba, numPipes*numBanks*pipeInterleave)
	return pa, ha, ba
}

// mipTileMode: small mip levels drop from 2D to 1D tiling.
func mipTileMode(mode, bpp, w, h, samples uint32) uint32 {
	if !isMacroTiled(mode) {
		return mode
	}
	aspect := macroAspect(mode)
	mtW := 8 * numBanks / aspect
	mtH := aspect * 8 * numPipes
	pa, _, _ := levelAlign(mode, bpp, samples, false)
	_ = pa
	if w < mtW || h < mtH {
		if tileThickness(mode) > 1 {
			return tile1DThick
		}
		return tile1DThin1
	}
	return mode
}

// calcSurfInfo works out a surface's layout the way GX2 does.
func calcSurfInfo(dim, w, h, d, mips, format, aa, use, mode uint32) surfInfo {
	bpp := formatBits(format)
	samples := uint32(1) << (aa & 3)
	depth := use&4 != 0 // GX2_SURFACE_USE_DEPTH_BUFFER
	if formatCompressed(format) {
		w, h = (w+3)/4, (h+3)/4
	}
	w, h, d = max(w, 1), max(h, 1), max(d, 1)
	slices := d
	if dim == 0 || dim == 1 { // 1D, 2D
		slices = 1
	}
	if dim == 3 || dim == 4 || dim == 5 { // cube, 1D array, 2D array
		slices = d
	}
	_ = dim
	info := surfInfo{Mode: mode}
	var mipStart, mipOff uint32
	for lvl := uint32(0); lvl < max(mips, 1); lvl++ {
		lw, lh := max(w>>lvl, 1), max(h>>lvl, 1)
		if lvl > 0 {
			lw, lh = nextPow2(lw), nextPow2(lh)
		}
		lmode := mode
		if lvl > 0 {
			lmode = mipTileMode(mode, bpp, lw, lh, samples)
		}
		pa, ha, ba := levelAlign(lmode, bpp, samples, depth)
		pitch := alignUp(lw, pa)
		height := alignUp(lh, ha)
		thick := tileThickness(lmode)
		ls := slices
		if dim == 2 { // 3D
			ls = max(d>>lvl, 1)
			ls = alignUp(ls, thick)
		}
		size := pitch * height * ls * bpp * samples / 8
		size = alignUp(size, ba)
		l := surfLevel{Pitch: pitch, Height: height, Width: lw, Mode: lmode, Size: size}
		if lvl == 0 {
			info.Pitch, info.Height, info.Size, info.Align = pitch, height, size, ba
			l.Offset = 0
		} else {
			mipOff = alignUp(mipOff, ba)
			l.Offset = mipOff
			mipOff += size
		}
		info.Levels = append(info.Levels, l)
	}
	_ = mipStart
	return info
}
