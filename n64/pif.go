package n64

// PIF: runs the joybus commands the game leaves in PIF RAM (controllers,
// Controller Pak, EEPROM) and answers the CIC-6105 challenge.

// N64 button bits (as returned by the controller "read" command)
const (
	BtnA      = 0x8000
	BtnB      = 0x4000
	BtnZ      = 0x2000
	BtnStart  = 0x1000
	BtnDUp    = 0x0800
	BtnDDown  = 0x0400
	BtnDLeft  = 0x0200
	BtnDRight = 0x0100
	BtnL      = 0x0020
	BtnR      = 0x0010
	BtnCUp    = 0x0008
	BtnCDown  = 0x0004
	BtnCLeft  = 0x0002
	BtnCRight = 0x0001
)

func (m *Machine) pifCommand() {
	ctl := m.pif[63]
	if ctl&0x02 != 0 { // CIC challenge
		m.cicChallenge()
		m.pif[63] &^= 0x02
	}
	if ctl&0x08 != 0 { // boot done
		m.pif[63] &^= 0x08
	}
	if ctl&0x01 == 0 {
		return
	}
	// walk the command list
	ch := 0
	i := 0
	for i < 63 && ch < 6 {
		tx := int(m.pif[i])
		if tx == 0xfe {
			break
		}
		if tx == 0xff || tx == 0xfd { // padding
			i++
			continue
		}
		if tx == 0 { // skip this channel
			ch++
			i++
			continue
		}
		if i+1 >= 63 {
			break
		}
		rxi := i + 1
		rx := int(m.pif[rxi] & 0x3f)
		txs := i + 2
		rxs := txs + (tx & 0x3f)
		if rxs+rx > 64 {
			break
		}
		m.joybus(ch, m.pif[txs:txs+(tx&0x3f)], m.pif[rxs:rxs+rx], rxi)
		i = rxs + rx
		ch++
	}
	m.pif[63] &^= 0x01
}

func (m *Machine) joybus(ch int, tx, rx []byte, rxi int) {
	if len(tx) == 0 {
		return
	}
	cmd := tx[0]
	notPresent := func() { m.pif[rxi] |= 0x80 }
	if ch < 4 {
		pad := &m.Pad[ch]
		if !pad.Present {
			notPresent()
			return
		}
		switch cmd {
		case 0x00, 0xff: // status
			if len(rx) >= 3 {
				rx[0], rx[1] = 0x05, 0x00
				if pad.Pak {
					rx[2] = 0x01
				} else {
					rx[2] = 0x02
				}
			}
		case 0x01: // buttons + stick
			if len(rx) >= 4 {
				rx[0], rx[1] = byte(pad.Buttons>>8), byte(pad.Buttons)
				rx[2], rx[3] = byte(pad.X), byte(pad.Y)
			}
		case 0x02: // Controller Pak read
			if len(tx) >= 3 && len(rx) >= 33 {
				addr := (uint32(tx[1])<<8 | uint32(tx[2])) &^ 0x1f
				m.Save.pakRead(ch, addr, rx[:32])
				rx[32] = pakCRC(rx[:32])
			}
		case 0x03: // Controller Pak write
			if len(tx) >= 35 && len(rx) >= 1 {
				addr := (uint32(tx[1])<<8 | uint32(tx[2])) &^ 0x1f
				m.Save.pakWrite(ch, addr, tx[3:35])
				rx[0] = pakCRC(tx[3:35])
			}
		default:
			notPresent()
		}
		return
	}
	if ch == 4 { // EEPROM
		sv := m.Save
		if sv.eepromSize == 0 {
			notPresent()
			return
		}
		switch cmd {
		case 0x00, 0xff:
			if len(rx) >= 3 {
				rx[0] = 0x00
				if sv.eepromSize == 2048 {
					rx[1] = 0xc0
				} else {
					rx[1] = 0x80
				}
				rx[2] = 0x00
			}
		case 0x04:
			if len(tx) >= 2 && len(rx) >= 8 {
				sv.eepromRead(int(tx[1]), rx[:8])
			}
		case 0x05:
			if len(tx) >= 10 {
				sv.eepromWrite(int(tx[1]), tx[2:10])
				if len(rx) >= 1 {
					rx[0] = 0
				}
			}
		default:
			notPresent()
		}
		return
	}
	notPresent()
}

func pakCRC(data []byte) byte {
	crc := byte(0)
	for i := 0; i <= len(data); i++ {
		for b := 7; b >= 0; b-- {
			x := byte(0)
			if crc&0x80 != 0 {
				x = 0x85
			}
			crc <<= 1
			if i < len(data) && data[i]&(1<<uint(b)) != 0 {
				crc |= 1
			}
			crc ^= x
		}
	}
	return crc
}

// cicChallenge answers the CIC-NUS-6105 challenge stored in PIF RAM 0x30-0x3E
// (30 nibbles), as the security chip does.
func (m *Machine) cicChallenge() {
	var ch, resp [30]byte
	for i := 0; i < 15; i++ {
		ch[i*2] = m.pif[0x30+i] >> 4
		ch[i*2+1] = m.pif[0x30+i] & 15
	}
	lut0 := [16]byte{0x4, 0x7, 0xA, 0x7, 0xE, 0x5, 0xE, 0x1, 0xC, 0xF, 0x8, 0xF, 0x6, 0x3, 0x6, 0x9}
	lut1 := [16]byte{0x4, 0x1, 0xA, 0x7, 0xE, 0x5, 0xE, 0x1, 0xC, 0x9, 0x8, 0x5, 0x6, 0x3, 0xC, 0x9}
	key := byte(0xb)
	useLut1 := false
	for i := 0; i < 30; i++ {
		r := (key + 5*ch[i]) & 15
		resp[i] = r
		if useLut1 {
			key = lut1[r]
		} else {
			key = lut0[r]
		}
		sgn := (r >> 3) & 1
		mag := r
		if sgn == 1 {
			mag = ^r
		}
		mag &= 7
		mod := byte(1) - sgn
		if mag%3 == 1 {
			mod = sgn
		}
		if useLut1 && (r == 0x1 || r == 0x9) {
			mod = 1
		}
		if useLut1 && (r == 0xb || r == 0xe) {
			mod = 0
		}
		useLut1 = mod == 1
	}
	for i := 0; i < 15; i++ {
		m.pif[0x30+i] = resp[i*2]<<4 | resp[i*2+1]
	}
}
