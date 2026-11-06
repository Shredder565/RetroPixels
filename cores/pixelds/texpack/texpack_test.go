package texpack

import (
	"bytes"
	"encoding/binary"
	"os"
	"path/filepath"
	"testing"

	"retropixels/cores/pixelds/nds"
)

// nitroDict builds a Nitro dictionary with one entry.
func nitroDict(name string, data []byte) []byte {
	var b bytes.Buffer
	n := 1
	tree := 4 + 8 + 4*n
	size := tree + 4 + len(data) + 16
	b.Write([]byte{0, byte(n)})
	binary.Write(&b, binary.LittleEndian, uint16(size))
	binary.Write(&b, binary.LittleEndian, uint16(8))
	binary.Write(&b, binary.LittleEndian, uint16(tree))
	binary.Write(&b, binary.LittleEndian, uint32(0x17f))
	binary.Write(&b, binary.LittleEndian, uint32(0))
	binary.Write(&b, binary.LittleEndian, uint16(len(data)))
	binary.Write(&b, binary.LittleEndian, uint16(4+len(data)))
	b.Write(data)
	nm := make([]byte, 16)
	copy(nm, name)
	b.Write(nm)
	return b.Bytes()
}

// testBTX0 is a texture file with one 8×8 4-colour texture and its palette.
func testBTX0() (file []byte, texels, pal []byte) {
	texels = make([]byte, 16)
	for i := range texels {
		texels[i] = byte(i * 37)
	}
	pal = []byte{0x1f, 0, 0xe0, 0x03, 0x00, 0x7c, 0xff, 0x7f} // red, green, blue, white
	tp := uint32(nds.Tex4Color<<26 | 0<<20 | 0<<23)
	td := make([]byte, 8)
	binary.LittleEndian.PutUint32(td, tp)
	texDict := nitroDict("board", td)
	pd := make([]byte, 4)
	palDict := nitroDict("board_pl", pd)

	tex := make([]byte, 0x3c)
	copy(tex, "TEX0")
	texDictOff := len(tex)
	tex = append(tex, texDict...)
	palDictOff := len(tex)
	tex = append(tex, palDict...)
	texDataOff := len(tex)
	tex = append(tex, texels...)
	palDataOff := len(tex)
	tex = append(tex, pal...)
	binary.LittleEndian.PutUint32(tex[4:], uint32(len(tex)))
	binary.LittleEndian.PutUint16(tex[0x0e:], uint16(texDictOff))
	binary.LittleEndian.PutUint32(tex[0x14:], uint32(texDataOff))
	binary.LittleEndian.PutUint32(tex[0x34:], uint32(palDictOff))
	binary.LittleEndian.PutUint32(tex[0x38:], uint32(palDataOff))

	hdr := make([]byte, 20)
	copy(hdr, "BTX0")
	hdr[4], hdr[5] = 0xff, 0xfe
	binary.LittleEndian.PutUint32(hdr[8:], uint32(20+len(tex)))
	binary.LittleEndian.PutUint16(hdr[12:], 16)
	binary.LittleEndian.PutUint16(hdr[14:], 1)
	binary.LittleEndian.PutUint32(hdr[16:], 20)
	return append(hdr, tex...), texels, pal
}

func TestTexturesFromBTX0(t *testing.T) {
	f, texels, pal := testBTX0()
	ts := Textures("/test.btx", f, false)
	if len(ts) != 1 {
		t.Fatalf("%d textures, want 1", len(ts))
	}
	want := nds.TextureHash(&nds.TexData{Format: nds.Tex4Color, W: 8, H: 8, Texels: texels, Pal: pal})
	if got := nds.TextureHash(&ts[0].Data); got != want || ts[0].Name != "board" || ts[0].PalName != "board_pl" {
		t.Errorf("got %s/%s hash %x, want board/board_pl %x", ts[0].Name, ts[0].PalName, got, want)
	}
}

func TestUnpackLZ77(t *testing.T) {
	// "abcabcabcX": 3 literals, a copy of 6 from 3 back, a literal
	in := []byte{0x10, 10, 0, 0, 0x10, 'a', 'b', 'c', 0x30, 0x02, 'X'}
	if got := string(Unpack(in)); got != "abcabcabcX" {
		t.Errorf("unpacked %q", got)
	}
}

func TestModernAndSamePicture(t *testing.T) {
	texels := make([]uint32, 16)
	for i := range texels {
		texels[i] = []uint32{31, 31 << 10, 0x7fff, 16 | 8<<5}[i%4] | 31<<16
	}
	p := fromTexels(4, 4, texels, nil)
	q := modern(p)
	h, s, _ := rgb2hsv(q.px[0], q.px[1], q.px[2])
	if h < 340 || s < 0.8 {
		t.Errorf("red became hue %.0f saturation %.2f", h, s)
	}
	if q.px[8] != 1 || q.px[9] != 1 || q.px[10] != 1 {
		t.Error("white changed")
	}
	if !samePicture(p, q) {
		t.Error("a recoloured picture should count as the same picture")
	}
	big := enlarge(p, 4)
	if big.w != 16 || big.h != 16 {
		t.Errorf("enlarged to %dx%d", big.w, big.h)
	}
}

func TestMakeWritesByHash(t *testing.T) {
	// a ROM with a file system holding the texture file
	f, texels, pal := testBTX0()
	rom := make([]byte, 0x200)
	fnt := len(rom)
	// root folder: entries at fnt+8, first file id 0, one file "mario.btx"
	dir := []byte{0, 0, 0, 0, 0, 0, 1, 0}
	binary.LittleEndian.PutUint32(dir, 8)
	ent := append([]byte{9}, "mario.btx"...)
	ent = append(ent, 0)
	rom = append(rom, dir...)
	rom = append(rom, ent...)
	for len(rom)%4 != 0 {
		rom = append(rom, 0)
	}
	fat := len(rom)
	rom = append(rom, make([]byte, 8)...)
	data := len(rom)
	rom = append(rom, f...)
	binary.LittleEndian.PutUint32(rom[fat:], uint32(data))
	binary.LittleEndian.PutUint32(rom[fat+4:], uint32(len(rom)))
	binary.LittleEndian.PutUint32(rom[0x40:], uint32(fnt))
	binary.LittleEndian.PutUint32(rom[0x44:], uint32(fat-fnt))
	binary.LittleEndian.PutUint32(rom[0x48:], uint32(fat))
	binary.LittleEndian.PutUint32(rom[0x4c:], 8)

	out := t.TempDir()
	r, err := Make(rom, Options{Out: out, Scale: 4, Style: "modern", Characters: []string{"mario"}})
	if err != nil || r.Textures != 1 || r.Characters != 1 {
		t.Fatalf("report %+v, %v", r, err)
	}
	h := nds.TextureHash(&nds.TexData{Format: nds.Tex4Color, W: 8, H: 8, Texels: texels, Pal: pal})
	m, _ := filepath.Glob(filepath.Join(out, nds.HashName(h)+"_*.png"))
	if len(m) != 1 {
		t.Fatalf("no picture named by the hash in %s", out)
	}
	p, err := readPic(m[0])
	if err != nil || p.w != 32 || p.h != 32 {
		t.Errorf("picture %dx%d, %v", p.w, p.h, err)
	}
	os.RemoveAll(out)
}
