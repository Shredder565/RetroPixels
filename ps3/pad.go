package ps3

// sys_io: the controller (one DUALSHOCK 3 / SIXAXIS).

// Pad buttons (bits of SetPad).
const (
	PadSelect = 1 << iota
	PadL3
	PadR3
	PadStart
	PadUp
	PadRight
	PadDown
	PadLeft
	PadL2
	PadR2
	PadL1
	PadR1
	PadTriangle
	PadCircle
	PadCross
	PadSquare
)

type padState struct {
	inited  bool
	buttons uint32
	sticks  [4]uint8 // left X, left Y, right X, right Y (0..255, 128 = centre)
}

// SetPad sets the controller: buttons (Pad* bits) and sticks (-1..1, down +).
func (m *Machine) SetPad(buttons uint32, lx, ly, rx, ry float32) {
	m.pad.buttons = buttons
	conv := func(v float32) uint8 {
		x := int(128 + v*127.5)
		return uint8(max(0, min(255, x)))
	}
	m.pad.sticks = [4]uint8{conv(lx), conv(ly), conv(rx), conv(ry)}
}

func init() {
	reg("cellPadInit", func(m *Machine, t *Thread) {
		m.pad.inited = true
		m.pad.sticks = [4]uint8{128, 128, 128, 128}
		t.ret(0)
	})
	reg("cellPadEnd", func(m *Machine, t *Thread) { m.pad.inited = false; t.ret(0) })
	reg("cellPadGetInfo2", func(m *Machine, t *Thread) {
		a := t.arg(0)
		m.Mem.Fill(a, 0, 0x7c)
		m.Mem.Write32(a, 7)   // max
		m.Mem.Write32(a+4, 1) // connected
		m.Mem.Write32(a+8, 0)
		m.Mem.Write32(a+12, 1)      // port 0 connected
		m.Mem.Write32(a+12+28, 0)   // port setting
		m.Mem.Write32(a+12+56, 0x7) // capability: PS3 conformity, press, sensor
		m.Mem.Write32(a+12+84, 0)   // standard
		t.ret(0)
	})
	reg("cellPadGetData", func(m *Machine, t *Thread) { // (port, data*)
		d := t.arg(1)
		m.Mem.Fill(d, 0, 4+64*2)
		if t.arg(0) != 0 {
			t.ret(0)
			return
		}
		b := m.pad.buttons
		m.Mem.Write32(d, 24)
		m.Mem.Write16(d+4+2*1, 0x0012) // len hi nibble: 2
		m.Mem.Write16(d+4+2*2, uint16(b&0xff))
		m.Mem.Write16(d+4+2*3, uint16(b>>8&0xff))
		m.Mem.Write16(d+4+2*4, uint16(m.pad.sticks[2]))
		m.Mem.Write16(d+4+2*5, uint16(m.pad.sticks[3]))
		m.Mem.Write16(d+4+2*6, uint16(m.pad.sticks[0]))
		m.Mem.Write16(d+4+2*7, uint16(m.pad.sticks[1]))
		press := func(bit uint32) uint16 {
			if b&bit != 0 {
				return 255
			}
			return 0
		}
		// pressure: right left up down triangle circle cross square L1 R1 L2 R2
		for i, bit := range []uint32{PadRight, PadLeft, PadUp, PadDown, PadTriangle, PadCircle, PadCross, PadSquare, PadL1, PadR1, PadL2, PadR2} {
			m.Mem.Write16(d+4+2*uint32(8+i), press(bit))
		}
		for i := uint32(20); i < 24; i++ {
			m.Mem.Write16(d+4+2*i, 512)
		}
		t.ret(0)
	})
	reg("cellPadSetPortSetting", func(m *Machine, t *Thread) { t.ret(0) })
	reg("cellPadSetActDirect", func(m *Machine, t *Thread) { t.ret(0) })
}
