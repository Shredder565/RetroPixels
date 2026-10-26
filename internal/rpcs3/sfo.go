package rpcs3

// PARAM.SFO: the small table of facts every PS3 game folder carries (its
// name, title ID, kind…). The layout: a 20-byte header ("\0PSF", version,
// where the key names start, where the values start, how many entries),
// then one 16-byte index entry per key (where its name is, its format,
// how long its value is, room kept for it, where it is), all little-endian.

import (
	"encoding/binary"
	"errors"
	"os"
	"sort"
	"strings"
)

// SFO is a PARAM.SFO's entries: text values as string, numbers as uint32.
type SFO map[string]any

// String is a text entry ("" when it's missing or a number).
func (s SFO) String(key string) string {
	v, _ := s[key].(string)
	return v
}

// Int is a number entry (0 when it's missing or text).
func (s SFO) Int(key string) uint32 {
	v, _ := s[key].(uint32)
	return v
}

const (
	sfoUTF8Special = 0x0004 // text without its ending zero
	sfoUTF8        = 0x0204 // text ending in a zero
	sfoInt32       = 0x0404
)

var errNotSFO = errors.New("not a PARAM.SFO file")

// ParseSFO reads the entries of a PARAM.SFO.
func ParseSFO(b []byte) (SFO, error) {
	if len(b) < 20 || string(b[:4]) != "\x00PSF" {
		return nil, errNotSFO
	}
	le := binary.LittleEndian
	keys, data, n := int(le.Uint32(b[8:])), int(le.Uint32(b[12:])), int(le.Uint32(b[16:]))
	if keys > len(b) || data > len(b) || n > (len(b)-20)/16 {
		return nil, errNotSFO
	}
	out := SFO{}
	for i := 0; i < n; i++ {
		e := b[20+16*i:]
		ko, fmtc, dlen := keys+int(le.Uint16(e[0:])), le.Uint16(e[2:]), int(le.Uint32(e[4:]))
		do := data + int(le.Uint32(e[12:]))
		if ko >= len(b) || do > len(b) || dlen < 0 || do+dlen > len(b) {
			return nil, errNotSFO
		}
		name := b[ko:]
		if z := indexZero(name); z >= 0 {
			name = name[:z]
		}
		val := b[do : do+dlen]
		switch fmtc {
		case sfoInt32:
			if len(val) < 4 {
				return nil, errNotSFO
			}
			out[string(name)] = le.Uint32(val)
		case sfoUTF8, sfoUTF8Special:
			if z := indexZero(val); z >= 0 {
				val = val[:z]
			}
			out[string(name)] = strings.TrimSpace(string(val))
		}
	}
	return out, nil
}

func indexZero(b []byte) int {
	for i, c := range b {
		if c == 0 {
			return i
		}
	}
	return -1
}

// ReadSFO reads a PARAM.SFO file.
func ReadSFO(path string) (SFO, error) {
	b, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	if len(b) > 1<<20 {
		return nil, errNotSFO
	}
	return ParseSFO(b)
}

// BuildSFO makes a PARAM.SFO from entries (string or uint32 values), keys
// in sorted order as the PS3 keeps them. For tests and tools.
func BuildSFO(entries map[string]any) []byte {
	var names []string
	for k := range entries {
		names = append(names, k)
	}
	sort.Strings(names)
	le := binary.LittleEndian
	var keyTab, dataTab []byte
	index := make([]byte, 16*len(names))
	for i, k := range names {
		e := index[16*i:]
		le.PutUint16(e[0:], uint16(len(keyTab)))
		keyTab = append(append(keyTab, k...), 0)
		var val []byte
		switch v := entries[k].(type) {
		case uint32:
			le.PutUint16(e[2:], sfoInt32)
			val = le.AppendUint32(nil, v)
			le.PutUint32(e[4:], 4)
			le.PutUint32(e[8:], 4)
		case string:
			le.PutUint16(e[2:], sfoUTF8)
			val = append([]byte(v), 0)
			room := (len(val) + 3) &^ 3
			le.PutUint32(e[4:], uint32(len(val)))
			le.PutUint32(e[8:], uint32(room))
			val = append(val, make([]byte, room-len(val))...)
		}
		le.PutUint32(e[12:], uint32(len(dataTab)))
		dataTab = append(dataTab, val...)
	}
	for len(keyTab)%4 != 0 {
		keyTab = append(keyTab, 0)
	}
	head := make([]byte, 20)
	copy(head, "\x00PSF")
	le.PutUint32(head[4:], 0x101)
	le.PutUint32(head[8:], uint32(20+len(index)))
	le.PutUint32(head[12:], uint32(20+len(index)+len(keyTab)))
	le.PutUint32(head[16:], uint32(len(names)))
	return append(append(append(head, index...), keyTab...), dataTab...)
}
