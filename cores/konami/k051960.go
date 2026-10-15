package konami

import "retropixels/cores/arcade"

// Sprite is what a K051960 board's sprite callback sees and changes.
type Sprite struct {
	Code, Color, Pri int
	Shadow           bool
}

var k051960W = [8]int{1, 2, 1, 2, 4, 2, 4, 8}
var k051960H = [8]int{1, 1, 2, 2, 2, 4, 4, 8}
var cellXOff = [8]int{0, 1, 4, 5, 16, 17, 20, 21}
var cellYOff = [8]int{0, 2, 8, 10, 32, 34, 40, 42}

// K051960 is the K051960 / K051937 sprite pair: 128 sprites of 8 bytes.
type K051960 struct {
	rom            []byte
	SpriteCallback func(t *Sprite)
	Ram            [0x400]uint8
	Regs           [8]uint8
	spriteStart    func(code, size int) int
	pens           []uint8
	readbackAddr   func(w int) int
	romByte        func(w, n int) int
	packed         bool
	codes          int
	DX, DY         int
	t              Sprite
	order          [128]int
	toggle, romLow int
}

// K051960Opts are the board's wiring (see k051960.js).
type K051960Opts struct {
	ROM            []byte
	SpriteCallback func(t *Sprite)
	Decode         func(rom []byte) []uint8
	SpriteStart    func(code, size int) int
	Pens           []uint8
	ReadbackAddr   func(w int) int
	RomByte        func(w, n int) int
}

// NewK051960 makes the chips.
func NewK051960(o K051960Opts) *K051960 {
	k := &K051960{rom: o.ROM, SpriteCallback: o.SpriteCallback, spriteStart: o.SpriteStart,
		readbackAddr: o.ReadbackAddr, romByte: o.RomByte}
	if k.rom == nil {
		k.rom = make([]byte, 128)
	}
	switch {
	case o.Pens != nil:
		k.pens = o.Pens
	case o.Decode != nil:
		k.pens = o.Decode(k.rom)
	default:
		k.pens = DecodeK051960(k.rom)
	}
	k.packed = o.Decode != nil && o.SpriteStart != nil
	k.codes = len(k.pens) >> 8
	return k
}

// DecodeK051960: 16x16 cells of four 8x8 planar blocks.
func DecodeK051960(rom []byte) []uint8 {
	n := len(rom) >> 7
	pens := make([]uint8, n*256)
	for c := 0; c < n; c++ {
		for q := 0; q < 4; q++ {
			bx, by := (q&1)*8, (q>>1)*8
			for y := 0; y < 8; y++ {
				o := c*128 + q*32 + y*4
				b0, b1, b2, b3 := rom[o], rom[o+1], rom[o+2], rom[o+3]
				d := c*256 + (by+y)*16 + bx
				for x := 0; x < 8; x++ {
					s := uint(7 - x)
					pens[d+x] = (b0>>s)&1 | ((b1>>s)&1)<<1 | ((b2>>s)&1)<<2 | ((b3>>s)&1)<<3
				}
			}
		}
	}
	return pens
}

// Reset clears the sprite list and registers.
func (k *K051960) Reset() {
	k.Ram = [0x400]uint8{}
	k.Regs = [8]uint8{}
	k.toggle, k.romLow = 0, 0
}

// Read is the sprite list (a read also latches the ROM readback address).
func (k *K051960) Read(off int) uint8 {
	if k.Regs[0]&0x20 != 0 {
		k.romLow = (off >> 2) & 0xff
	}
	return k.Ram[off&0x3ff]
}

func (k *K051960) Write(off int, v uint8) { k.Ram[off&0x3ff] = v }

// FlipScreen is register 0 bit 3.
func (k *K051960) FlipScreen() bool { return k.Regs[0]&8 != 0 }

// ReadReg reads a K051937 register.
func (k *K051960) ReadReg(r int) uint8 {
	r &= 7
	if r == 0 {
		k.toggle ^= 1
		return uint8(k.toggle)
	}
	if r >= 4 && k.Regs[0]&0x20 != 0 {
		w := int(k.Regs[3])<<16 | int(k.Regs[2])<<8 | k.romLow
		if k.readbackAddr != nil {
			w = k.readbackAddr(w)
		}
		if k.romByte != nil {
			return uint8(k.romByte(w, r&3))
		}
		if k.packed {
			at, n := (w*8)%len(k.pens), uint(r&3)
			v := 0
			for x := 0; x < 8; x++ {
				v |= int((k.pens[at+x]>>n)&1) << (7 - x)
			}
			return uint8(v)
		}
		if len(k.rom) > 0 {
			return k.rom[(w*4+(r&3))%len(k.rom)]
		}
		return 0
	}
	return 0
}

func (k *K051960) WriteReg(r int, v uint8) { k.Regs[r&7] = v }

// K051960RenderOpts are Render's options.
type K051960RenderOpts struct {
	X0, Y0 int
	Pri    []uint8
	Shadow []uint8
}

// Render draws the sprites into out (w*h, colour * 16 + pen).
func (k *K051960) Render(out []uint16, w, h int, o K051960RenderOpts) {
	ram, pens, t := &k.Ram, k.pens, &k.t
	slot := &k.order
	for i := range slot {
		slot[i] = -1
	}
	for i := 0; i < 128; i++ {
		if ram[i*8]&0x80 != 0 {
			slot[ram[i*8]&0x7f] = i
		}
	}
	npens := len(pens)
	for p := 0; p < 128; p++ {
		i := slot[p]
		if i < 0 {
			continue
		}
		b := i * 8
		size := int(ram[b+1] >> 5)
		t.Code = int(ram[b+2]) | int(ram[b+1]&0x1f)<<8
		t.Color, t.Pri, t.Shadow = int(ram[b+3]), 0, false
		if k.SpriteCallback != nil {
			k.SpriteCallback(t)
		}
		code, color, pri, shadow := t.Code, uint16(t.Color<<4), uint8(t.Pri), t.Shadow
		zy, zx := int(ram[b+4]>>2), int(ram[b+6]>>2)
		flipY, flipX := ram[b+4]&2 != 0, ram[b+6]&2 != 0
		xr := int(ram[b+6]&1)<<8 | int(ram[b+7])
		yr := int(ram[b+4]&1)<<8 | int(ram[b+5])
		cw, ch := k051960W[size], k051960H[size]
		sw := max(1, (16*cw*(128-zx)+64)>>7)
		shh := max(1, (16*ch*(128-zy)+64)>>7)
		left := ((xr+k.DX-o.X0)%512 + 512) % 512
		top := ((256-yr+k.DY-o.Y0)%512 + 512) % 512
		rowWords := 2 * cw
		start := -1
		if k.spriteStart != nil {
			start = k.spriteStart(code, size)
		}
		for py := 0; py < shh; py++ {
			sy := top + py
			if sy >= 512 {
				sy -= 512
			}
			if sy >= h {
				continue
			}
			srcY := py * 16 * ch / shh
			if flipY {
				srcY = 16*ch - 1 - srcY
			}
			cy, ry := srcY>>4, srcY&15
			rowAt := 0
			if start >= 0 {
				rowAt = (start + srcY*rowWords) * 8
			}
			for px := 0; px < sw; px++ {
				sx := left + px
				if sx >= 512 {
					sx -= 512
				}
				if sx >= w {
					continue
				}
				srcX := px * 16 * cw / sw
				if flipX {
					srcX = 16*cw - 1 - srcX
				}
				var pen uint8
				if start >= 0 {
					pen = pens[(rowAt+srcX)%npens]
				} else {
					if k.codes == 0 {
						continue
					}
					cx, rx := srcX>>4, srcX&15
					c := (code + cellXOff[cx] + cellYOff[cy]) % k.codes
					pen = pens[c*256+ry*16+rx]
				}
				if pen == 0 {
					continue
				}
				at := sy*w + sx
				if shadow && pen == 15 {
					if o.Shadow != nil {
						o.Shadow[at] = 1
					}
					continue
				}
				out[at] = color | uint16(pen)
				if o.Pri != nil {
					o.Pri[at] = pri
				}
				if o.Shadow != nil {
					o.Shadow[at] = 0
				}
			}
		}
	}
}

func (k *K051960) AppendState(w *arcade.StateW) {
	w.Bytes(k.Ram[:])
	w.Bytes(k.Regs[:])
	w.Int(k.toggle)
	w.Int(k.romLow)
}

func (k *K051960) LoadState(r *arcade.StateR) {
	r.Bytes(k.Ram[:])
	r.Bytes(k.Regs[:])
	k.toggle = r.Int()
	k.romLow = r.Int()
}
