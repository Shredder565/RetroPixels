package sh2

import "fmt"

// Disasm returns the assembly text of op at address pc. If read is not nil
// it is used to show the values PC-relative loads fetch.
func Disasm(pc uint32, op uint16, read func(a uint32) uint32) string {
	n, m := rn(op), rm(op)
	i8 := int32(int8(op))
	d4 := uint32(op & 15)
	d8 := uint32(op & 0xff)
	br8 := pc + 4 + disp8(op)
	br12 := pc + 4 + disp12(op)
	switch op >> 12 {
	case 0:
		switch op & 0xff {
		case 0x08:
			return "clrt"
		case 0x18:
			return "sett"
		case 0x28:
			return "clrmac"
		case 0x09:
			return "nop"
		case 0x19:
			return "div0u"
		case 0x0b:
			return "rts"
		case 0x1b:
			return "sleep"
		case 0x2b:
			return "rte"
		}
		switch op & 0xff {
		case 0x02:
			return fmt.Sprintf("stc sr,r%d", n)
		case 0x12:
			return fmt.Sprintf("stc gbr,r%d", n)
		case 0x22:
			return fmt.Sprintf("stc vbr,r%d", n)
		case 0x03:
			return fmt.Sprintf("bsrf r%d", n)
		case 0x23:
			return fmt.Sprintf("braf r%d", n)
		case 0x29:
			return fmt.Sprintf("movt r%d", n)
		case 0x0a:
			return fmt.Sprintf("sts mach,r%d", n)
		case 0x1a:
			return fmt.Sprintf("sts macl,r%d", n)
		case 0x2a:
			return fmt.Sprintf("sts pr,r%d", n)
		}
		switch op & 15 {
		case 4:
			return fmt.Sprintf("mov.b r%d,@(r0,r%d)", m, n)
		case 5:
			return fmt.Sprintf("mov.w r%d,@(r0,r%d)", m, n)
		case 6:
			return fmt.Sprintf("mov.l r%d,@(r0,r%d)", m, n)
		case 7:
			return fmt.Sprintf("mul.l r%d,r%d", m, n)
		case 0xc:
			return fmt.Sprintf("mov.b @(r0,r%d),r%d", m, n)
		case 0xd:
			return fmt.Sprintf("mov.w @(r0,r%d),r%d", m, n)
		case 0xe:
			return fmt.Sprintf("mov.l @(r0,r%d),r%d", m, n)
		case 0xf:
			return fmt.Sprintf("mac.l @r%d+,@r%d+", m, n)
		}
	case 1:
		return fmt.Sprintf("mov.l r%d,@(%d,r%d)", m, d4*4, n)
	case 2:
		f := [16]string{"mov.b r%d,@r%d", "mov.w r%d,@r%d", "mov.l r%d,@r%d", "", "mov.b r%d,@-r%d", "mov.w r%d,@-r%d",
			"mov.l r%d,@-r%d", "div0s r%d,r%d", "tst r%d,r%d", "and r%d,r%d", "xor r%d,r%d", "or r%d,r%d",
			"cmp/str r%d,r%d", "xtrct r%d,r%d", "mulu.w r%d,r%d", "muls.w r%d,r%d"}[op&15]
		if f != "" {
			return fmt.Sprintf(f, m, n)
		}
	case 3:
		f := [16]string{"cmp/eq", "", "cmp/hs", "cmp/ge", "div1", "dmulu.l", "cmp/hi", "cmp/gt", "sub", "", "subc",
			"subv", "add", "dmuls.l", "addc", "addv"}[op&15]
		if f != "" {
			return fmt.Sprintf("%s r%d,r%d", f, m, n)
		}
	case 4:
		if op&15 == 0xf {
			return fmt.Sprintf("mac.w @r%d+,@r%d+", m, n)
		}
		f := map[uint16]string{0x00: "shll r%d", 0x01: "shlr r%d", 0x02: "sts.l mach,@-r%d", 0x03: "stc.l sr,@-r%d",
			0x04: "rotl r%d", 0x05: "rotr r%d", 0x06: "lds.l @r%d+,mach", 0x07: "ldc.l @r%d+,sr", 0x08: "shll2 r%d",
			0x09: "shlr2 r%d", 0x0a: "lds r%d,mach", 0x0b: "jsr @r%d", 0x0e: "ldc r%d,sr", 0x10: "dt r%d",
			0x11: "cmp/pz r%d", 0x12: "sts.l macl,@-r%d", 0x13: "stc.l gbr,@-r%d", 0x15: "cmp/pl r%d",
			0x16: "lds.l @r%d+,macl", 0x17: "ldc.l @r%d+,gbr", 0x18: "shll8 r%d", 0x19: "shlr8 r%d",
			0x1a: "lds r%d,macl", 0x1b: "tas.b @r%d", 0x1e: "ldc r%d,gbr", 0x20: "shal r%d", 0x21: "shar r%d",
			0x22: "sts.l pr,@-r%d", 0x23: "stc.l vbr,@-r%d", 0x24: "rotcl r%d", 0x25: "rotcr r%d",
			0x26: "lds.l @r%d+,pr", 0x27: "ldc.l @r%d+,vbr", 0x28: "shll16 r%d", 0x29: "shlr16 r%d",
			0x2a: "lds r%d,pr", 0x2b: "jmp @r%d", 0x2e: "ldc r%d,vbr"}[op&0xff]
		if f != "" {
			return fmt.Sprintf(f, n)
		}
	case 5:
		return fmt.Sprintf("mov.l @(%d,r%d),r%d", d4*4, m, n)
	case 6:
		f := [16]string{"mov.b @r%d,r%d", "mov.w @r%d,r%d", "mov.l @r%d,r%d", "mov r%d,r%d", "mov.b @r%d+,r%d",
			"mov.w @r%d+,r%d", "mov.l @r%d+,r%d", "not r%d,r%d", "swap.b r%d,r%d", "swap.w r%d,r%d", "negc r%d,r%d",
			"neg r%d,r%d", "extu.b r%d,r%d", "extu.w r%d,r%d", "exts.b r%d,r%d", "exts.w r%d,r%d"}[op&15]
		return fmt.Sprintf(f, m, n)
	case 7:
		return fmt.Sprintf("add #%d,r%d", i8, n)
	case 8:
		switch op >> 8 & 15 {
		case 0:
			return fmt.Sprintf("mov.b r0,@(%d,r%d)", d4, m)
		case 1:
			return fmt.Sprintf("mov.w r0,@(%d,r%d)", d4*2, m)
		case 4:
			return fmt.Sprintf("mov.b @(%d,r%d),r0", d4, m)
		case 5:
			return fmt.Sprintf("mov.w @(%d,r%d),r0", d4*2, m)
		case 8:
			return fmt.Sprintf("cmp/eq #%d,r0", i8)
		case 9:
			return fmt.Sprintf("bt $%08x", br8)
		case 0xb:
			return fmt.Sprintf("bf $%08x", br8)
		case 0xd:
			return fmt.Sprintf("bt/s $%08x", br8)
		case 0xf:
			return fmt.Sprintf("bf/s $%08x", br8)
		}
	case 9:
		a := pc + 4 + d8*2
		if read != nil {
			v := read(a&^3) >> (16 - 8*(a&2)) & 0xffff
			return fmt.Sprintf("mov.w @($%08x),r%d ; $%04x", a, n, v)
		}
		return fmt.Sprintf("mov.w @($%08x),r%d", a, n)
	case 0xa:
		return fmt.Sprintf("bra $%08x", br12)
	case 0xb:
		return fmt.Sprintf("bsr $%08x", br12)
	case 0xc:
		f := [16]string{"mov.b r0,@(%d,gbr)", "mov.w r0,@(%d,gbr)", "mov.l r0,@(%d,gbr)", "trapa #%d",
			"mov.b @(%d,gbr),r0", "mov.w @(%d,gbr),r0", "mov.l @(%d,gbr),r0", "", "tst #%d,r0", "and #%d,r0",
			"xor #%d,r0", "or #%d,r0", "tst.b #%d,@(r0,gbr)", "and.b #%d,@(r0,gbr)", "xor.b #%d,@(r0,gbr)",
			"or.b #%d,@(r0,gbr)"}[op>>8&15]
		switch op >> 8 & 15 {
		case 1:
			return fmt.Sprintf(f, d8*2)
		case 2, 6:
			return fmt.Sprintf(f, d8*4)
		case 7:
			return fmt.Sprintf("mova @($%08x),r0", (pc+4)&^3+d8*4)
		}
		return fmt.Sprintf(f, d8)
	case 0xd:
		a := (pc+4)&^3 + d8*4
		if read != nil {
			return fmt.Sprintf("mov.l @($%08x),r%d ; $%08x", a, n, read(a))
		}
		return fmt.Sprintf("mov.l @($%08x),r%d", a, n)
	case 0xe:
		return fmt.Sprintf("mov #%d,r%d", i8, n)
	}
	return fmt.Sprintf(".word $%04x", op)
}
