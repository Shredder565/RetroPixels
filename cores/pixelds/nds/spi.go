package nds

import "encoding/binary"

// spiBus is the ARM7's SPI bus with its three chips: power management,
// the firmware flash and the touch screen controller.
type spiBus struct {
	m      *Machine
	cnt    uint16
	data   uint8
	doneAt int64

	fw    *firmware
	pos   int // bytes into the current transfer
	dev   int
	touch struct {
		cmd uint8
		val uint16
	}
	pm struct {
		index uint8
		regs  [8]uint8
	}
}

func newSPI(m *Machine, image []byte, user string) *spiBus {
	s := &spiBus{m: m, doneAt: never, fw: newFirmware(image, user)}
	s.pm.regs[0] = 0x0d // sound and both backlights on
	s.pm.regs[4] = 0x03 // backlight level (DS Lite)
	return s
}

func (s *spiBus) writeCnt(v uint16) {
	s.cnt = v & 0xcf03
	dev := int(v >> 8 & 3)
	if v&0x8000 == 0 || dev != s.dev {
		s.deselect()
	}
	s.dev = dev
}

func (s *spiBus) deselect() {
	if s.pos > 0 && s.dev == 1 {
		s.fw.deselect()
	}
	s.pos = 0
}

func (s *spiBus) write(v uint8) {
	if s.cnt&0x8000 == 0 {
		return
	}
	switch s.dev {
	case 0:
		s.data = s.powerMan(v)
	case 1:
		s.data = s.fw.transfer(s.pos, v)
	case 2:
		s.data = s.touchScreen(v)
	default:
		s.data = 0
	}
	s.pos++
	if s.cnt&(1<<11) == 0 { // chip select released after this byte
		s.deselect()
	}
	if s.cnt&(1<<14) != 0 {
		s.m.irq7.raise(irqSPI)
	}
}

func (s *spiBus) finish() { s.doneAt = never }

func (s *spiBus) powerMan(v uint8) uint8 {
	if s.pos == 0 {
		s.pm.index = v
		return 0
	}
	reg := s.pm.index & 7
	if s.pm.index&0x80 != 0 {
		if reg == 1 {
			return 0 // battery fine
		}
		return s.pm.regs[reg]
	}
	s.pm.regs[reg] = v
	if reg == 0 && v&0x40 != 0 {
		// power off: nothing to switch off here
	}
	return 0
}

// touchScreen: the TSC2046. A command picks a channel; the next two bytes
// read its 12-bit value.
func (s *spiBus) touchScreen(v uint8) uint8 {
	var out uint8
	switch s.pos {
	case 0:
	default:
		if s.pos&1 == 1 {
			out = uint8(s.touch.val >> 5)
		} else {
			out = uint8(s.touch.val << 3)
		}
	}
	if v&0x80 != 0 {
		s.touch.cmd = v
		m := s.m
		switch v >> 4 & 7 {
		case 1: // Y
			if m.touching {
				s.touch.val = uint16(m.touchY) << 4
			} else {
				s.touch.val = 0xfff
			}
		case 5: // X
			if m.touching {
				s.touch.val = uint16(m.touchX) << 4
			} else {
				s.touch.val = 0
			}
		case 0: // temperature
			s.touch.val = 0x2c0
		case 6: // microphone: silence
			s.touch.val = 0x800
		default:
			s.touch.val = 0xfff
		}
		if s.pos == 0 {
			s.pos = 0
		}
	}
	return out
}

// ---------------- the firmware flash ----------------

type firmware struct {
	image []byte
	cmd   uint8
	addr  uint32
	wel   bool
}

const fwSize = 256 << 10

// newFirmware makes a 256 KB firmware: the given image, or one made here
// with the user settings a game reads (name, language, touch calibration).
func newFirmware(image []byte, user string) *firmware {
	f := &firmware{image: make([]byte, fwSize)}
	if len(image) >= fwSize {
		copy(f.image, image)
		return f
	}
	img := f.image
	for i := range img {
		img[i] = 0xff
	}
	// header
	copy(img[0x08:], "MAC")
	img[0x0b] = 'P'                                   // console type
	binary.LittleEndian.PutUint16(img[0x20:], 0x7fc0) // user settings at 0x3FE00
	binary.LittleEndian.PutUint16(img[0x22:], 0x0b51) // data
	img[0x1d] = 0xff                                  // DS (not DSi)
	// wifi configuration area: unused but present
	for i := 0x2a; i < 0x200; i++ {
		img[i] = 0
	}
	// user settings, twice
	us := makeUserSettings(user)
	for _, base := range []int{0x3fe00, 0x3ff00} {
		copy(img[base:], us)
	}
	return f
}

func makeUserSettings(user string) []byte {
	b := make([]byte, 0x100)
	for i := range b {
		b[i] = 0xff
	}
	for i := 0; i < 0x74; i++ {
		b[i] = 0
	}
	binary.LittleEndian.PutUint16(b[0x00:], 5) // version
	b[0x02] = 11                               // favourite colour
	b[0x03], b[0x04] = 1, 1                    // birthday
	if user == "" {
		user = "RetroPixels"
	}
	name := []rune(user)
	if len(name) > 10 {
		name = name[:10]
	}
	for i, r := range name {
		binary.LittleEndian.PutUint16(b[0x06+2*i:], uint16(r))
	}
	binary.LittleEndian.PutUint16(b[0x1a:], uint16(len(name)))
	// touch calibration: ADC = pixel * 16
	binary.LittleEndian.PutUint16(b[0x58:], 32*16)
	binary.LittleEndian.PutUint16(b[0x5a:], 32*16)
	b[0x5c], b[0x5d] = 32, 32
	binary.LittleEndian.PutUint16(b[0x5e:], 224*16)
	binary.LittleEndian.PutUint16(b[0x60:], 160*16)
	b[0x62], b[0x63] = 224, 160
	binary.LittleEndian.PutUint16(b[0x64:], 0x0001|0x3<<4) // English, backlight max
	b[0x66] = 0                                            // year 2000 + n (unused)
	binary.LittleEndian.PutUint16(b[0x70:], 1)             // update counter
	binary.LittleEndian.PutUint16(b[0x72:], crc16(0xffff, b[:0x70]))
	return b
}

// userSettings: the 0x70 bytes the boot menu copies to 0x027FFC80.
func (f *firmware) userSettings() []byte {
	off := int(binary.LittleEndian.Uint16(f.image[0x20:])) * 8
	if off <= 0 || off+0x200 > len(f.image) {
		off = 0x3fe00
	}
	a, b := f.image[off:off+0x100], f.image[off+0x100:off+0x200]
	pick := a
	if crc16(0xffff, b[:0x70]) == binary.LittleEndian.Uint16(b[0x72:]) &&
		(crc16(0xffff, a[:0x70]) != binary.LittleEndian.Uint16(a[0x72:]) || b[0x70] > a[0x70]) {
		pick = b
	}
	return pick[:0x70]
}

func crc16(crc uint16, data []byte) uint16 {
	vals := [8]uint16{0xc0c1, 0xc181, 0xc301, 0xc601, 0xcc01, 0xd801, 0xf001, 0xa001}
	for _, d := range data {
		crc ^= uint16(d)
		for j := 0; j < 8; j++ {
			carry := crc&1 != 0
			crc >>= 1
			if carry {
				crc ^= vals[j] << (7 - j)
			}
		}
	}
	return crc
}

func (f *firmware) transfer(pos int, v uint8) uint8 {
	if pos == 0 {
		f.cmd = v
		f.addr = 0
		switch v {
		case 0x06:
			f.wel = true
		case 0x04:
			f.wel = false
		}
		return 0xff
	}
	switch f.cmd {
	case 0x05:
		if f.wel {
			return 2
		}
		return 0
	case 0x9f:
		return [3]uint8{0x20, 0x40, 0x12}[(pos-1)%3]
	case 0x03, 0x0b, 0x0a, 0x02:
		if pos <= 3 {
			f.addr = f.addr<<8 | uint32(v)
			return 0xff
		}
		if f.cmd == 0x0b && pos == 4 {
			return 0xff
		}
		a := f.addr % fwSize
		f.addr++
		if f.cmd == 0x0a || f.cmd == 0x02 {
			if f.wel {
				f.image[a] = v
			}
			return 0xff
		}
		return f.image[a]
	}
	return 0xff
}

func (f *firmware) deselect() {
	if f.cmd == 0x0a || f.cmd == 0x02 {
		f.wel = false
	}
}
