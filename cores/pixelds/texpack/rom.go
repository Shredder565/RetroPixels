// Package texpack makes texture packs for PixelDS (see cores/pixelds/nds
// texpack.go) straight from a DS game: it finds the 3D models and texture
// files in the game's file system, decodes each texture with each palette
// it can be drawn with, names it by the same hash PixelDS looks textures up
// by, and writes it bigger and, for the chosen characters, restyled.
package texpack

import (
	"encoding/binary"
	"errors"
)

// File is one file of the game's file system (or of an archive in it).
type File struct {
	Path string
	Data []byte
}

// ROMFiles lists the files of a DS game's file system.
func ROMFiles(rom []byte) ([]File, error) {
	if len(rom) < 0x200 {
		return nil, errors.New("not a DS game")
	}
	fntOff := int(le32(rom, 0x40))
	fatOff := int(le32(rom, 0x48))
	fatSize := int(le32(rom, 0x4c))
	if fntOff <= 0 || fntOff >= len(rom) || fatOff <= 0 || fatOff+fatSize > len(rom) {
		return nil, errors.New("the game has no file system")
	}
	var out []File
	var walk func(dir int, path string, depth int)
	walk = func(dir int, path string, depth int) {
		off := fntOff + (dir&0xfff)*8
		if depth > 32 || off+8 > len(rom) {
			return
		}
		p := fntOff + int(le32(rom, off))
		id := int(binary.LittleEndian.Uint16(rom[off+4:]))
		for p < len(rom) {
			l := int(rom[p])
			p++
			if l == 0 {
				break
			}
			n := l & 0x7f
			if p+n > len(rom) {
				return
			}
			name := string(rom[p : p+n])
			p += n
			if l&0x80 != 0 {
				if p+2 > len(rom) {
					return
				}
				sub := int(binary.LittleEndian.Uint16(rom[p:]))
				p += 2
				walk(sub, path+name+"/", depth+1)
				continue
			}
			if e := fatOff + id*8; e+8 <= fatOff+fatSize {
				s, t := int(le32(rom, e)), int(le32(rom, e+4))
				if s <= t && t <= len(rom) {
					out = append(out, File{path + name, rom[s:t]})
				}
			}
			id++
		}
	}
	walk(0xf000, "/", 0)
	return out, nil
}

func le32(b []byte, o int) uint32 { return binary.LittleEndian.Uint32(b[o:]) }
func le16(b []byte, o int) int    { return int(binary.LittleEndian.Uint16(b[o:])) }

// Unpack returns a file's contents, LZ77-decompressed when it's compressed
// (the BIOS's type 0x10, with or without an "LZ77" tag in front), or nil.
func Unpack(d []byte) []byte {
	if len(d) >= 8 && string(d[:4]) == "LZ77" {
		d = d[4:]
	}
	if len(d) < 4 || d[0] != 0x10 {
		return nil
	}
	n := int(le32(d, 0) >> 8)
	if n == 0 || n > 64<<20 {
		return nil
	}
	out := make([]byte, 0, n)
	p := 4
	for len(out) < n && p < len(d) {
		flags := d[p]
		p++
		for bit := 0; bit < 8 && len(out) < n; bit++ {
			if flags&(0x80>>bit) == 0 {
				if p >= len(d) {
					return nil
				}
				out = append(out, d[p])
				p++
				continue
			}
			if p+1 >= len(d) {
				return nil
			}
			l := int(d[p]>>4) + 3
			disp := (int(d[p]&15)<<8 | int(d[p+1])) + 1
			p += 2
			if disp > len(out) {
				return nil
			}
			for k := 0; k < l && len(out) < n; k++ {
				out = append(out, out[len(out)-disp])
			}
		}
	}
	if len(out) != n {
		return nil
	}
	return out
}

// NARC lists the files in a NARC archive (nil if it isn't one).
func NARC(d []byte, path string) []File {
	if len(d) < 16 || string(d[:4]) != "NARC" {
		return nil
	}
	p := le16(d, 0xc)
	var fat [][2]int
	var gmif int = -1
	var names []string
	for blk := 0; blk < 3 && p+8 <= len(d); blk++ {
		tag, size := string(d[p:p+4]), int(le32(d, p+4))
		if size < 8 || p+size > len(d) {
			return nil
		}
		switch tag {
		case "BTAF":
			n := le16(d, p+8)
			for i := 0; i < n && p+12+i*8+8 <= p+size; i++ {
				fat = append(fat, [2]int{int(le32(d, p+12+i*8)), int(le32(d, p+16+i*8))})
			}
		case "BTNF":
			// names of the root folder's files, when the archive has them
			q := p + 8
			if q+8 <= p+size {
				sub := q + int(le32(d, q))
				for sub < p+size {
					l := int(d[sub])
					sub++
					if l == 0 || l&0x80 != 0 || sub+l > p+size {
						break
					}
					names = append(names, string(d[sub:sub+l]))
					sub += l
				}
			}
		case "GMIF":
			gmif = p + 8
		}
		p += size
	}
	if gmif < 0 {
		return nil
	}
	var out []File
	for i, f := range fat {
		s, e := gmif+f[0], gmif+f[1]
		if s > e || e > len(d) {
			continue
		}
		name := ""
		if i < len(names) {
			name = names[i]
		} else {
			name = "file" + itoa(i)
		}
		out = append(out, File{path + "/" + name, d[s:e]})
	}
	return out
}

func itoa(i int) string {
	if i == 0 {
		return "0"
	}
	var b []byte
	for ; i > 0; i /= 10 {
		b = append([]byte{byte('0' + i%10)}, b...)
	}
	return string(b)
}
