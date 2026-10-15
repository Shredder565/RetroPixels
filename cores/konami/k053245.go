package konami

import (
	"math"
	"sort"

	"retropixels/cores/arcade"
)

// Sprite2 is what a K053245 / K053247 board's sprite callback sees and changes.
// Shadow is 0 (none) or a shadow type (1 for the chip's own shadow bit).
type Sprite2 struct {
	Attr, Color, Pri, Shadow, Code int
}

// jsRound is JavaScript's Math.round (halves go up).
func jsRound(x float64) float64 {
	f := math.Floor(x)
	if x-f >= 0.5 {
		f++
	}
	return f
}

// axisMap: K053245 zoom along one axis (see k053245.js).
func axisMap(n int, z float64, c int, m, mf []int16) (start, length int) {
	scale := 64 / z
	start = c - int(math.Trunc(float64(n*16)*scale/2))
	for k := 0; k < n; k++ {
		a := int(jsRound(float64(k*16) * scale))
		b := int(jsRound(float64((k+1)*16) * scale))
		zw := b - a
		if zw > 0 {
			dx := int(math.Floor(float64(16*65536) / float64(zw)))
			for j := 0; j < zw && a+j < len(m); j++ {
				m[a+j] = int16(k*16 + int(int32(j*dx)>>16))
				mf[a+j] = int16(int32((zw-1-j)*dx) >> 16)
			}
		}
		length = min(b, len(m))
	}
	return
}

// K053245 is the K053245 / K053244 sprite pair (TMNT2) or the K053247 /
// K053246 pair (The Simpsons).
type K053245 struct {
	is247          bool
	count          int
	Ram            []uint8
	Buffer         []uint8
	Regs           [16]uint8
	rom            []byte
	SpriteCallback func(t *Sprite2)
	DX, DY         int
	codeCounters   bool
	zoomStep       bool
	offXSign       int
	pens           []uint8
	tiles          int
	t              Sprite2
	order          []int
	slot           [128]int
	mapX, mapY     [16384]int16
	mapXf, mapYf   [16384]int16
}

// K053245Opts: Type "053245" or "053247"; Layout "planar" or "packed"
// (empty: the type's own).
type K053245Opts struct {
	Type            string
	ROM             []byte
	Layout          string
	SpriteCallback  func(t *Sprite2)
	DX, DY          int
	CodeCounters    bool
	ZoomStep        bool
	SubtractOffsetX bool
}

// NewK053245 makes the chips.
func NewK053245(o K053245Opts) *K053245 {
	k := &K053245{is247: o.Type == "053247", rom: o.ROM, SpriteCallback: o.SpriteCallback, DX: o.DX, DY: o.DY,
		codeCounters: o.CodeCounters, zoomStep: o.ZoomStep, offXSign: 1}
	k.count = 128
	if k.is247 {
		k.count = 256
	}
	k.Ram = make([]uint8, k.count*16)
	k.Buffer = make([]uint8, k.count*16)
	if k.rom == nil {
		k.rom = make([]byte, 128)
	}
	if o.SubtractOffsetX {
		k.offXSign = -1
	}
	layout := o.Layout
	if layout == "" {
		layout = "planar"
		if k.is247 {
			layout = "packed"
		}
	}
	k.pens = DecodeK053245(k.rom, layout)
	k.tiles = len(k.pens) >> 8
	k.order = make([]int, 0, k.count)
	return k
}

// DecodeK053245 turns the sprite ROMs into one byte per pixel, 256 a tile.
func DecodeK053245(rom []byte, layout string) []uint8 {
	n := len(rom) >> 7
	pens := make([]uint8, max(1, n)*256)
	for t := 0; t < n; t++ {
		base, out := t*128, t*256
		if layout == "packed" {
			for y := 0; y < 16; y++ {
				for x := 0; x < 16; x++ {
					word := base + y*8 + (x>>2)*2
					v := int(rom[word]) | int(rom[word+1])<<8
					pens[out+y*16+x] = uint8((v >> (12 - (x&3)*4)) & 15)
				}
			}
			continue
		}
		for q := 0; q < 4; q++ {
			for y := 0; y < 8; y++ {
				o := base + q*32 + y*4
				b0, b1, b2, b3 := rom[o], rom[o+1], rom[o+2], rom[o+3]
				py, px0 := y, 0
				if q&2 != 0 {
					py += 8
				}
				if q&1 != 0 {
					px0 = 8
				}
				for x := 0; x < 8; x++ {
					s := uint(7 - x)
					pens[out+py*16+px0+x] = (b0>>s)&1 | ((b1>>s)&1)<<1 | ((b2>>s)&1)<<2 | ((b3>>s)&1)<<3
				}
			}
		}
	}
	return pens
}

// CounterCode: tile of column tx, row ty when the chip counts in the code's own bits.
func CounterCode(code, tx, ty int) int {
	x := (code & 1) | ((code >> 1) & 2) | ((code >> 2) & 4)
	y := ((code >> 1) & 1) | ((code >> 2) & 2) | ((code >> 3) & 4)
	x, y = (x+tx)&7, (y+ty)&7
	return (code &^ 0x3f) | (x & 1) | ((x & 2) << 1) | ((x & 4) << 2) | ((y & 1) << 1) | ((y & 2) << 2) | ((y & 4) << 3)
}

func (k *K053245) Reset() {
	clear(k.Ram)
	clear(k.Buffer)
	k.Regs = [16]uint8{}
}

func (k *K053245) ReadWord(i int) int {
	i = (i % (k.count * 8)) * 2
	return int(k.Ram[i])<<8 | int(k.Ram[i+1])
}

func (k *K053245) WriteWord(i, v int) {
	i = (i % (k.count * 8)) * 2
	k.Ram[i], k.Ram[i+1] = uint8(v>>8), uint8(v)
}

// Latch copies the list into the buffer that gets drawn.
func (k *K053245) Latch() { copy(k.Buffer, k.Ram) }

func (k *K053245) flipX() bool      { return k.Regs[5]&1 != 0 }
func (k *K053245) flipY() bool      { return k.Regs[5]&2 != 0 }
func (k *K053245) DMAEnabled() bool { return k.Regs[5]&0x10 != 0 }
func (k *K053245) offsetX() int {
	v := int(k.Regs[0])<<8 | int(k.Regs[1])
	return int(int32(uint32(v&0x3ff)<<22) >> 22)
}
func (k *K053245) offsetY() int {
	v := int(k.Regs[2])<<8 | int(k.Regs[3])
	return int(int32(uint32(v&0x3ff)<<22) >> 22)
}

func (k *K053245) RegWrite(r int, v uint8) {
	r &= 15
	if k.is247 {
		r &= 7
	}
	k.Regs[r] = v
	if !k.is247 && r == 6 {
		k.Latch()
	}
}

func (k *K053245) RegRead(r int) uint8 {
	r &= 15
	if !k.is247 {
		if r >= 12 && k.Regs[5]&0x10 != 0 {
			addr := int(k.Regs[10]&0x3f)<<18 | int(k.Regs[9])<<10 | int(k.Regs[8])<<2 | (r & 3)
			return k.ReadRom(addr)
		}
		return 0
	}
	addr := int(k.Regs[6])<<17 | int(k.Regs[7])<<9 | int(k.Regs[4])<<1 | (r & 1)
	return k.ReadRom(addr)
}

func (k *K053245) ReadRom(off int) uint8 {
	n := len(k.rom)
	if n == 0 {
		return 0xff
	}
	return k.rom[uint32(off)%uint32(n)]
}

func s10(v int) int { return int(int32(uint32(v)<<22) >> 22) }

// Render draws the buffer's sprites into a w x h picture.
func (k *K053245) Render(outPens []uint16, outPri, outShadow []uint8, w, h int) {
	b, t := k.Buffer, &k.t
	pens, tiles, is247 := k.pens, k.tiles, k.is247
	ord := k.order[:0]
	if is247 {
		for i := 0; i < k.count; i++ {
			if b[i*16]&0x80 != 0 {
				ord = append(ord, i)
			}
		}
		sort.Slice(ord, func(x, y int) bool {
			p, q := ord[x], ord[y]
			if b[q*16+1] != b[p*16+1] {
				return b[q*16+1] < b[p*16+1]
			}
			return q < p
		})
	} else {
		slot := &k.slot
		for i := range slot {
			slot[i] = -1
		}
		for i := 0; i < k.count; i++ {
			if b[i*16]&0x80 != 0 && slot[b[i*16+1]&0x7f] < 0 {
				slot[b[i*16+1]&0x7f] = i
			}
		}
		for s := 0; s < 128; s++ {
			if slot[s] >= 0 {
				ord = append(ord, slot[s])
			}
		}
	}
	k.order = ord
	counters := k.codeCounters
	flipScreenX, flipScreenY := k.flipX(), k.flipY()
	offX, offY := k.offXSign*k.offsetX(), k.offsetY()
	for _, i := range ord {
		o := i * 16
		w0 := int(b[o])<<8 | int(b[o+1])
		code := int(b[o+2])<<8 | int(b[o+3])
		y := s10(int(b[o+4])<<8 | int(b[o+5]))
		x := s10(int(b[o+6])<<8 | int(b[o+7]))
		zy := (int(b[o+8])<<8 | int(b[o+9])) & 0x3ff
		zx := (int(b[o+10])<<8 | int(b[o+11])) & 0x3ff
		attr := int(b[o+12])<<8 | int(b[o+13])
		if w0&0x4000 != 0 {
			zx = zy
		}
		tw, th := 1<<((w0>>8)&3), 1<<((w0>>10)&3)
		flipX, flipY := w0&0x1000 != 0, w0&0x2000 != 0
		var mirrorX, mirrorY bool
		t.Attr, t.Code, t.Pri = attr, code, 0
		if is247 {
			t.Color = attr & 0xff
			t.Shadow = 0
			if attr&0x1000 != 0 {
				t.Shadow = 1
			}
			mirrorX, mirrorY = attr&0x4000 != 0, attr&0x8000 != 0
		} else {
			t.Color = attr & 0x1f
			t.Shadow = 0
			if attr&0x80 != 0 {
				t.Shadow = 1
			}
			mirrorX, mirrorY = attr&0x100 != 0, attr&0x200 != 0
		}
		if k.SpriteCallback != nil {
			k.SpriteCallback(t)
		}
		fzx, fzy := float64(zx), float64(zy)
		if zx == 0 {
			fzx = 0.5
		}
		if zy == 0 {
			fzy = 0.5
		}
		sx, sy := 64/fzx, 64/fzy
		if zx > 0x2000 || zy > 0x2000 {
			continue
		}
		fullW, fullH := tw*16, th*16
		zs := k.zoomStep && zx != 0 && zy != 0
		var dw, dh int
		if zs {
			dw, dh = fullW*64/zx, fullH*64/zy
		} else {
			dw, dh = int(jsRound(float64(fullW)*sx)), int(jsRound(float64(fullH)*sy))
		}
		if dw <= 0 || dh <= 0 {
			continue
		}
		cx := ((x + offX + 1024 + 512) & 0x3ff) - 512
		cy := ((-(y + offY) + 1024 + 512) & 0x3ff) - 512
		cx += k.DX
		cy += k.DY
		var left, top int
		var mapX, mapY []int16
		dw2, dh2 := dw, dh
		if !is247 {
			left, dw2 = axisMap(tw, fzx, cx, k.mapX[:], k.mapXf[:])
			top, dh2 = axisMap(th, fzy, cy, k.mapY[:], k.mapYf[:])
			mapX, mapY = k.mapX[:], k.mapY[:]
		} else if zs {
			left, top = cx-(dw>>1), cy-(dh>>1)
		} else {
			left = int(jsRound(float64(cx) - float64(dw)/2))
			top = int(jsRound(float64(cy) - float64(dh)/2))
		}
		if flipScreenX {
			left = w - left - dw2
			flipX = !flipX
		}
		if flipScreenY {
			top = h - top - dh2
			flipY = !flipY
		}
		if left >= w || top >= h || left+dw2 <= 0 || top+dh2 <= 0 {
			continue
		}
		color := uint16((t.Color << 4) & 0xfff0)
		pri, shadow, baseCode := uint8(t.Pri), t.Shadow, t.Code
		tcx := (baseCode & 1) | ((baseCode >> 1) & 2) | ((baseCode >> 2) & 4)
		tcy := ((baseCode >> 1) & 1) | ((baseCode >> 2) & 2) | ((baseCode >> 3) & 4)
		x0, x1 := max(0, left), min(w, left+dw2)
		y0, y1 := max(0, top), min(h, top+dh2)
		for py := y0; py < y1; py++ {
			var ty, fy int
			if mapY != nil {
				sj := py - top
				if flipScreenY {
					sj = dh2 - 1 - (py - top)
				}
				m := int(mapY[sj])
				tile, pf := m>>4, false
				if mirrorY && tile*2 < th {
					tile, pf = th-1-tile, true
				}
				if flipY {
					tile, pf = th-1-tile, !pf
				}
				ty = tile
				if pf {
					fy = int(k.mapYf[sj])
				} else {
					fy = m & 15
				}
			} else {
				var v int
				if zs {
					v = min(fullH-1, ((py-top)*zy+12)>>6)
				} else {
					v = (py - top) * fullH / dh
				}
				if flipY {
					v = fullH - 1 - v
				}
				if mirrorY && v*2 < fullH {
					v = fullH - 1 - v
				}
				ty, fy = v>>4, v&15
			}
			row := py * w
			for px := x0; px < x1; px++ {
				var tx, fx int
				if mapX != nil {
					sj := px - left
					if flipScreenX {
						sj = dw2 - 1 - (px - left)
					}
					m := int(mapX[sj])
					tile, pf := m>>4, false
					if flipX {
						tile, pf = tw-1-tile, true
					}
					if mirrorX && tile*2 >= tw {
						tile, pf = tw-1-tile, !pf
					}
					tx = tile
					if pf {
						fx = int(k.mapXf[sj])
					} else {
						fx = m & 15
					}
				} else {
					var u int
					if zs {
						u = min(fullW-1, ((px-left)*zx+12)>>6)
					} else {
						u = (px - left) * fullW / dw
					}
					if flipX {
						u = fullW - 1 - u
					}
					if mirrorX && u*2 < fullW {
						u = fullW - 1 - u
					}
					tx, fx = u>>4, u&15
				}
				var tile int
				if is247 {
					if counters {
						tile = CounterCode(baseCode, tx, ty)
					} else {
						tile = baseCode + cellXOff[tx] + cellYOff[ty]
					}
				} else {
					tile = (baseCode &^ 0x3f) | cellXOff[(tcx+tx)&7] | cellYOff[(tcy+ty)&7]
				}
				tile %= tiles
				pen := pens[tile*256+fy*16+fx]
				if pen == 0 {
					continue
				}
				at := row + px
				if shadow != 0 && pen == 15 {
					if is247 {
						outShadow[at] = uint8(shadow)
						if outPens[at]&15 == 0 {
							outPri[at] = pri
						}
					} else {
						outShadow[at] = pri + 1
					}
					continue
				}
				outPens[at] = color | uint16(pen)
				outPri[at] = pri
				if is247 {
					outShadow[at] = 0
				}
			}
		}
	}
}

func (k *K053245) AppendState(w *arcade.StateW) {
	w.Bytes(k.Ram)
	w.Bytes(k.Buffer)
	w.Bytes(k.Regs[:])
}

func (k *K053245) LoadState(r *arcade.StateR) {
	r.Bytes(k.Ram)
	r.Bytes(k.Buffer)
	r.Bytes(k.Regs[:])
}
