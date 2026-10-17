package psikyo

// eeprom is a 93C56 serial EEPROM in its 8-bit organisation (256 bytes):
// commands are a start bit, two opcode bits and nine address bits (the top
// one unused), clocked in on CLK's rising edge while CS is high. After a
// write or erase the chip is busy (DO low) for a while once CS goes low.
type eeprom struct {
	mem [256]uint8

	cs, clk, di bool
	do          bool

	state     int    // what the clocked-in bits are for
	bits      int    // bits gathered in this state
	sr        uint32 // gathered bits
	op        int    // opcode after the start bit (0-3)
	addr      int
	out       uint8 // byte being shifted out
	outBits   int
	writeOK   bool  // EWEN given
	program   bool  // a write or erase starts when CS goes low
	busyUntil int64 // the board's clock when programming ends
}

const (
	eeIdle = iota // waiting for the start bit
	eeCmd         // opcode + address
	eeRead        // shifting data out
	eeData        // shifting write data in
	eeDone        // command finished; wait for CS low
)

const (
	eeAddrBits  = 9
	eeWriteTime = cpuHz / 625 // CPU cycles a write or erase takes (1.6 ms)
)

func (e *eeprom) reset() {
	e.cs, e.clk, e.di, e.do = false, false, false, true
	e.state, e.bits, e.sr = eeIdle, 0, 0
	e.writeOK, e.program = false, false
	e.busyUntil = 0
}

// dataOut is the DO line at time now.
func (e *eeprom) dataOut(now int64) bool {
	if now < e.busyUntil {
		return false
	}
	return e.do
}

// write sets the three input lines at time now.
func (e *eeprom) write(cs, clk, di bool, now int64) {
	if !cs {
		if e.cs {
			// CS falling ends a command and starts programming
			e.state, e.bits = eeIdle, 0
			e.do = true
			if e.program {
				e.program = false
				e.busyUntil = now + eeWriteTime
			}
		}
		e.cs, e.clk, e.di = cs, clk, di
		return
	}
	rising := clk && !e.clk
	e.cs, e.clk, e.di = cs, clk, di
	if !rising || now < e.busyUntil {
		return
	}
	bit := uint32(0)
	if di {
		bit = 1
	}
	switch e.state {
	case eeIdle:
		if bit == 1 {
			e.state, e.bits, e.sr = eeCmd, 0, 0
		}
	case eeCmd:
		e.sr = e.sr<<1 | bit
		e.bits++
		if e.bits == 2+eeAddrBits {
			e.op = int(e.sr >> eeAddrBits)
			e.addr = int(e.sr) & 0xff
			e.command()
		}
	case eeRead:
		if e.outBits == 0 {
			// sequential read: the next byte follows
			e.addr = (e.addr + 1) & 0xff
			e.out, e.outBits = e.mem[e.addr], 8
		}
		e.do = e.out&0x80 != 0
		e.out <<= 1
		e.outBits--
	case eeData:
		e.sr = e.sr<<1 | bit
		e.bits++
		if e.bits == 8 {
			v := uint8(e.sr)
			if e.writeOK {
				if e.op == 1 { // WRITE
					e.mem[e.addr] = v
				} else { // WRAL
					for i := range e.mem {
						e.mem[i] = v
					}
				}
				e.program = true
			}
			e.state = eeDone
		}
	}
}

// command runs once the opcode and address are in.
func (e *eeprom) command() {
	switch e.op {
	case 2: // READ: a dummy 0, then the data from the next clock
		e.state = eeRead
		e.do = false
		e.out, e.outBits = e.mem[e.addr], 8
		return
	case 1: // WRITE
		e.state, e.bits, e.sr = eeData, 0, 0
		return
	case 3: // ERASE
		if e.writeOK {
			e.mem[e.addr] = 0xff
			e.program = true
		}
	case 0:
		switch e.sr >> (eeAddrBits - 2) & 3 {
		case 3: // EWEN
			e.writeOK = true
		case 0: // EWDS
			e.writeOK = false
		case 2: // ERAL
			if e.writeOK {
				for i := range e.mem {
					e.mem[i] = 0xff
				}
				e.program = true
			}
		case 1: // WRAL: data follows
			e.state, e.bits, e.sr = eeData, 0, 0
			return
		}
	}
	e.state = eeDone
}
