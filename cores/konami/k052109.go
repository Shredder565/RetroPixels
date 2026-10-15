// Package konami holds the Konami arcade boards' video and sound chips, Go
// ports of the web version's chips (web/emu/k052109.js, k051960.js,
// k053245.js, k053251.js, k007232.js, k053260.js, upd7759.js and
// eeprom93c46.js). Each behaves exactly like its JS original.
package konami

import "retropixels/cores/arcade"

// Tile is what a K052109 board's tile callback sees and changes (t in the JS).
type Tile struct {
	Layer, Bank, Code, Color int
	FlipX, FlipY             bool
	Pri                      int
}

// K052109 is the tilemap chip (with its K051962 pixel partner): a fixed
// layer (0) and two scrolling layers A (1) and B (2) of 64x32 8x8 tiles.
type K052109 struct {
	Ram          [0x6000]uint8
	rom          []byte
	TileCallback func(t *Tile)
	extraCode    bool
	Pens         []uint8 // one byte per pixel, 64 per tile
	tileCount    int
	DX, DY       [3]int
	Rmrd         bool // the board's ROM-read line
	RomRead      func(off int, k *K052109) int

	tCode  [2048]int
	tColor [2048]uint16
	tFlags [2048]uint8
	tPri   [2048]uint8
	t      Tile
}

// K052109Opts are the board's wiring.
type K052109Opts struct {
	ROM          []byte
	TileCallback func(t *Tile)
	ExtraCode    bool
	Decode       func(rom []byte) []uint8 // nil: the chip's own planar layout
	RomRead      func(off int, k *K052109) int
}

// NewK052109 makes the chip.
func NewK052109(o K052109Opts) *K052109 {
	k := &K052109{rom: o.ROM, TileCallback: o.TileCallback, extraCode: o.ExtraCode, RomRead: o.RomRead}
	if k.rom == nil {
		k.rom = make([]byte, 32)
	}
	if o.Decode != nil {
		k.Pens = o.Decode(k.rom)
	} else {
		k.Pens = DecodeK052109(k.rom)
	}
	k.tileCount = len(k.Pens) >> 6
	k.Reset()
	return k
}

// DecodeK052109: 4 bytes a row, byte n = colour bit n of the row's 8 pixels.
func DecodeK052109(rom []byte) []uint8 {
	n := len(rom) >> 5
	pens := make([]uint8, n*64)
	for t := 0; t < n; t++ {
		for y := 0; y < 8; y++ {
			o := t*32 + y*4
			b0, b1, b2, b3 := rom[o], rom[o+1], rom[o+2], rom[o+3]
			for x := 0; x < 8; x++ {
				s := uint(7 - x)
				pens[t*64+y*8+x] = (b0>>s)&1 | ((b1>>s)&1)<<1 | ((b2>>s)&1)<<2 | ((b3>>s)&1)<<3
			}
		}
	}
	return pens
}

// Reset clears the chip's RAM.
func (k *K052109) Reset() {
	k.Ram = [0x6000]uint8{}
	k.Rmrd = false
}

func (k *K052109) scrollCtrl() int  { return int(k.Ram[0x1c80]) }
func (k *K052109) IRQEnabled() bool { return k.Ram[0x1d00]&4 != 0 }
func (k *K052109) FIRQEnabled() bool {
	return k.Ram[0x1d00]&2 != 0
}
func (k *K052109) NMIEnabled() bool { return k.Ram[0x1d00]&1 != 0 }
func (k *K052109) FlipScreen() bool { return k.Ram[0x1e80]&1 != 0 }

// BankReg is tile ROM bank register i (0-3).
func (k *K052109) BankReg(i int) int {
	if i < 2 {
		return int(k.Ram[0x1d80]>>(uint(i)*4)) & 15
	}
	return int(k.Ram[0x1f00]>>(uint(i-2)*4)) & 15
}

// Read is the chip's own address space (0x0000-0x5fff).
func (k *K052109) Read(off int) uint8 {
	off &= 0x7fff
	if off >= 0x6000 {
		return 0
	}
	if k.Rmrd {
		if k.RomRead != nil {
			return uint8(k.RomRead(off, k))
		}
		return k.ReadRom(off)
	}
	return k.Ram[off]
}

// Write is the chip's own address space.
func (k *K052109) Write(off int, v uint8) {
	off &= 0x7fff
	if off < 0x6000 {
		k.Ram[off] = v
	}
}

// ReadRom: the ROM byte shown at off while the ROM-read line is on.
func (k *K052109) ReadRom(off int) uint8 {
	n := len(k.rom)
	if n == 0 {
		return 0xff
	}
	a := ((int(k.Ram[0x1e00])<<13 | (off & 0x1fff)) * 4) + ((off >> 13) & 3)
	return k.rom[uint32(a)%uint32(n)]
}

func (k *K052109) buildLayer(layer int) {
	ram, t, flipCtl, base := &k.Ram, &k.t, k.Ram[0x1e80], layer*0x800
	max := k.tileCount
	for i := 0; i < 2048; i++ {
		attr := int(ram[base+i])
		code := int(ram[0x2000+base+i])
		if k.extraCode {
			code |= int(ram[0x4000+base+i]) << 8
		}
		bankFull := k.BankReg((attr >> 2) & 3)
		attr = (attr & 0xf3) | ((bankFull & 3) << 2)
		t.Layer, t.Bank, t.Code, t.Color, t.Pri = layer, bankFull>>2, code, attr, 0
		t.FlipX = flipCtl&2 != 0 && attr&1 != 0
		t.FlipY = flipCtl&4 != 0 && attr&2 != 0
		if k.TileCallback != nil {
			k.TileCallback(t)
		}
		if max > 0 {
			k.tCode[i] = ((t.Code % max) + max) % max
		} else {
			k.tCode[i] = 0
		}
		k.tColor[i] = uint16(t.Color)
		f := uint8(0)
		if t.FlipX {
			f |= 1
		}
		if t.FlipY {
			f |= 2
		}
		k.tFlags[i] = f
		k.tPri[i] = uint8(t.Pri)
	}
}

// RenderOpts are render's options.
type RenderOpts struct {
	X0, Y0   int
	Opaque   bool
	Pri      []uint8
	PriMask  int
	PriValue int
}

// Render draws one layer into out (w*h, colour * 16 + pen) for the window
// whose top-left is map pixel (X0, Y0) of the unscrolled screen.
func (k *K052109) Render(layer int, out []uint16, w, h int, o RenderOpts) {
	k.buildLayer(layer)
	ram, pens := &k.Ram, k.Pens
	flip := k.FlipScreen()
	mode, sbase := 0, 0
	if layer == 1 {
		mode, sbase = k.scrollCtrl()&7, 0x1800
	} else if layer == 2 {
		mode, sbase = (k.scrollCtrl()>>3)&7, 0x3800
	}
	xmode, colY := mode&3, mode&4 != 0
	dx, dy := k.DX[layer], k.DY[layer]
	for sy := 0; sy < h; sy++ {
		line := o.Y0 + sy
		if flip {
			line = o.Y0 + h - 1 - sy
		}
		line &= 255
		xs, ysG := 0, 0
		if layer != 0 {
			li := 0
			if xmode == 3 {
				li = line
			} else if xmode == 2 {
				li = line & 0xf8
			}
			xs = int(ram[sbase+0x200+li*2]) | (int(ram[sbase+0x201+li*2])&1)<<8
			ysG = int(ram[sbase+0x0c])
		}
		row := sy * w
		for sx := 0; sx < w; sx++ {
			scrX := o.X0 + sx
			if flip {
				scrX = o.X0 + w - 1 - sx
			}
			ys := ysG
			if colY {
				ys = int(ram[sbase+((scrX>>3)&63)])
			}
			mx, my := (scrX+xs+dx)&511, (line+ys+dy)&255
			i := (my>>3)*64 + (mx >> 3)
			if o.PriMask != 0 && int(k.tPri[i])&o.PriMask != o.PriValue {
				continue
			}
			f := k.tFlags[i]
			px, py := mx&7, my&7
			if f&1 != 0 {
				px ^= 7
			}
			if f&2 != 0 {
				py ^= 7
			}
			pen := pens[k.tCode[i]*64+py*8+px]
			if pen != 0 || o.Opaque {
				out[row+sx] = k.tColor[i]<<4 | uint16(pen)
				if o.Pri != nil {
					o.Pri[row+sx] = k.tPri[i]
				}
			}
		}
	}
}

// AppendState / LoadState: the RAM, the ROM-read line and the offsets.
func (k *K052109) AppendState(w *arcade.StateW) {
	w.Bytes(k.Ram[:])
	w.Bool(k.Rmrd)
	w.Ints(k.DX[:])
	w.Ints(k.DY[:])
}

func (k *K052109) LoadState(r *arcade.StateR) {
	r.Bytes(k.Ram[:])
	k.Rmrd = r.Bool()
	r.Ints(k.DX[:])
	r.Ints(k.DY[:])
}
