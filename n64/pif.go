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

// pakCRC is the check byte the Controller Pak sends with each 32-byte block:
// a CRC-8 with polynomial x^8+x^7+x^2+1 (0x85), starting from zero.
func pakCRC(data []byte) byte {
	var crc byte
	for _, d := range data {
		crc ^= d
		for k := 0; k < 8; k++ {
			top := crc & 0x80
			crc <<= 1
			if top != 0 {
				crc ^= 0x85
			}
		}
	}
	return crc
}

// The CIC-NUS-6105 answers a challenge of 30 nibbles one nibble at a time.
// Each answer nibble is key + 5*challenge (mod 16). That nibble then picks
// the key for the next step from one of two rows of sixteen, and also says
// which row the next step reads. The rows are packed four bits per entry,
// entry n at bits 4n; the switch masks have bit n set when answer n moves to
// (or stays on) the second row.
const (
	cicKeyRow0   = 0x9636f8fc1e5e7a74
	cicKeyRow1   = 0x9c36589c1e5e7a14
	cicNextRow0  = 0x496d // used while on the first row
	cicNextRow1  = 0x036f // used while on the second row
	cicFirstKey  = 0xb
	cicNibbleLen = 30
)

// cicChallenge answers the challenge the game left in PIF RAM 0x30-0x3E.
func (m *Machine) cicChallenge() {
	key := byte(cicFirstKey)
	row := 0
	for i := 0; i < cicNibbleLen; i++ {
		at, shift := 0x30+i/2, uint(4-4*(i&1)) // high nibble first
		q := m.pif[at] >> shift & 15
		a := (key + 5*q) & 15
		m.pif[at] = m.pif[at]&^(15<<shift) | a<<shift
		keys, next := uint64(cicKeyRow0), cicNextRow0
		if row == 1 {
			keys, next = cicKeyRow1, cicNextRow1
		}
		key = byte(keys >> (4 * a) & 15)
		row = next >> a & 1
	}
}

// SetInput sets controller port's buttons (Btn* bits) and analog stick
// (-128..127, up is positive). Ports are 0-3; others are ignored.
func (m *Machine) SetInput(port int, buttons uint16, x, y int8) {
	if port < 0 || port >= len(m.Pad) {
		return
	}
	p := &m.Pad[port]
	p.Buttons, p.X, p.Y = buttons, x, y
}

// ConnectPad plugs a controller into a port (or unplugs it), with or without
// a Controller Pak.
func (m *Machine) ConnectPad(port int, present, pak bool) {
	if port < 0 || port >= len(m.Pad) {
		return
	}
	m.Pad[port].Present, m.Pad[port].Pak = present, pak
}
