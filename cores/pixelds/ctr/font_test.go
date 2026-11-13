package ctr

import (
	"encoding/binary"
	"image"
	"image/color"
	"image/png"
	"os"
	"testing"
)

// TestSharedFont reads the built font as a game would: from the CFNT's
// pointers, through the character map, sampling the sheets as A4
// textures, and draws a line of text.
func TestSharedFont(t *testing.T) {
	const base = sharedFontAddr
	blk := buildSharedFont(base)
	le := binary.LittleEndian
	at := func(a uint32) []byte { return blk[a-base:] }
	if string(blk[0x80:0x84]) != "CFNT" || le.Uint32(blk) != 2 {
		t.Fatal("no CFNT")
	}
	finf := blk[0x80+0x14:]
	tglp := at(le.Uint32(finf[16:]) - 8)
	cwdh := at(le.Uint32(finf[20:]) - 8)
	cmap := at(le.Uint32(finf[24:]) - 8)
	if string(tglp[:4]) != "TGLP" || string(cwdh[:4]) != "CWDH" || string(cmap[:4]) != "CMAP" {
		t.Fatal("bad pointers")
	}
	cw, ch := int(tglp[8]), int(tglp[9])
	sheetSize := le.Uint32(tglp[12:])
	cols := int(le.Uint16(tglp[20:]))
	rows := int(le.Uint16(tglp[22:]))
	sw := int(le.Uint16(tglp[24:]))
	sheets := le.Uint32(tglp[28:])
	index := func(r rune) int {
		n := int(le.Uint16(cmap[20:]))
		for i := 0; i < n; i++ {
			if rune(le.Uint16(cmap[22+4*i:])) == r {
				return int(le.Uint16(cmap[24+4*i:]))
			}
		}
		return int(le.Uint16(finf[10:]))
	}
	text := "Hello, 3DS! ÀéÑ ö…"
	img := image.NewGray(image.Rect(0, 0, 400, ch))
	x := 0
	ink := 0
	for _, r := range text {
		i := index(r)
		w := cwdh[16+3*i:]
		sheet := i / (cols * rows)
		cx, cy := (i%(cols*rows))%cols*(cw+1), (i%(cols*rows))/cols*(ch+1)
		data := at(sheets + uint32(sheet)*sheetSize)
		for yy := 0; yy < ch; yy++ {
			for xx := 0; xx < int(w[1]); xx++ {
				c := decodeTexel(data, 11, int(tiledOffset(uint32(cx+xx), uint32(cy+yy), uint32(sw))))
				if px := x + int(int8(w[0])) + xx; px >= 0 && px < 400 {
					img.SetGray(px, yy, color.Gray{c[3]})
					if c[3] > 128 {
						ink++
					}
				}
			}
		}
		x += int(w[2])
	}
	if ink < 200 || x < 150 {
		t.Errorf("text drawn %d px wide with %d inked pixels", x, ink)
	}
	if index('A') == index('?') || index('一') != index('?') {
		t.Error("character map lookups")
	}
	if p := os.Getenv("PIXELDS_FONT_PNG"); p != "" {
		f, _ := os.Create(p)
		png.Encode(f, img)
		f.Close()
	}
}
