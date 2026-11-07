package boxart

import (
	"encoding/binary"
	"encoding/json"
	"fmt"
	"image"
	"image/color"
	"io"
	"os"
	"path/filepath"
	"regexp"
	"strings"
)

// ReadID reads the game's ID from its file, for GameTDB: a DS game's code
// ("A2DE"), a 3DS game's ("AREE", from "CTR-P-AREE") or a Wii U game's
// ("ARDE", from "WUP-P-ARDE"). "" when the file doesn't say.
func ReadID(sys, path string) string {
	if path == "" {
		return ""
	}
	ext := strings.ToLower(filepath.Ext(path))
	switch sys {
	case DS:
		switch ext {
		case ".nds", ".dsi", ".srl":
			b := readAt(path, 0x0C, 4)
			if isCode(b) {
				return string(b)
			}
		}
	case N3DS:
		switch ext {
		case ".3ds", ".cci":
			if string(readAt(path, 0x100, 4)) != "NCSD" {
				return ""
			}
			off := int64(binary.LittleEndian.Uint32(pad(readAt(path, 0x120, 4), 4))) * 0x200
			return ncchCode(path, off)
		case ".cxi":
			return ncchCode(path, 0)
		case ".cia":
			h := pad(readAt(path, 0, 0x20), 0x20)
			al := func(n uint32) int64 { return (int64(n) + 63) &^ 63 }
			hdr, cert, tik, tmd := binary.LittleEndian.Uint32(h[0:]), binary.LittleEndian.Uint32(h[8:]),
				binary.LittleEndian.Uint32(h[12:]), binary.LittleEndian.Uint32(h[16:])
			return ncchCode(path, al(hdr)+al(cert)+al(tik)+al(tmd))
		}
	case WiiU:
		switch ext {
		case ".wud", ".iso":
			return wiiuCode(readAt(path, 0, 10))
		case ".wux":
			return wuxCode(path)
		case ".rpx", ".elf":
			// An unpacked game: meta\meta.xml beside its code folder.
			if dir := filepath.Dir(path); strings.EqualFold(filepath.Base(dir), "code") {
				b, _ := os.ReadFile(filepath.Join(filepath.Dir(dir), "meta", "meta.xml"))
				if m := reProductCode.FindSubmatch(b); m != nil {
					return wiiuCode(m[1])
				}
			}
		}
	}
	return ""
}

var reProductCode = regexp.MustCompile(`<product_code[^>]*>\s*([^<\s]+)\s*<`)

func ncchCode(path string, off int64) string {
	if string(readAt(path, off+0x100, 4)) != "NCCH" {
		return ""
	}
	pc := strings.TrimRight(string(readAt(path, off+0x150, 16)), "\x00 ")
	if i := strings.LastIndexByte(pc, '-'); i >= 0 && len(pc)-i-1 == 4 && isCode([]byte(pc[i+1:])) {
		return pc[i+1:]
	}
	return ""
}

// wiiuCode: "WUP-P-ARDE" -> "ARDE".
func wiiuCode(b []byte) string {
	s := string(b)
	if len(s) >= 10 && strings.HasPrefix(s, "WUP-") && s[5] == '-' && isCode([]byte(s[6:10])) {
		return s[6:10]
	}
	return ""
}

// wuxCode reads the product code from a .wux (a compressed .wud): its first
// sector, found through the sector table.
func wuxCode(path string) string {
	f, err := os.Open(path)
	if err != nil {
		return ""
	}
	defer f.Close()
	h := make([]byte, 0x24)
	if _, err := io.ReadFull(f, h); err != nil || string(h[:4]) != "WUX0" {
		return ""
	}
	sector := int64(binary.LittleEndian.Uint32(h[8:]))
	size := int64(binary.LittleEndian.Uint64(h[0x10:]))
	if sector <= 0 || sector > 1<<24 || size <= 0 {
		return ""
	}
	count := (size + sector - 1) / sector
	data := (0x20 + count*4 + sector - 1) / sector * sector
	idx := make([]byte, 4)
	if _, err := f.ReadAt(idx, 0x20); err != nil {
		return ""
	}
	b := make([]byte, 10)
	if _, err := f.ReadAt(b, data+int64(binary.LittleEndian.Uint32(idx))*sector); err != nil {
		return ""
	}
	return wiiuCode(b)
}

func isCode(b []byte) bool {
	if len(b) != 4 {
		return false
	}
	for _, c := range b {
		if !(c >= 'A' && c <= 'Z' || c >= '0' && c <= '9') {
			return false
		}
	}
	return true
}

func readAt(path string, off int64, n int) []byte {
	f, err := os.Open(path)
	if err != nil {
		return nil
	}
	defer f.Close()
	b := make([]byte, n)
	m, _ := f.ReadAt(b, off)
	return b[:m]
}

func pad(b []byte, n int) []byte {
	if len(b) >= n {
		return b
	}
	return append(b, make([]byte, n-len(b))...)
}

func getJSON(u string, v any) error {
	b, err := get(u)
	if err != nil {
		return err
	}
	if err := json.Unmarshal(b, v); err != nil {
		return fmt.Errorf("%s: %v", u, err)
	}
	return nil
}

// ---------------- the game's own icon ----------------

// ownIcon: an unpacked Wii U game's icon (meta\iconTex.tga), as a PNG.
func ownIcon(g Game) []byte {
	if g.System != WiiU {
		return nil
	}
	dir := filepath.Dir(g.Path)
	if !strings.EqualFold(filepath.Base(dir), "code") {
		return nil
	}
	b, err := os.ReadFile(filepath.Join(filepath.Dir(dir), "meta", "iconTex.tga"))
	if err != nil {
		return nil
	}
	img := decodeTGA(b)
	if img == nil {
		return nil
	}
	return encodePNG(img)
}

// decodeTGA reads an uncompressed or RLE true-colour TGA (24 or 32 bits).
func decodeTGA(b []byte) image.Image {
	if len(b) < 18 {
		return nil
	}
	idLen, cmap, kind := int(b[0]), b[1], b[2]
	w, h := int(binary.LittleEndian.Uint16(b[12:])), int(binary.LittleEndian.Uint16(b[14:]))
	bpp, desc := int(b[16]), b[17]
	if cmap != 0 || (kind != 2 && kind != 10) || (bpp != 24 && bpp != 32) || w <= 0 || h <= 0 || w > 4096 || h > 4096 {
		return nil
	}
	px := bpp / 8
	p := 18 + idLen
	img := image.NewNRGBA(image.Rect(0, 0, w, h))
	topDown := desc&0x20 != 0
	set := func(i int, c []byte) {
		x, y := i%w, i/w
		if !topDown {
			y = h - 1 - y
		}
		a := byte(255)
		if px == 4 {
			a = c[3]
		}
		img.SetNRGBA(x, y, color.NRGBA{R: c[2], G: c[1], B: c[0], A: a})
	}
	n := w * h
	if kind == 2 {
		if len(b) < p+n*px {
			return nil
		}
		for i := 0; i < n; i++ {
			set(i, b[p+i*px:])
		}
		return img
	}
	for i := 0; i < n; {
		if p >= len(b) {
			return nil
		}
		hdr := b[p]
		p++
		cnt := int(hdr&0x7f) + 1
		if hdr&0x80 != 0 {
			if p+px > len(b) {
				return nil
			}
			for k := 0; k < cnt && i < n; k++ {
				set(i, b[p:])
				i++
			}
			p += px
		} else {
			if p+cnt*px > len(b) {
				return nil
			}
			for k := 0; k < cnt && i < n; k++ {
				set(i, b[p:])
				p += px
				i++
			}
		}
	}
	return img
}
