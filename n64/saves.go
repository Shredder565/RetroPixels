package n64

import "errors"

// Cartridge save memory (EEPROM, SRAM, FlashRAM) and Controller Paks.

// Games whose EEPROM is the 16 Kbit size (by the header's game code).
var eeprom16k = map[string]bool{
	"NB7": true, "NGT": true, "NFU": true, "NCW": true, "NCZ": true, "ND6": true, "NDO": true, "ND2": true, "N3D": true,
	"NMX": true, "NGC": true, "NIM": true, "NNB": true, "NMV": true, "NM8": true, "NEV": true, "NPP": true, "NUB": true,
	"NPD": true, "NRZ": true, "NR7": true, "NEP": true, "NYS": true,
}

// Games that use SRAM / FlashRAM / no EEPROM, by game code.
var saveSRAM = map[string]bool{
	"NAL": true, "NB5": true, "CFZ": true, "NFZ": true, "NKG": true, "NMF": true, "NRI": true, "NUT": true, "NYW": true,
	"NZL": true, "CZL": true, "NTE": true, "NVB": true, "NWL": true, "NPA": true, "NPF": true, "NPO": true, "NG6": true,
	"NOB": true, "NSI": true, "NTW": true, "NWX": true, "N3H": true, "NGP": true, "NTP": true, "NJF": true,
}
var saveFlash = map[string]bool{
	"NCC": true, "NDA": true, "NAF": true, "NJF": false, "NKJ": true, "NZS": true, "NM6": true, "NCK": true, "NMQ": true,
	"NPN": true, "NSQ": true, "NT9": true, "NW4": true, "NDP": true, "NP3": true, "NRH": true, "NPS": true, "NPQ": true,
	"CP2": true, "NP2": true, "NYS": false, "NMG": true, "NB8": true, "NRR": true,
}

// Saves holds all of a game's save memory.
type Saves struct {
	eeprom     []byte
	eepromSize int
	sram       []byte // 32 KB SRAM or 128 KB FlashRAM
	flash      bool
	hasSRAM    bool // the game is known to use SRAM
	paks       [4][]byte
	dirty      bool

	// FlashRAM state
	fMode   int // 0 read, 1 status, 2 erase, 3 write
	fStatus uint32
	fOffset uint32
	fBuf    [128]byte
}

func newSaves(rom []byte) *Saves {
	id := string(rom[0x3b:0x3e])
	s := &Saves{}
	s.eepromSize = 512
	if eeprom16k[id] {
		s.eepromSize = 2048
	}
	s.eeprom = filled(s.eepromSize, 0xff)
	s.flash = saveFlash[id]
	s.hasSRAM = saveSRAM[id]
	if s.flash {
		s.sram = filled(0x20000, 0xff)
	} else {
		s.sram = filled(0x20000, 0xff)
	}
	for i := range s.paks {
		s.paks[i] = make([]byte, 0x8000)
		formatPak(s.paks[i])
	}
	return s
}

func filled(n int, v byte) []byte {
	b := make([]byte, n)
	for i := range b {
		b[i] = v
	}
	return b
}

// ---- EEPROM ----
func (s *Saves) eepromRead(block int, out []byte) {
	o := block * 8
	if o+8 <= len(s.eeprom) {
		copy(out, s.eeprom[o:o+8])
	}
}
func (s *Saves) eepromWrite(block int, in []byte) {
	o := block * 8
	if o+8 <= len(s.eeprom) {
		copy(s.eeprom[o:o+8], in)
		s.dirty = true
	}
}

// ---- SRAM / FlashRAM (PI domain 2) ----

// dmaRead copies save memory into dst for a PI DMA starting at cart address addr.
func (s *Saves) dmaRead(addr uint32, dst []byte) {
	if s.flash {
		if s.fMode == 1 {
			for i := range dst {
				w := s.flashStatusWord(addr + uint32(i)&^3)
				dst[i] = byte(w >> ((3 - ((addr + uint32(i)) & 3)) * 8))
			}
			return
		}
		// FlashRAM is addressed in 16-bit units on the cartridge bus
		o := (addr - 0x08000000) * 2
		for i := range dst {
			if int(o)+i < len(s.sram) {
				dst[i] = s.sram[int(o)+i]
			} else {
				dst[i] = 0
			}
		}
		return
	}
	o := int(addr - 0x08000000)
	for i := range dst {
		if o+i < len(s.sram) {
			dst[i] = s.sram[o+i]
		} else {
			dst[i] = 0
		}
	}
}

// dmaWrite stores a PI DMA into save memory.
func (s *Saves) dmaWrite(addr uint32, src []byte) {
	if s.flash {
		if s.fMode == 3 {
			for i, v := range src {
				s.fBuf[(int(addr-0x08000000)+i)&127] = v
			}
		}
		return
	}
	o := int(addr - 0x08000000)
	for i, v := range src {
		if o+i < len(s.sram) && s.sram[o+i] != v {
			s.sram[o+i] = v
			s.dirty = true
		}
	}
}

func (s *Saves) flashStatusWord(addr uint32) uint32 {
	if addr&4 == 0 {
		return 0x11118001
	}
	return 0x00c2001e
}

func (s *Saves) cartRead32(addr uint32) uint32 {
	if s.flash {
		return s.flashStatusWord(addr)
	}
	var b [4]byte
	s.dmaRead(addr, b[:])
	return uint32(b[0])<<24 | uint32(b[1])<<16 | uint32(b[2])<<8 | uint32(b[3])
}

func (s *Saves) cartWrite32(addr uint32, v uint32) {
	if addr == 0x08010000 { // FlashRAM command register
		s.flash = true
		s.flashCommand(v)
		return
	}
	s.dmaWrite(addr, []byte{byte(v >> 24), byte(v >> 16), byte(v >> 8), byte(v)})
}

func (s *Saves) flashCommand(v uint32) {
	switch v >> 24 {
	case 0x4b: // pick the sector to erase
		s.fOffset = (v & 0xffff) * 128
		s.fMode = 2
	case 0x78: // start the erase
		s.fStatus = 0x08
		s.flashExecute()
	case 0xb4: // write buffer mode
		s.fMode = 3
	case 0xa5: // pick the page and program it from the buffer
		s.fOffset = (v & 0xffff) * 128
		s.fStatus = 0x04
		s.flashExecute()
	case 0xd2: // run the pending erase or program again (harmless)
		s.flashExecute()
	case 0xe1:
		s.fMode = 1
	case 0xf0:
		s.fMode = 0
	case 0x3c: // chip erase
		for i := range s.sram {
			s.sram[i] = 0xff
		}
		s.dirty = true
	}
}

// flashExecute does the erase or page program the current mode asks for.
func (s *Saves) flashExecute() {
	switch s.fMode {
	case 2: // erase a 16 KB sector
		start := s.fOffset &^ 0x3fff
		for i := start; i < start+0x4000 && int(i) < len(s.sram); i++ {
			s.sram[i] = 0xff
		}
		s.dirty = true
	case 3: // write the 128-byte buffer
		for i := uint32(0); i < 128; i++ {
			if int(s.fOffset+i) < len(s.sram) {
				s.sram[s.fOffset+i] = s.fBuf[i]
			}
		}
		s.dirty = true
	}
}

// ---- Controller Pak ----
func (s *Saves) pakRead(ch int, addr uint32, out []byte) {
	if addr >= 0x8000 { // rumble / transfer pak area: nothing there
		for i := range out {
			out[i] = 0
		}
		return
	}
	copy(out, s.paks[ch][addr:addr+32])
}
func (s *Saves) pakWrite(ch int, addr uint32, in []byte) {
	if addr >= 0x8000 {
		return
	}
	copy(s.paks[ch][addr:addr+32], in)
	s.dirty = true
}

// formatPak writes an empty, formatted Controller Pak file system.
func formatPak(p []byte) {
	for i := range p {
		p[i] = 0
	}
	// ID block (copies at 0x20, 0x60, 0x80, 0xC0)
	id := make([]byte, 32)
	for i := 0; i < 24; i++ {
		id[i] = byte(i * 7)
	}
	id[25] = 0x01 // device id
	id[26] = 0x01 // bank count
	id[27] = 0x00 // version
	sum := uint16(0)
	for i := 0; i < 28; i += 2 {
		sum += uint16(id[i])<<8 | uint16(id[i+1])
	}
	id[28], id[29] = byte(sum>>8), byte(sum)
	inv := 0xfff2 - sum
	id[30], id[31] = byte(inv>>8), byte(inv)
	for _, o := range []int{0x20, 0x60, 0x80, 0xc0} {
		copy(p[o:], id)
	}
	// index tables (pages 1 and 2): entries 5..127 free (0x0003)
	for _, base := range []int{0x100, 0x200} {
		for e := 5; e < 128; e++ {
			p[base+e*2] = 0x00
			p[base+e*2+1] = 0x03
		}
		c := byte(0)
		for e := 5; e < 128; e++ {
			c += p[base+e*2+1]
		}
		p[base+1] = c
	}
}

// Export packs all save memory into one blob (for battery-save storage):
// "RP64", the EEPROM size and data, the FlashRAM flag, the 128 KB SRAM /
// FlashRAM area and the four Controller Paks.
func (s *Saves) Export() []byte {
	out := make([]byte, 0, 8+len(s.eeprom)+len(s.sram)+4*0x8000)
	out = append(out, 'R', 'P', '6', '4')
	out = append(out, byte(len(s.eeprom)>>8), byte(len(s.eeprom)))
	out = append(out, s.eeprom...)
	out = append(out, boolByte(s.flash), 0)
	out = append(out, s.sram...)
	for _, p := range s.paks {
		out = append(out, p...)
	}
	return out
}

// Import restores Export's blob, or a plain .eep / .sra / .fla / .mpk file.
func (s *Saves) Import(b []byte) error {
	if len(b) >= 6 && string(b[:4]) == "RP64" {
		n := int(b[4])<<8 | int(b[5])
		if n != 512 && n != 2048 {
			return errors.New("the save file is damaged")
		}
		need := 6 + n + 2 + 0x20000 + 4*0x8000
		if len(b) != need {
			return errors.New("the save file is damaged")
		}
		p := 6
		s.eepromSize = n
		s.eeprom = append([]byte(nil), b[p:p+n]...)
		p += n
		s.flash = b[p] == 1
		p += 2
		copy(s.sram, b[p:p+0x20000])
		p += 0x20000
		for i := range s.paks {
			copy(s.paks[i], b[p:p+0x8000])
			p += 0x8000
		}
		s.dirty = false
		return nil
	}
	switch len(b) {
	case 512, 2048: // EEPROM
		s.eepromSize = len(b)
		s.eeprom = append([]byte(nil), b...)
	case 0x8000: // SRAM, or a Controller Pak for games without SRAM
		if s.hasSRAM {
			copy(s.sram, b)
		} else {
			copy(s.paks[0], b)
		}
	case 0x20000: // FlashRAM (or 128 KB SRAM)
		copy(s.sram, b)
	default:
		return errors.New("that doesn't look like a Nintendo 64 save file")
	}
	return nil
}

func boolByte(b bool) byte {
	if b {
		return 1
	}
	return 0
}
