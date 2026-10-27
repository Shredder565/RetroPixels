package model3

import (
	"encoding/binary"
	"math"
)

// The Real3D Pro-1000 video board as the processor sees it: its memories
// (culling RAM, polygon RAM, texture RAM), the texture FIFO and VROM texture
// port, the DMA device, the status register and the ping-pong timing that
// says when the game may start writing the next frame. The drawing itself is
// in render.go.
//
// The board is little endian: every 32-bit word the processor writes is
// stored byte-reversed, which is the value the chips work with.

const (
	cullLoWords = 0x400000 / 4
	cullHiWords = 0x100000 / 4
	polyWords   = 0x400000 / 4
	texW        = 2048
	texH        = 2048
	fifoWords   = 0x100000 / 4
)

type pendingWrite struct {
	hi   bool // high culling RAM (else polygon RAM)
	addr uint32
	val  uint32
}

type real3D struct {
	step int

	cullLo  []uint32
	cullHi  []uint32
	poly    []uint32
	tex     []uint16 // 2048x2048 texels (two 2048x1024 pages)
	fifo    []uint32
	fifoIdx int
	vrom    []byte // the VROM as loaded (read as little-endian words)

	vromFIFO    [2]uint32
	vromFIFOIdx int

	// DMA device
	dmaSrc, dmaDst, dmaLen uint32
	dmaData                uint32
	dmaStatus, dmaConfig   uint8

	config [4]uint32

	pingPong       bool
	pingPongCopy   bool
	commandWritten bool
	tilegenDraw    bool
	pending        []pendingWrite

	modeword [6]uint32

	// frameReady: a frame was triggered and should be drawn at the end of
	// this frame (DrawFrame)
	frameReady   bool
	blockCulling bool

	texDirty bool

	los [4]float32 // line of sight results of the last frame drawn
}

func newReal3D(step int, vrom []byte) *real3D {
	r := &real3D{step: step, vrom: vrom}
	r.cullLo = make([]uint32, cullLoWords)
	r.cullHi = make([]uint32, cullHiWords)
	r.poly = make([]uint32, polyWords)
	r.tex = make([]uint16, texW*texH)
	r.fifo = make([]uint32, fifoWords)
	return r
}

func (r *real3D) reset() {
	clear(r.cullLo)
	clear(r.cullHi)
	clear(r.poly)
	clear(r.tex)
	clear(r.fifo)
	r.fifoIdx = 0
	r.vromFIFOIdx = 0
	r.dmaSrc, r.dmaDst, r.dmaLen, r.dmaData = 0, 0, 0, 0
	r.dmaStatus, r.dmaConfig = 0, 0
	r.config = [4]uint32{}
	r.pingPong, r.pingPongCopy = false, false
	r.commandWritten, r.tilegenDraw = false, false
	r.pending = r.pending[:0]
	r.modeword = [6]uint32{}
	r.frameReady = false
	r.blockCulling = false
	r.texDirty = true
}

func (r *real3D) pciID() uint32 {
	if r.step >= 0x20 {
		return 0x178611DB
	}
	return 0x16C311DB
}

func (r *real3D) flipped() bool { return r.pingPong != r.pingPongCopy }

// ---------------------------------------------------------------- frame timing

func (r *real3D) beginVBlank() {
	r.pingPongCopy = r.pingPong
	for _, w := range r.pending {
		if w.hi {
			r.cullHi[w.addr] = w.val
		} else {
			r.poly[w.addr] = w.val
		}
	}
	r.pending = r.pending[:0]
	if r.commandWritten {
		r.flushTextures()
	}
	if r.tilegenDraw && r.commandWritten {
		r.drawFrame()
	}
}

func (r *real3D) flipPingPong() {
	r.pingPong = !r.pingPong
	r.tilegenDraw = false
	r.commandWritten = false
}

func (r *real3D) drawFrame() {
	r.tilegenDraw = false
	r.commandWritten = false
	r.blockCulling = r.modeword[1]&4 != 0
	r.frameReady = true
}

// tilegenDrawFrame: the game wrote tile generator register 0x0C.
func (r *real3D) tilegenDrawFrame() {
	r.tilegenDraw = true
	if !r.flipped() && r.commandWritten {
		r.drawFrame()
	}
}

// flush: the game wrote 0x88000000 (the command port).
func (r *real3D) flush() {
	r.commandWritten = true
	if !r.flipped() {
		r.flushTextures()
		if r.tilegenDraw {
			r.drawFrame()
		}
	}
}

// ---------------------------------------------------------------- memories

func (r *real3D) writeCullLo(a, v uint32) { r.cullLo[(a&0x3FFFFF)/4] = v }

func (r *real3D) writeCullHi(a, v uint32) {
	a = (a & 0xFFFFF) / 4
	if r.flipped() {
		r.pending = append(r.pending, pendingWrite{true, a, v})
		return
	}
	r.cullHi[a] = v
}

func (r *real3D) writePoly(a, v uint32) {
	a = (a & 0x3FFFFF) / 4
	if r.flipped() {
		r.pending = append(r.pending, pendingWrite{false, a, v})
		return
	}
	r.poly[a] = v
}

func (r *real3D) writeFIFO(v uint32) {
	if r.fifoIdx < fifoWords {
		r.fifo[r.fifoIdx] = v
		r.fifoIdx++
	}
}

func (r *real3D) vromWord(i uint32) uint32 {
	o := (i * 4) % uint32(len(r.vrom))
	return binary.LittleEndian.Uint32(r.vrom[o:])
}

func (r *real3D) writeTexturePort(v uint32) {
	if r.step == 0x10 {
		addr := v & 0xFFFFFF
		n := (2 + r.vromWord(addr)/2) / 4
		for i := uint32(0); i < n; i++ {
			r.writeFIFO(r.vromWord((addr + i) & 0xFFFFFF))
		}
		return
	}
	if r.vromFIFOIdx == 2 {
		addr, header := r.vromFIFO[0], r.vromFIFO[1]
		r.vromFIFOIdx = 0
		if addr < 0x100000 {
			r.uploadTexture(header, &wordTexels{r.poly, addr & 0xFFFFFF})
		} else {
			r.uploadTexture(header, &vromTexels{r, addr & 0xFFFFFF})
		}
		return
	}
	r.vromFIFO[r.vromFIFOIdx] = v
	r.vromFIFOIdx++
}

// texels: a stream of 16-bit texels (the low half of each little-endian word first)
type texels interface {
	at(i uint32) uint16
}

type wordTexels struct {
	w    []uint32
	base uint32 // word index
}

func (t *wordTexels) at(i uint32) uint16 {
	k := t.base + i/2
	if int(k) >= len(t.w) {
		return 0
	}
	return uint16(t.w[k] >> (16 * (i & 1)))
}

type vromTexels struct {
	r    *real3D
	base uint32
}

func (t *vromTexels) at(i uint32) uint16 {
	o := (uint64(t.base)*4 + uint64(i)*2) % uint64(len(t.r.vrom))
	return binary.LittleEndian.Uint16(t.r.vrom[o:])
}

func (r *real3D) flushTextures() {
	if r.fifoIdx > 2 {
		for i := 0; i < r.fifoIdx-2; {
			size := (2 + r.fifo[i]/2) / 4
			header := r.fifo[i+1]
			if size == 0 {
				break
			}
			r.uploadTexture(header, &wordTexels{r.fifo, uint32(i + 2)})
			i += int(size)
		}
	}
	r.fifoIdx = 0
}

// ---------------------------------------------------------------- textures

var (
	mipXBase = [...]uint32{0, 1024, 1536, 1792, 1920, 1984, 2016, 2032, 2040, 2044, 2046, 2047}
	mipYBase = [...]uint32{0, 512, 768, 896, 960, 992, 1008, 1016, 1020, 1022, 1023}
)

// the order of the texels of an 8x8, 8x4 or 8x2 tile in the data
var (
	tile8x8 = [64]uint32{
		1, 0, 5, 4, 9, 8, 13, 12,
		3, 2, 7, 6, 11, 10, 15, 14,
		17, 16, 21, 20, 25, 24, 29, 28,
		19, 18, 23, 22, 27, 26, 31, 30,
		33, 32, 37, 36, 41, 40, 45, 44,
		35, 34, 39, 38, 43, 42, 47, 46,
		49, 48, 53, 52, 57, 56, 61, 60,
		51, 50, 55, 54, 59, 58, 63, 62}
	tile8x4 = [32]uint32{1, 0, 5, 4, 3, 2, 7, 6, 9, 8, 13, 12, 11, 10, 15, 14,
		17, 16, 21, 20, 19, 18, 23, 22, 25, 24, 29, 28, 27, 26, 31, 30}
	tile8x2 = [16]uint32{1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14}
)

// storeTexture writes one mip level's texels; it returns how many 16-bit
// words of data it used.
func (r *real3D) storeTexture(x0, y0, w, h uint32, data texels, off uint32, sixteen, lsb, msb bool) uint32 {
	tx, ty := min(8, w), min(8, h)
	var dec []uint32
	switch tx {
	case 8:
		dec = tile8x8[:]
	case 4:
		dec = tile8x4[:]
	case 2:
		dec = tile8x2[:]
	default:
		return 0
	}
	used := uint32(0)
	if sixteen {
		for y := y0; y < y0+h; y += ty {
			for x := x0; x < x0+w; x += tx {
				d := y*texW + x
				for yy := uint32(0); yy < ty; yy++ {
					for xx := uint32(0); xx < tx; xx++ {
						if d < uint32(len(r.tex)) {
							r.tex[d] = data.at(off + dec[yy*tx+xx])
						}
						d++
						used++
					}
					d += texW - tx
				}
				off += ty * tx
			}
		}
		return used
	}
	sel := 0
	if lsb {
		sel |= 1
	}
	if msb {
		sel |= 2
	}
	keep := [4]uint16{0xFFFF, 0xFF00, 0x00FF, 0x0000}[sel]
	step := max(1, ty*tx/2)
	for y := y0; y < y0+h; y += ty {
		for x := x0; x < x0+w; x += tx {
			d := y*texW + x
			for yy := uint32(0); yy < ty; yy++ {
				for xx := uint32(0); xx < tx; xx++ {
					if sel != 0 && d < uint32(len(r.tex)) {
						shift := 8 * ((xx & 1) ^ 1)
						idx := (yy^1)*tx + (xx ^ 1) - (tx & 1)
						v := data.at(off+dec[idx]/2) >> shift & 0xFF
						v |= v << 8
						v &= keep ^ 0xFFFF
						r.tex[d] = r.tex[d]&keep | v
					}
					d++
				}
				d += texW - tx
			}
			off += step
			used += step
		}
	}
	return used
}

func (r *real3D) uploadTexture(header uint32, data texels) {
	x := 32 * (header & 0x3F)
	y := 32 * (header >> 7 & 0x1F)
	page := header >> 20 & 1
	w := uint32(32) << (header >> 14 & 7)
	h := uint32(32) << (header >> 17 & 7)
	typ := header >> 24 & 0xFF
	sixteen := header>>23&1 != 0
	msb := header>>22&1 != 0
	lsb := header>>21&1 != 0
	off := uint32(0)
	r.texDirty = true
	switch typ {
	case 0, 1, 2:
		if typ != 2 {
			off += r.storeTexture(x, y+page*1024, w, h, data, off, sixteen, lsb, msb)
			if typ == 1 {
				return
			}
		}
		for i := 1; w >= 4 && h >= 4 && i < len(mipYBase); i++ {
			div := uint32(1) << i
			xp := mipXBase[i] + x/div
			yp := mipYBase[i] + y/div
			w /= 2
			h /= 2
			off += r.storeTexture(xp, yp+page*1024, w, h, data, off, sixteen, lsb, msb)
		}
	}
}

// ---------------------------------------------------------------- registers and DMA

// status register (the chip's view; the processor sees it byte-reversed)
func (r *real3D) readReg(reg uint32) uint32 {
	if reg == 0 {
		v := uint32(0xFDFFFFFF)
		if r.pingPong {
			v |= 0x02000000
		}
		return v
	}
	if reg >= 20 && reg <= 32 {
		return math.Float32bits(r.los[(reg-20)/4])
	}
	return 0xFFFFFFFF
}
