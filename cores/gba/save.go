package gba

import (
	"bytes"
	"regexp"
	"time"
)

// Cartridge save memory (SRAM, Flash, EEPROM) and the real-time clock, as in
// the Save and RTC classes of web/emu/gba.js.

const (
	saveNone = iota
	saveEEPROM
	saveFlash128
	saveFlash64
	saveSRAM
)

var rtcID = regexp.MustCompile(`SIIRTC_V\d\d\d`)

type saveMem struct {
	typ        int
	data       []byte
	eepromSize int // 512 or 8192 once known
	dirty      bool
	rtc        bool // the game's code asks for the clock chip

	fState, fCmd, fBank int
	fID                 bool

	eBits    []byte
	eRead    []byte
	eReadPos int
}

func newSave(rom []byte) *saveMem {
	s := &saveMem{}
	switch {
	case bytes.Contains(rom, []byte("EEPROM_V")):
		s.typ = saveEEPROM
	case bytes.Contains(rom, []byte("FLASH1M_V")):
		s.typ = saveFlash128
	case bytes.Contains(rom, []byte("FLASH_V")) || bytes.Contains(rom, []byte("FLASH512_V")):
		s.typ = saveFlash64
	case bytes.Contains(rom, []byte("SRAM_V")) || bytes.Contains(rom, []byte("SRAM_F_V")):
		s.typ = saveSRAM
	}
	s.rtc = rtcID.Match(rom)
	size := map[int]int{saveEEPROM: 0x2000, saveFlash128: 0x20000, saveFlash64: 0x10000, saveSRAM: 0x8000, saveNone: 0x8000}[s.typ]
	s.data = bytes.Repeat([]byte{0xff}, size)
	return s
}

func (s *saveMem) load(b []byte) {
	if s.typ == saveEEPROM {
		if len(b) <= 512 {
			s.eepromSize = 512
		} else {
			s.eepromSize = 8192
		}
	}
	copy(s.data, b)
}

func (s *saveMem) bytes() []byte {
	if s.typ == saveEEPROM {
		n := s.eepromSize
		if n == 0 {
			n = 8192
		}
		return append([]byte(nil), s.data[:n]...)
	}
	return append([]byte(nil), s.data...)
}

func (s *saveMem) isFlash() bool { return s.typ == saveFlash64 || s.typ == saveFlash128 }

// ---- SRAM / Flash (0x0E000000) ----

func (s *saveMem) read8(a uint32) uint32 {
	a &= 0xffff
	if s.isFlash() {
		if s.fID && a < 2 {
			if s.typ == saveFlash128 {
				if a != 0 {
					return 0x09
				}
				return 0xc2
			}
			if a != 0 {
				return 0x1b
			}
			return 0x32
		}
		return uint32(s.data[s.fBank*0x10000+int(a)])
	}
	if s.typ == saveSRAM || s.typ == saveNone {
		return uint32(s.data[a&0x7fff])
	}
	return 0xff
}

func (s *saveMem) write8(a, v uint32) {
	a &= 0xffff
	if s.typ == saveSRAM || s.typ == saveNone {
		if s.typ == saveNone {
			s.typ = saveSRAM
		}
		if uint32(s.data[a&0x7fff]) != v {
			s.data[a&0x7fff] = byte(v)
			s.dirty = true
		}
		return
	}
	if !s.isFlash() {
		return
	}
	if s.fCmd == 0xa0 {
		s.data[s.fBank*0x10000+int(a)] &= byte(v)
		s.fCmd = 0
		s.dirty = true
		return
	}
	if s.fCmd == 0xb0 && a == 0 {
		if s.typ == saveFlash128 {
			s.fBank = int(v & 1)
		} else {
			s.fBank = 0
		}
		s.fCmd = 0
		return
	}
	if s.fState == 0 && a == 0x5555 && v == 0xaa {
		s.fState = 1
		return
	}
	if s.fState == 1 && a == 0x2aaa && v == 0x55 {
		s.fState = 2
		return
	}
	if s.fState == 2 {
		s.fState = 0
		if s.fCmd == 0x80 && v == 0x30 { // erase 4 KB sector
			base := s.fBank*0x10000 + int(a&0xf000)
			fill(s.data[base:base+0x1000], 0xff)
			s.fCmd = 0
			s.dirty = true
			return
		}
		if a != 0x5555 {
			return
		}
		switch v {
		case 0x90:
			s.fID = true
		case 0xf0:
			s.fID = false
		case 0x80:
			s.fCmd = 0x80
		case 0x10:
			if s.fCmd == 0x80 {
				fill(s.data, 0xff)
				s.dirty = true
			}
			s.fCmd = 0
		case 0xa0:
			s.fCmd = 0xa0
		case 0xb0:
			s.fCmd = 0xb0
		}
		return
	}
	s.fState = 0
}

func fill(b []byte, v byte) {
	for i := range b {
		b[i] = v
	}
}

// ---- EEPROM (serial, through DMA 3) ----

func (s *saveMem) addrBits() int {
	switch s.eepromSize {
	case 512:
		return 6
	case 8192:
		return 14
	}
	return 0
}

func (s *saveMem) eepromWrite(bit uint32) {
	s.eBits = append(s.eBits, byte(bit&1))
	n, b := len(s.eBits), s.eBits
	if n < 2 {
		return
	}
	cmd := int(b[0])<<1 | int(b[1])
	switch cmd {
	case 3: // read request: 2 + addr + 1
		if ab := s.addrBits(); ab != 0 && n == 2+ab+1 {
			s.eepromStartRead(ab)
		}
	case 2: // write: 2 + addr + 64 + 1
		if ab := s.addrBits(); ab != 0 && n == 2+ab+64+1 {
			s.eepromDoWrite(ab)
		}
	default:
		s.eBits = s.eBits[:0]
	}
}

// eepromDmaDone: a DMA to the EEPROM finished; its length tells the chip size.
func (s *saveMem) eepromDmaDone(units int) {
	if s.typ != saveEEPROM {
		return
	}
	if s.eepromSize == 0 {
		switch units {
		case 9, 73:
			s.eepromSize = 512
		case 17, 81:
			s.eepromSize = 8192
		default:
			return
		}
		// re-run the bits now that the size is known
		bits := s.eBits
		s.eBits = nil
		for _, x := range bits {
			s.eepromWrite(uint32(x))
		}
	}
}

func (s *saveMem) eAddr(ab int) int {
	a := 0
	for i := 0; i < ab; i++ {
		a = a<<1 | int(s.eBits[2+i])
	}
	if s.eepromSize == 512 {
		return a & 0x3f
	}
	return a & 0x3ff
}

func (s *saveMem) eepromStartRead(ab int) {
	a := s.eAddr(ab) * 8
	s.eRead = append(s.eRead[:0], 0, 0, 0, 0)
	for i := 0; i < 64; i++ {
		s.eRead = append(s.eRead, s.data[a+i>>3]>>(7-i&7)&1)
	}
	s.eReadPos = 0
	s.eBits = s.eBits[:0]
}

func (s *saveMem) eepromDoWrite(ab int) {
	a := s.eAddr(ab) * 8
	for i := 0; i < 8; i++ {
		v := byte(0)
		for k := 0; k < 8; k++ {
			v = v<<1 | s.eBits[2+ab+i*8+k]
		}
		s.data[a+i] = v
	}
	s.dirty = true
	s.eBits = s.eBits[:0]
	s.eRead = s.eRead[:0]
	s.eReadPos = 0
}

func (s *saveMem) eepromRead() uint32 {
	if s.eReadPos < len(s.eRead) {
		s.eReadPos++
		return uint32(s.eRead[s.eReadPos-1])
	}
	return 1 // ready
}

func (s *saveMem) state(st *sio) {
	st.Int(&s.typ)
	st.Bytes(s.data)
	st.Ints(&s.eepromSize, &s.fState, &s.fCmd, &s.fBank)
	st.Bools(&s.fID, &s.dirty)
	st.VarBytes(&s.eBits)
	st.VarBytes(&s.eRead)
	st.Int(&s.eReadPos)
}

// ===================================================================
// Real-time clock (Seiko S-3511 style, on the cartridge GPIO port)
// ===================================================================

type rtcChip struct {
	now                                 func() time.Time
	dir, ctrl, pins, state, bits, nbits int
	cmd, status                         int
	out                                 []byte
}

func newRTC(now func() time.Time) *rtcChip { return &rtcChip{now: now, status: 0x40} }

func bcd(v int) int { return v/10*16 + v%10 }

func (r *rtcChip) timeBytes(withDate bool) []int {
	d := r.now()
	h := d.Hour()
	var hb int
	if r.status&0x40 != 0 {
		hb = bcd(h)
	} else {
		hb = bcd(h % 12)
		if h >= 12 {
			hb |= 0x80
		}
	}
	t := []int{hb, bcd(d.Minute()), bcd(d.Second())}
	if withDate {
		return append([]int{bcd(d.Year() % 100), bcd(int(d.Month())), bcd(d.Day()), int(d.Weekday())}, t...)
	}
	return t
}

func (r *rtcChip) write(v int) {
	old := r.pins
	r.pins = r.pins&^r.dir | v&r.dir
	sck, sio, cs := r.pins&1, r.pins>>1&1, r.pins>>2&1
	if cs == 0 {
		r.state, r.nbits, r.bits = 0, 0, 0
		return
	}
	if old&4 == 0 { // chip select rose
		r.state, r.nbits, r.bits = 1, 0, 0
		return
	}
	if old&1 != 0 && sck == 0 {
		return
	}
	if old&1 == 0 && sck != 0 { // rising edge
		switch r.state {
		case 1: // command byte "0110 ccc r", sent MSB first (or LSB first by some games)
			r.bits = (r.bits<<1 | sio) & 0xff
			r.nbits++
			if r.nbits == 8 {
				c := r.bits
				if c>>4 != 6 {
					rv := 0
					for i := 0; i < 8; i++ {
						rv |= (c >> i & 1) << (7 - i)
					}
					c = rv
				}
				r.cmd = c >> 1 & 7
				read := c & 1
				r.nbits, r.bits = 0, 0
				if read != 0 {
					var bs []int
					switch r.cmd {
					case 2:
						bs = r.timeBytes(true)
					case 3:
						bs = r.timeBytes(false)
					case 1:
						bs = []int{r.status}
					}
					r.out = r.out[:0]
					for _, b := range bs {
						for i := 0; i < 8; i++ {
							r.out = append(r.out, byte(b>>i&1))
						}
					}
					r.state = 2
				} else {
					if r.cmd == 0 {
						r.state = 0
						r.status = 0x40
					} else {
						r.state = 3
					}
				}
			}
		case 2:
			bit := 1
			if len(r.out) > 0 {
				bit = int(r.out[0])
				r.out = r.out[1:]
			}
			r.pins = r.pins&^2 | bit<<1
		case 3:
			r.bits |= sio << r.nbits
			r.nbits++
			if r.nbits == 8 {
				if r.cmd == 1 {
					r.status = r.bits
				}
				r.nbits, r.bits = 0, 0
			}
		}
	} else if r.state == 2 && sck == 0 && len(r.out) > 0 {
		r.pins = r.pins&^2 | int(r.out[0])<<1
	}
}

func (r *rtcChip) stateIO(s *sio) {
	s.Ints(&r.dir, &r.ctrl, &r.pins, &r.state, &r.bits, &r.nbits, &r.cmd, &r.status)
	s.VarBytes(&r.out)
}
