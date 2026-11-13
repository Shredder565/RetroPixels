package texpack

import (
	"strings"

	"retropixels/cores/pixelds/nds"
)

// Texture is one texture of a model or texture file, drawn with one
// palette.
type Texture struct {
	File    string // where it came from
	Name    string // the texture's name in the file
	PalName string
	Data    nds.TexData
	Clear0  bool // colour 0 is see-through (TEXIMAGE_PARAM bit 29)
}

type dictEntry struct {
	name string
	data []byte
}

// dict reads a Nitro dictionary (names and fixed-size data) at off.
func dict(b []byte, off int) []dictEntry {
	if off < 0 || off+8 > len(b) {
		return nil
	}
	n := int(b[off+1])
	treeSize := le16(b, off+6)
	p := off + treeSize // the tree (after the 4-byte header) ends here
	if p+4 > len(b) {
		return nil
	}
	unit, dataSize := le16(b, p), le16(b, p+2)
	names := p + dataSize
	if unit == 0 || names+16*n > len(b) || p+4+unit*n > len(b) {
		return nil
	}
	out := make([]dictEntry, n)
	for i := range out {
		nm := b[names+16*i : names+16*i+16]
		out[i].name = strings.TrimRight(string(nm), "\x00")
		out[i].data = b[p+4+unit*i : p+4+unit*(i+1)]
	}
	return out
}

// blocks finds a Nitro file's blocks by tag.
func blocks(b []byte) map[string][]byte {
	if len(b) < 16 || b[4] != 0xff || b[5] != 0xfe {
		return nil
	}
	n := le16(b, 0xe)
	out := map[string][]byte{}
	for i := 0; i < n && 16+i*4+4 <= len(b); i++ {
		o := int(le32(b, 16+i*4))
		if o+8 > len(b) {
			continue
		}
		size := int(le32(b, o+4))
		if size < 8 || o+size > len(b) {
			size = len(b) - o
		}
		out[string(b[o:o+4])] = b[o : o+size]
	}
	return out
}

// IsNitroModel tells whether a file is a model (BMD0) or texture file
// (BTX0).
func IsNitroModel(b []byte) bool {
	return len(b) >= 16 && (string(b[:4]) == "BMD0" || string(b[:4]) == "BTX0")
}

// Textures lists the textures of a BMD0 or BTX0 file with the palettes its
// materials draw them with. allPalettes adds every other palette of the
// file that fits (for characters whose palette changes in the game, like
// Mario's when he has a power-up).
func Textures(path string, b []byte, allPalettes bool) []Texture {
	bl := blocks(b)
	tex := bl["TEX0"]
	if len(tex) < 0x3c {
		return nil
	}
	texDict := dict(tex, le16(tex, 0x0e))
	texData := int(le32(tex, 0x14))
	cmpData := int(le32(tex, 0x24))
	cmpIdx := int(le32(tex, 0x28))
	palDict := dict(tex, int(le32(tex, 0x34)))
	palData := int(le32(tex, 0x38))

	pals := map[string]int{}
	var palNames []string
	for _, e := range palDict {
		if len(e.data) >= 2 {
			pals[e.name] = palData + le16(e.data, 0)*8
			palNames = append(palNames, e.name)
		}
	}
	pairs := materialPairs(bl["MDL0"])

	var out []Texture
	for _, e := range texDict {
		if len(e.data) < 4 {
			continue
		}
		tp := le32(e.data, 0)
		f := int(tp >> 26 & 7)
		w, h := 8<<(tp>>20&7), 8<<(tp>>23&7)
		off := int(tp&0xffff) * 8
		d := nds.TexData{Format: f, W: w, H: h}
		n := nds.TexelBytes(f, w, h)
		if f == nds.Tex4x4 {
			s := cmpData + off
			ix := cmpIdx + off/2
			if s+n > len(tex) || ix+n/2 > len(tex) {
				continue
			}
			d.Texels, d.Index = tex[s:s+n], tex[ix:ix+n/2]
		} else {
			s := texData + off
			if f == 0 || s+n > len(tex) {
				continue
			}
			d.Texels = tex[s : s+n]
		}
		clear0 := tp&(1<<29) != 0
		if f == nds.TexDirect {
			out = append(out, Texture{File: path, Name: e.name, Data: d, Clear0: clear0})
			continue
		}
		// the palettes to try
		var use []string
		seen := map[string]bool{}
		add := func(p string) {
			if _, ok := pals[p]; ok && !seen[p] {
				seen[p] = true
				use = append(use, p)
			}
		}
		for _, p := range pairs[e.name] {
			add(p)
		}
		add(e.name + "_pl")
		if len(use) == 0 && len(palNames) == 1 {
			add(palNames[0])
		}
		if allPalettes || len(use) == 0 && len(palNames) <= 16 {
			for _, p := range palNames {
				add(p)
			}
		}
		for _, p := range use {
			po := pals[p]
			need := nds.PalColors(f) * 2
			if f == nds.Tex4x4 {
				need = nds.PalBytes4x4(d.Index)
			}
			if po >= len(tex) {
				continue
			}
			pd := make([]byte, need) // past the file's palettes: zero, as in an empty VRAM
			copy(pd, tex[po:min(len(tex), po+need)])
			t := Texture{File: path, Name: e.name, PalName: p, Data: d, Clear0: clear0}
			t.Data.Pal = pd
			out = append(out, t)
		}
	}
	return out
}

// materialPairs: for each texture name, the palettes the model's materials
// use with it.
func materialPairs(mdl []byte) map[string][]string {
	out := map[string][]string{}
	if len(mdl) < 16 {
		return out
	}
	for _, m := range dict(mdl, 8) {
		if len(m.data) < 4 {
			continue
		}
		mo := int(le32(m.data, 0))
		if mo+0x14 > len(mdl) {
			continue
		}
		ms := mo + int(le32(mdl, mo+8)) // the materials section
		if ms+4 > len(mdl) {
			continue
		}
		mats := func(off int) map[string][]int {
			r := map[string][]int{}
			for _, e := range dict(mdl, ms+off) {
				if len(e.data) < 4 {
					continue
				}
				lo, n := ms+le16(e.data, 0), int(e.data[2])
				for k := 0; k < n && lo+k < len(mdl); k++ {
					r[e.name] = append(r[e.name], int(mdl[lo+k]))
				}
			}
			return r
		}
		tm, pm := mats(le16(mdl, ms)), mats(le16(mdl, ms+2))
		for t, tl := range tm {
			for p, pl := range pm {
				for _, a := range tl {
					for _, b := range pl {
						if a == b {
							out[t] = append(out[t], p)
						}
					}
				}
			}
		}
	}
	return out
}
