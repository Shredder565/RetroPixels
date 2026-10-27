package ps3

import (
	"fmt"
	"image"
	"image/png"
	"os"
	"strconv"
	"strings"
)

// PS3_DRAWDUMP=<frame>: during that frame, save the target surface after each
// draw to /tmp/claude-0/ps3/draws/NNNN.png and log each draw's state.
func init() {
	f, err := strconv.Atoi(os.Getenv("PS3_DRAWDUMP"))
	if err != nil {
		return
	}
	dir := "/tmp/claude-0/ps3/draws"
	fpDump := map[int]bool{}
	for _, x := range strings.Split(os.Getenv("PS3_FPDUMP"), ",") {
		if v, err := strconv.Atoi(x); err == nil {
			fpDump[v] = true
		}
	}
	os.MkdirAll(dir, 0o755)
	n := 0
	drawHook = func(m *Machine, r *rsxState, s *rsxSurface) {
		if int(m.frame) != f {
			return
		}
		texs := ""
		for i := uint32(0); i < 16; i++ {
			if r.reg(0x1A0C+0x20*i)>>31 != 0 { // enabled
				texs += fmt.Sprintf(" t%d:fmt%02x@%08X %dx%d", i, r.reg(0x1A04+0x20*i)>>8&0xff, m.textureRegs(r, int(i)).ea, r.reg(0x1A18+0x20*i)>>16, r.reg(0x1A18+0x20*i)&0xffff)
			}
		}
		m.Logf("MRT %d: target %x B %x/%x/%x C %x/%x/%x D %x/%x/%x blendMRT %x maskMRT %x 1d60 %x", n, r.reg(0x220), r.reg(0x18C), r.reg(0x218), r.reg(0x21C),
			r.reg(0x1B4), r.reg(0x280), r.reg(0x288), r.reg(0x1B8), r.reg(0x284), r.reg(0x28C), r.reg(0x36C), r.reg(0x370), r.reg(0x1D60))
		m.Logf("DRAW %d: surface %08X fmt %d %dx%d z %08X | blend %x src %08x dst %08x eq %08x | depth %x func %x mask %x | alpha %x | cmask %08x | fp %08X ctl %08x |%s",
			n, s.addr, s.fmt, s.w, s.h, s.zAddr, r.reg(0x310), r.reg(0x314), r.reg(0x318), r.reg(0x320),
			r.reg(0xA74), r.reg(0xA6C), r.reg(0xA70), r.reg(0x304), r.reg(0x324), r.reg(0x8E4), r.reg(0x1D60), texs)
		if fpDump[n] {
			fa := r.reg(0x8E4)
			p := m.decodeFP(m.dmaAddr(map[uint32]uint32{1: 0xFEED0000, 2: 0xFEED0001}[fa&3], fa&^3))
			m.Logf("FP of draw %d:\n%s", n, fpDisasm(p))
		}
		img := image.NewRGBA(image.Rect(0, 0, s.w, s.h))
		bpp := surfBpp(s.fmt)
		for y := 0; y < s.h; y++ {
			for x := 0; x < s.w; x++ {
				c := m.readPixel(s, s.addr+uint32(y)*s.pitch+uint32(x)*bpp)
				o := 4 * (y*s.w + x)
				img.Pix[o], img.Pix[o+1], img.Pix[o+2], img.Pix[o+3] = byte(u8c(c[0])), byte(u8c(c[1])), byte(u8c(c[2])), 255
			}
		}
		out, _ := os.Create(fmt.Sprintf("%s/%04d.png", dir, n))
		png.Encode(out, img)
		out.Close()
		n++
	}
}

var fpOpNames = map[uint8]string{0: "NOP", 1: "MOV", 2: "MUL", 3: "ADD", 4: "MAD", 5: "DP3", 6: "DP4", 7: "DST", 8: "MIN", 9: "MAX",
	0xA: "SLT", 0xB: "SGE", 0xC: "SLE", 0xD: "SGT", 0xE: "SNE", 0xF: "SEQ", 0x10: "FRC", 0x11: "FLR", 0x12: "KIL", 0x13: "PK4", 0x14: "UP4",
	0x15: "DDX", 0x16: "DDY", 0x17: "TEX", 0x18: "TXP", 0x19: "TXD", 0x1A: "RCP", 0x1B: "RSQ", 0x1C: "EX2", 0x1D: "LG2", 0x1E: "LIT",
	0x1F: "LRP", 0x20: "STR", 0x21: "SFL", 0x22: "COS", 0x23: "SIN", 0x24: "PK2", 0x25: "UP2", 0x26: "POW", 0x27: "PKB", 0x28: "UPB",
	0x29: "PK16", 0x2A: "UP16", 0x2B: "BEM", 0x2C: "PKG", 0x2D: "UPG", 0x2E: "DP2A", 0x2F: "TXL", 0x31: "TXB", 0x38: "DP2", 0x39: "NRM",
	0x3A: "DIV", 0x3B: "DIVSQ", 0x3C: "LIF", 0x40: "BRK", 0x41: "CAL", 0x42: "IFE", 0x43: "LOOP", 0x44: "REP", 0x45: "RET"}

func fpDisasm(p *fpProg) string {
	s := ""
	xyzw := "xyzw"
	src := func(in *fpInst, k int) string {
		x := in.src[k]
		r := ""
		switch x.typ {
		case 0:
			if x.half {
				r = fmt.Sprintf("H%d", x.tmp)
			} else {
				r = fmt.Sprintf("R%d", x.tmp)
			}
		case 1:
			r = fmt.Sprintf("IN%d", in.input)
		case 2:
			r = fmt.Sprintf("C%v", in.constant)
		}
		r += "." + string([]byte{xyzw[x.swz[0]], xyzw[x.swz[1]], xyzw[x.swz[2]], xyzw[x.swz[3]]})
		if x.abs {
			r = "|" + r + "|"
		}
		if x.neg {
			r = "-" + r
		}
		if x.prec != 0 {
			r += fmt.Sprintf("_p%d", x.prec)
		}
		return r
	}
	for i := range p.inst {
		in := &p.inst[i]
		name := fpOpNames[in.op]
		if in.flow {
			s += fmt.Sprintf("  %2d %s else %d end %d cond %d\n", i, name, in.elseAt, in.endAt, in.cond)
			continue
		}
		d := fmt.Sprintf("R%d", in.dst)
		if in.dstHalf {
			d = fmt.Sprintf("H%d", in.dst)
		}
		if in.noDest {
			d = "RC"
		}
		mask := ""
		for k := 0; k < 4; k++ {
			if in.mask[k] {
				mask += string(xyzw[k])
			}
		}
		extra := ""
		if in.sat {
			extra += "_SAT"
		}
		if in.prec != 0 {
			extra += fmt.Sprintf("_P%d", in.prec)
		}
		if in.scale != 1 {
			extra += fmt.Sprintf("x%g", in.scale)
		}
		if in.setCond {
			extra += "C"
		}
		if in.cond != 7 {
			extra += fmt.Sprintf(" (cc%d %d)", in.condReg, in.cond)
		}
		if in.op >= 0x17 && in.op <= 0x19 || in.op == 0x2F || in.op == 0x31 {
			extra += fmt.Sprintf(" tex%d", in.tex)
		}
		n := fpReads(in.op)
		args := ""
		for k := 0; k < n; k++ {
			args += " " + src(in, k)
		}
		s += fmt.Sprintf("  %2d %s%s %s.%s,%s\n", i, name, extra, d, mask, args)
	}
	return s
}
