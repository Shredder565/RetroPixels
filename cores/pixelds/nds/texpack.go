package nds

// Texture packs: pictures that replace the game's 3D textures, found by a
// hash of each texture (TextureHash), and dumping the game's textures as
// pictures to make packs from.
//
// A pack is a folder of PNG files named <hash>.png, where hash is the
// texture's 16 hex digits; anything after the hash and an underscore is
// ignored, so 0123456789abcdef_mario_body.png works too. Pictures in a
// folder named auto (the ones PixelDS makes itself) give way to others for
// the same texture. A picture can be
// any size (a multiple of the texture's own is best: 4× gives a 64×64
// texture a 256×256 picture). Its alpha is used as is, unless it has
// none: then the texture's own see-through texels stay see-through.

import (
	"fmt"
	"image"
	"image/png"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
)

// texRep is a replacement picture.
type texRep struct {
	w, h   int
	px     []uint32 // bits 0-14 colour, 16-20 alpha, like DecodeTexture
	opaque bool     // no see-through pixels: keep the texture's own
	clear  []uint8  // the texture's own texels: 1 see-through, 2 colour 0
	ow, oh int
}

// texEntry is what a texture in VRAM turned out to be.
type texEntry struct {
	hash uint64
	rep  *texRep
}

type texCache struct {
	mu      sync.Mutex
	on      bool               // load the pack's pictures
	files   map[uint64]string  // hash -> picture file
	loaded  map[uint64]*texRep // pictures read so far (nil: unreadable)
	dumpDir string             // dump textures here
	dumped  map[uint64]bool    // dumped (or already in the folder)
	seen    map[[2]uint32]*texEntry
	dumpErr error
}

// texKey: the TEXIMAGE_PARAM bits that pick the texels (address, size,
// format) and the palette base where the format has one.
func texKey(tp, pltt uint32) [2]uint32 {
	k := tp & (0xffff | 0x3f<<20 | 7<<26)
	if tp>>26&7 == TexDirect {
		pltt = 0
	}
	return [2]uint32{k, pltt}
}

// SetTexturePack loads texture pack pictures from dir ("" for none).
func (m *Machine) SetTexturePack(dir string) error {
	c := &m.gpu3d.tex
	c.mu.Lock()
	defer c.mu.Unlock()
	c.files, c.loaded, c.seen = nil, nil, nil
	c.on = false
	if dir == "" {
		return nil
	}
	files := map[uint64]string{}
	err := filepath.WalkDir(dir, func(p string, d os.DirEntry, err error) error {
		if err != nil || d.IsDir() || !strings.EqualFold(filepath.Ext(p), ".png") {
			return nil
		}
		h, ok := hashFromName(d.Name())
		if !ok {
			return nil
		}
		// pictures in an "auto" folder (made by PixelDS) give way to
		// any of the same texture elsewhere in the pack
		if old, ok := files[h]; ok && !isAuto(dir, old) {
			return nil
		}
		files[h] = p
		return nil
	})
	if err != nil {
		return err
	}
	c.files, c.loaded, c.on = files, map[uint64]*texRep{}, true
	return nil
}

// TexturePackSize is how many pictures the loaded pack has.
func (m *Machine) TexturePackSize() int {
	c := &m.gpu3d.tex
	c.mu.Lock()
	defer c.mu.Unlock()
	return len(c.files)
}

// EnableTexturePack turns the loaded pack on or off.
func (m *Machine) EnableTexturePack(on bool) {
	c := &m.gpu3d.tex
	c.mu.Lock()
	c.on = on && c.files != nil
	c.seen = nil
	c.mu.Unlock()
}

// DumpTextures writes each texture the game draws with, once, into dir as
// <hash>.png ("" stops).
func (m *Machine) DumpTextures(dir string) error {
	c := &m.gpu3d.tex
	c.mu.Lock()
	defer c.mu.Unlock()
	c.dumpDir, c.seen, c.dumpErr = dir, nil, nil
	if dir == "" {
		return nil
	}
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return err
	}
	c.dumped = map[uint64]bool{}
	if es, err := os.ReadDir(dir); err == nil {
		for _, e := range es {
			if h, ok := hashFromName(e.Name()); ok {
				c.dumped[h] = true
			}
		}
	}
	return nil
}

// TexturesDumped is how many textures are in the dump folder.
func (m *Machine) TexturesDumped() int {
	c := &m.gpu3d.tex
	c.mu.Lock()
	defer c.mu.Unlock()
	return len(c.dumped)
}

func isAuto(root, p string) bool {
	rel, err := filepath.Rel(root, p)
	if err != nil {
		return false
	}
	for _, part := range strings.Split(filepath.ToSlash(rel), "/") {
		if strings.EqualFold(part, "auto") {
			return true
		}
	}
	return false
}

func hashFromName(name string) (uint64, bool) {
	if len(name) < 16 {
		return 0, false
	}
	h, err := strconv.ParseUint(name[:16], 16, 64)
	if err != nil || len(name) > 16 && name[16] != '_' && name[16] != '.' {
		return 0, false
	}
	return h, true
}

// HashName is the file name (without .png) for a texture hash.
func HashName(h uint64) string { return fmt.Sprintf("%016x", h) }

// active: the 3D engine should look textures up this frame.
func (c *texCache) active() bool { return c.on || c.dumpDir != "" }

// readTex reads a texture out of texture VRAM.
func (g *gpu3D) readTex(tp, pltt uint32) *TexData {
	f := int(tp >> 26 & 7)
	w, h := 8<<(tp>>20&7), 8<<(tp>>23&7)
	addr := (tp & 0xffff) << 3
	tex, pal := g.m.vram.tex, g.m.vram.texpal
	d := &TexData{Format: f, W: w, H: h, Texels: make([]byte, TexelBytes(f, w, h))}
	for i := range d.Texels {
		d.Texels[i] = tex.read8(addr + uint32(i))
	}
	palBase := pltt << 4
	if f == Tex4Color {
		palBase = pltt << 3
	}
	n := PalColors(f) * 2
	if f == Tex4x4 {
		slot1 := 0x20000 + (addr&0x1ffff)/2
		if addr >= 0x40000 {
			slot1 += 0x10000
		}
		d.Index = make([]byte, w*h/16*2)
		for i := range d.Index {
			d.Index[i] = tex.read8(slot1 + uint32(i))
		}
		n = PalBytes4x4(d.Index)
	}
	d.Pal = make([]byte, n)
	for i := range d.Pal {
		d.Pal[i] = pal.read8(palBase + uint32(i))
	}
	return d
}

// lookup finds the replacement for a texture (nil: none), dumping it if
// asked. Called by the drawing goroutines, so it locks.
func (g *gpu3D) lookup(tp, pltt uint32) *texRep {
	c := &g.tex
	c.mu.Lock()
	defer c.mu.Unlock()
	if g.m.vram.dirtyTex {
		c.seen = nil
		g.m.vram.dirtyTex = false
	}
	k := texKey(tp, pltt)
	if e, ok := c.seen[k]; ok {
		return e.rep
	}
	if c.seen == nil {
		c.seen = map[[2]uint32]*texEntry{}
	}
	d := g.readTex(tp, pltt)
	texels := DecodeTexture(d)
	e := &texEntry{hash: HashTexels(d.Format, d.W, d.H, texels)}
	c.seen[k] = e
	if c.dumpDir != "" && !c.dumped[e.hash] {
		c.dumped[e.hash] = true
		if err := WriteTexturePNG(filepath.Join(c.dumpDir, HashName(e.hash)+".png"), d.W, d.H, texels, tp&(1<<29) != 0 && d.Format >= Tex4Color && d.Format <= Tex256Color, d); err != nil && c.dumpErr == nil {
			c.dumpErr = err
		}
	}
	if c.on {
		r, ok := c.loaded[e.hash]
		if !ok {
			if p, ok := c.files[e.hash]; ok {
				r = loadRep(p)
			}
			c.loaded[e.hash] = r
		}
		if r != nil && r.opaque && r.clear == nil {
			r.clear, r.ow, r.oh = make([]uint8, len(texels)), d.W, d.H
			z := color0(d)
			for i, v := range texels {
				if v>>16&31 == 0 {
					r.clear[i] = 1
				} else if z[i] {
					r.clear[i] = 2
				}
			}
		}
		e.rep = r
	}
	return e.rep
}

// color0 tells which texels of a paletted texture use colour 0.
func color0(d *TexData) []bool {
	z := make([]bool, d.W*d.H)
	for i := range z {
		switch d.Format {
		case Tex4Color:
			z[i] = int(d.Texels[i/4])>>(i&3*2)&3 == 0
		case Tex16Color:
			z[i] = int(d.Texels[i/2])>>(i&1*4)&15 == 0
		case Tex256Color:
			z[i] = d.Texels[i] == 0
		}
	}
	return z
}

// WriteTexturePNG writes decoded texels as a PNG; with clear0, colour 0 of
// a paletted texture is see-through (as the game draws it).
func WriteTexturePNG(path string, w, h int, texels []uint32, clear0 bool, d *TexData) error {
	img := image.NewNRGBA(image.Rect(0, 0, w, h))
	var z []bool
	if clear0 && d != nil {
		z = color0(d)
	}
	for i, v := range texels {
		a := v >> 16 & 31
		if z != nil && z[i] {
			a = 0
		}
		c := to888(v)
		img.Pix[i*4+0] = uint8(c >> 16)
		img.Pix[i*4+1] = uint8(c >> 8)
		img.Pix[i*4+2] = uint8(c)
		img.Pix[i*4+3] = uint8(a*255/31 + 0)
	}
	f, err := os.Create(path)
	if err != nil {
		return err
	}
	if err := png.Encode(f, img); err != nil {
		f.Close()
		return err
	}
	return f.Close()
}

func to888(v uint32) uint32 {
	r, g, b := v&31, v>>5&31, v>>10&31
	return (r<<3|r>>2)<<16 | (g<<3|g>>2)<<8 | (b<<3 | b>>2)
}

// loadRep reads a pack picture (nil if it can't).
func loadRep(path string) *texRep {
	f, err := os.Open(path)
	if err != nil {
		return nil
	}
	defer f.Close()
	img, _, err := image.Decode(f)
	if err != nil {
		return nil
	}
	b := img.Bounds()
	r := &texRep{w: b.Dx(), h: b.Dy(), opaque: true}
	if r.w == 0 || r.h == 0 || r.w > 4096 || r.h > 4096 {
		return nil
	}
	r.px = make([]uint32, r.w*r.h)
	for y := 0; y < r.h; y++ {
		for x := 0; x < r.w; x++ {
			cr, cg, cb, ca := img.At(b.Min.X+x, b.Min.Y+y).RGBA()
			if ca < 0xffff {
				r.opaque = false
			}
			if ca != 0 { // un-premultiply
				cr, cg, cb = cr*0xffff/ca, cg*0xffff/ca, cb*0xffff/ca
			}
			a := (ca*31 + 0x7fff) / 0xffff
			r.px[y*r.w+x] = cr>>11 | cg>>11<<5 | cb>>11<<10 | a<<16
		}
	}
	return r
}

// wrapN is wrap for any size n.
func wrapN(v, n int, rep, flip bool) int {
	if !rep {
		return min(max(v, 0), n-1)
	}
	if flip {
		v %= 2 * n
		if v < 0 {
			v += 2 * n
		}
		if v >= n {
			v = 2*n - 1 - v
		}
		return v
	}
	v %= n
	if v < 0 {
		v += n
	}
	return v
}

// at samples the picture at the texture's coordinates s, t (texels × 16).
func (r *texRep) at(ts *texSampler, sf, tf float64) (uint16, int) {
	x := wrapN(int(sf*float64(r.w)/float64(ts.w*16)), r.w, ts.repS, ts.flipS)
	y := wrapN(int(tf*float64(r.h)/float64(ts.h*16)), r.h, ts.repT, ts.flipT)
	if sf < 0 && !ts.repS {
		x = 0
	}
	if tf < 0 && !ts.repT {
		y = 0
	}
	v := r.px[y*r.w+x]
	if r.clear != nil {
		ox := min(x*r.ow/r.w, r.ow-1)
		oy := min(y*r.oh/r.h, r.oh-1)
		switch r.clear[oy*r.ow+ox] {
		case 1:
			return 0, 0
		case 2:
			if ts.color0Trans {
				return 0, 0
			}
		}
	}
	return uint16(v & 0x7fff), int(v >> 16 & 31)
}
