package nds

// Texture decoding shared by texture packs (texpack.go) and the texture
// pack maker (cores/pixelds/texpack), which reads the same textures from the
// game's model files. Both must decode a texture to the same texels, since
// a texture pack finds its pictures by a hash of them.

import (
	"encoding/binary"
	"hash/fnv"
)

// Texture formats (TEXIMAGE_PARAM bits 26-28).
const (
	TexA3I5     = 1
	Tex4Color   = 2
	Tex16Color  = 3
	Tex256Color = 4
	Tex4x4      = 5
	TexA5I3     = 6
	TexDirect   = 7
)

// TexData is one texture's bytes as the 3D engine reads them.
type TexData struct {
	Format int
	W, H   int
	Texels []byte // the texel data (4×4 compressed: the blocks)
	Index  []byte // 4×4 compressed only: the palette index data, 2 bytes per block
	Pal    []byte // the palette, from the texture's palette base
}

// TexelBytes is the size of a w×h texture's texel data in format f.
func TexelBytes(f, w, h int) int {
	switch f {
	case Tex4Color, Tex4x4:
		return w * h / 4
	case Tex16Color:
		return w * h / 2
	case TexDirect:
		return w * h * 2
	}
	return w * h
}

// PalColors is how many palette colours a texture in format f can use (4×4
// compressed textures: see PalBytes4x4).
func PalColors(f int) int {
	switch f {
	case TexA3I5:
		return 32
	case Tex4Color:
		return 4
	case Tex16Color:
		return 16
	case Tex256Color:
		return 256
	case TexA5I3:
		return 8
	}
	return 0
}

// PalBytes4x4 is how much palette a 4×4 compressed texture's index data
// reaches.
func PalBytes4x4(index []byte) int {
	n := 0
	for i := 0; i+1 < len(index); i += 2 {
		v := int(binary.LittleEndian.Uint16(index[i:]))
		if e := (v&0x3fff)*4 + 4; e > n {
			n = e
		}
	}
	return n * 2
}

// DecodeTexture returns the texture's texels, row by row: bits 0-14 the
// colour (5 bits each of red, green, blue), bits 16-20 the alpha (0-31).
// Colour 0 of paletted formats is kept opaque; whether it's transparent is
// the polygon's choice (TEXIMAGE_PARAM bit 29), applied when drawing.
func DecodeTexture(d *TexData) []uint32 {
	w, h := d.W, d.H
	out := make([]uint32, w*h)
	b8 := func(i int) uint32 {
		if i < len(d.Texels) {
			return uint32(d.Texels[i])
		}
		return 0
	}
	pal := func(idx int) uint32 {
		if i := idx * 2; i+1 < len(d.Pal) {
			return uint32(binary.LittleEndian.Uint16(d.Pal[i:])) & 0x7fff
		}
		return 0
	}
	const op = 31 << 16
	for t := 0; t < h; t++ {
		for s := 0; s < w; s++ {
			i := t*w + s
			var v uint32
			switch d.Format {
			case TexA3I5:
				b := b8(i)
				a := b >> 5
				v = pal(int(b&31)) | (a*4+a/2)<<16
			case Tex4Color:
				v = pal(int(b8(i/4)>>(i&3*2)&3)) | op
			case Tex16Color:
				v = pal(int(b8(i/2)>>(i&1*4)&15)) | op
			case Tex256Color:
				v = pal(int(b8(i))) | op
			case Tex4x4:
				v = texel4x4(d, s, t, pal)
			case TexA5I3:
				b := b8(i)
				v = pal(int(b&7)) | (b>>3)<<16
			case TexDirect:
				c := b8(i*2) | b8(i*2+1)<<8
				if c&0x8000 != 0 {
					v = c&0x7fff | op
				}
			}
			out[i] = v
		}
	}
	return out
}

func texel4x4(d *TexData, s, t int, pal func(int) uint32) uint32 {
	blk := (t/4)*(d.W/4) + s/4
	if blk*4+3 >= len(d.Texels) || blk*2+1 >= len(d.Index) {
		return 0
	}
	word := binary.LittleEndian.Uint32(d.Texels[blk*4:])
	texel := word >> (uint(t&3)*8 + uint(s&3)*2) & 3
	info := int(binary.LittleEndian.Uint16(d.Index[blk*2:]))
	base := (info & 0x3fff) * 2
	c := func(k int) uint32 { return pal(base + k) }
	mix := func(a, b, wa, wb, div uint32) uint32 {
		ch := func(sh uint) uint32 { return (a>>sh&31*wa + b>>sh&31*wb) / div }
		return ch(0) | ch(5)<<5 | ch(10)<<10
	}
	const op = 31 << 16
	switch info >> 14 {
	case 0:
		if texel == 3 {
			return 0
		}
		return c(int(texel)) | op
	case 1:
		switch texel {
		case 2:
			return mix(c(0), c(1), 1, 1, 2) | op
		case 3:
			return 0
		}
		return c(int(texel)) | op
	case 2:
		return c(int(texel)) | op
	}
	switch texel {
	case 2:
		return mix(c(0), c(1), 5, 3, 8) | op
	case 3:
		return mix(c(0), c(1), 3, 5, 8) | op
	}
	return c(int(texel)) | op
}

// TextureHash names a texture in a texture pack: a hash of its format, size
// and decoded texels, so the same picture with the same colours always gets
// the same name, wherever the game put it in VRAM.
func TextureHash(d *TexData) uint64 {
	return HashTexels(d.Format, d.W, d.H, DecodeTexture(d))
}

// HashTexels is TextureHash for texels already decoded.
func HashTexels(f, w, h int, texels []uint32) uint64 {
	hs := fnv.New64a()
	var b [4]byte
	binary.LittleEndian.PutUint32(b[:], uint32(f)|uint32(w)<<8|uint32(h)<<20)
	hs.Write(b[:])
	buf := make([]byte, 4*len(texels))
	for i, v := range texels {
		binary.LittleEndian.PutUint32(buf[i*4:], v)
	}
	hs.Write(buf)
	return hs.Sum64()
}
