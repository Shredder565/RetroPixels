package ppc

// Idle loops: games spend most of their time in short loops that read a
// variable in RAM until an interrupt handler changes it. Such a loop does
// the same thing every time round until the next interrupt, so the
// processor skips ahead to it (the decrementer or the end of this Run, after
// which the board raises its interrupts). Nothing the program can see
// changes: the skipped cycles still count as run.

// idleLoop says whether the loop from start to the branch at end only reads
// RAM and compares, so that taking the branch once means taking it until an
// interrupt.
func (c *CPU) idleLoop(start, end uint32) bool {
	if c.NoIdleSkip || c.notIdle[end] {
		return false
	}
	ok := c.analyzeIdle(start, end)
	if !ok {
		if c.notIdle == nil || len(c.notIdle) > 4096 {
			c.notIdle = map[uint32]bool{}
		}
		c.notIdle[end] = true
	}
	return ok
}

func (c *CPU) analyzeIdle(start, end uint32) bool {
	// registers the loop writes
	var written uint32
	type insn struct{ srcs, dst uint32 }
	var body []insn
	for pc := start; pc < end; pc += 4 {
		op := c.read32(pc)
		var srcs, dst uint32
		switch op >> 26 {
		case 32, 34, 40, 42: // lwz, lbz, lhz, lha
			a := rA(op)
			if a == 0 {
				return false
			}
			ea := c.GPR[a] + uint32(int32(int16(op)))
			if ea >= uint32(len(c.RAM))-4 {
				return false // a device register may change by itself
			}
			srcs, dst = 1<<a, 1<<rD(op)
		case 10, 11: // cmplwi, cmpwi
			srcs = 1 << rA(op)
		case 21, 24, 28: // rlwinm, ori, andi.
			srcs, dst = 1<<rD(op), 1<<rA(op)
		case 31:
			switch op >> 1 & 0x3FF {
			case 0, 32: // cmpw, cmplw
				srcs = 1<<rA(op) | 1<<rB(op)
			default:
				return false
			}
		default:
			return false
		}
		written |= dst
		body = append(body, insn{srcs, dst})
	}
	// every register the loop reads must either stay the same or be set
	// earlier in the same time round
	var set uint32
	for _, in := range body {
		if in.srcs&written&^set != 0 {
			return false
		}
		set |= in.dst
	}
	// a load's base register must not change
	for pc := start; pc < end; pc += 4 {
		op := c.read32(pc)
		switch op >> 26 {
		case 32, 34, 40, 42:
			if written&(1<<rA(op)) != 0 {
				return false
			}
		}
	}
	return true
}

// skipIdle skips whole times round the loop of n instructions, up to the
// next decrementer interrupt or the end of this Run, so the program ends up
// exactly where it would have been. It is called from the loop's branch,
// before the branch itself is counted.
func (c *CPU) skipIdle(n int) {
	limit := 0
	if c.decTrig >= 0 && c.decTrig < c.icount {
		limit = c.decTrig
	}
	if k := (c.icount - 2 - limit) / n; k > 0 {
		c.icount -= k * n
	}
}
