package ppc

import "fmt"

// Disasm returns a readable form of the instruction op at pc (for debugging).
func Disasm(pc, op uint32) string {
	d, a, b := rD(op), rA(op), rB(op)
	si := int32(int16(op))
	ui := op & 0xFFFF
	rc := ""
	if op&1 != 0 {
		rc = "."
	}
	switch op >> 26 {
	case 3:
		return fmt.Sprintf("twi %d,r%d,%d", d, a, si)
	case 7:
		return fmt.Sprintf("mulli r%d,r%d,%d", d, a, si)
	case 8:
		return fmt.Sprintf("subfic r%d,r%d,%d", d, a, si)
	case 10:
		return fmt.Sprintf("cmplwi cr%d,r%d,0x%X", op>>23&7, a, ui)
	case 11:
		return fmt.Sprintf("cmpwi cr%d,r%d,%d", op>>23&7, a, si)
	case 12:
		return fmt.Sprintf("addic r%d,r%d,%d", d, a, si)
	case 13:
		return fmt.Sprintf("addic. r%d,r%d,%d", d, a, si)
	case 14:
		if a == 0 {
			return fmt.Sprintf("li r%d,%d", d, si)
		}
		return fmt.Sprintf("addi r%d,r%d,%d", d, a, si)
	case 15:
		if a == 0 {
			return fmt.Sprintf("lis r%d,0x%X", d, ui)
		}
		return fmt.Sprintf("addis r%d,r%d,0x%X", d, a, ui)
	case 16:
		t := uint32(int32(int16(op & 0xFFFC)))
		if op&2 == 0 {
			t += pc
		}
		l := ""
		if op&1 != 0 {
			l = "l"
		}
		return fmt.Sprintf("bc%s %d,%d,%08X", l, d, a, t)
	case 17:
		return "sc"
	case 18:
		t := uint32(int32(op<<6)>>6) &^ 3
		if op&2 == 0 {
			t += pc
		}
		l := ""
		if op&1 != 0 {
			l = "l"
		}
		return fmt.Sprintf("b%s %08X", l, t)
	case 19:
		switch op >> 1 & 0x3FF {
		case 16:
			if d == 20 {
				if op&1 != 0 {
					return "blrl"
				}
				return "blr"
			}
			return fmt.Sprintf("bclr %d,%d", d, a)
		case 528:
			if op&1 != 0 {
				return "bctrl"
			}
			return fmt.Sprintf("bcctr %d,%d", d, a)
		case 50:
			return "rfi"
		case 150:
			return "isync"
		case 0:
			return "mcrf"
		}
		return fmt.Sprintf("cr-op %d (%d,%d,%d)", op>>1&0x3FF, d, a, b)
	case 20:
		return fmt.Sprintf("rlwimi%s r%d,r%d,%d,%d,%d", rc, a, d, b, op>>6&31, op>>1&31)
	case 21:
		return fmt.Sprintf("rlwinm%s r%d,r%d,%d,%d,%d", rc, a, d, b, op>>6&31, op>>1&31)
	case 23:
		return fmt.Sprintf("rlwnm%s r%d,r%d,r%d,%d,%d", rc, a, d, b, op>>6&31, op>>1&31)
	case 24:
		if op == 0x60000000 {
			return "nop"
		}
		return fmt.Sprintf("ori r%d,r%d,0x%X", a, d, ui)
	case 25:
		return fmt.Sprintf("oris r%d,r%d,0x%X", a, d, ui)
	case 26:
		return fmt.Sprintf("xori r%d,r%d,0x%X", a, d, ui)
	case 27:
		return fmt.Sprintf("xoris r%d,r%d,0x%X", a, d, ui)
	case 28:
		return fmt.Sprintf("andi. r%d,r%d,0x%X", a, d, ui)
	case 29:
		return fmt.Sprintf("andis. r%d,r%d,0x%X", a, d, ui)
	case 31:
		x := op >> 1 & 0x3FF
		names := map[uint32]string{0: "cmpw", 32: "cmplw", 266: "add", 10: "addc", 138: "adde", 40: "subf", 8: "subfc", 136: "subfe",
			104: "neg", 235: "mullw", 75: "mulhw", 11: "mulhwu", 491: "divw", 459: "divwu", 28: "and", 60: "andc", 444: "or",
			124: "nor", 316: "xor", 412: "orc", 476: "nand", 284: "eqv", 24: "slw", 536: "srw", 792: "sraw", 824: "srawi",
			26: "cntlzw", 922: "extsh", 954: "extsb", 23: "lwzx", 55: "lwzux", 87: "lbzx", 119: "lbzux", 279: "lhzx", 343: "lhax",
			151: "stwx", 183: "stwux", 215: "stbx", 247: "stbux", 407: "sthx", 439: "sthux", 534: "lwbrx", 662: "stwbrx",
			1014: "dcbz", 86: "dcbf", 54: "dcbst", 470: "dcbi", 982: "icbi", 598: "sync", 854: "eieio", 19: "mfcr", 144: "mtcrf",
			83: "mfmsr", 146: "mtmsr", 535: "lfsx", 599: "lfdx", 663: "stfsx", 727: "stfdx", 983: "stfiwx", 20: "lwarx", 150: "stwcx.",
			371: "mftb", 597: "lswi", 725: "stswi"}
		switch x {
		case 339:
			return fmt.Sprintf("mfspr r%d,%d", d, a|b<<5)
		case 467:
			return fmt.Sprintf("mtspr %d,r%d", a|b<<5, d)
		}
		n := names[x]
		if n == "" {
			n = names[x&0x1FF]
		}
		if n == "" {
			n = fmt.Sprintf("op31/%d", x)
		}
		return fmt.Sprintf("%s%s r%d,r%d,r%d", n, rc, d, a, b)
	case 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55:
		names := []string{"lwz", "lwzu", "lbz", "lbzu", "stw", "stwu", "stb", "stbu", "lhz", "lhzu", "lha", "lhau", "sth", "sthu", "lmw", "stmw",
			"lfs", "lfsu", "lfd", "lfdu", "stfs", "stfsu", "stfd", "stfdu"}
		r := "r"
		if op>>26 >= 48 {
			r = "f"
		}
		return fmt.Sprintf("%s %s%d,%d(r%d)", names[op>>26-32], r, d, si, a)
	case 59:
		return fmt.Sprintf("fp-s/%d f%d,f%d,f%d,f%d", op>>1&31, d, a, b, rC(op))
	case 63:
		return fmt.Sprintf("fp-d/%d f%d,f%d,f%d,f%d", op>>1&0x3FF, d, a, b, rC(op))
	}
	return fmt.Sprintf(".long 0x%08X", op)
}
