package d3d

// A disassembler for the bytecode the assembler writes, for debugging the
// shader translation.

import (
	"encoding/binary"
	"fmt"
	"math"
	"strings"
)

var opNames = map[uint32]string{
	OpAdd: "add", OpAnd: "and", OpBreak: "break", OpBreakc: "breakc", OpContinue: "continue", OpContinuec: "continuec",
	OpDerivRtx: "deriv_rtx", OpDerivRty: "deriv_rty", OpDiscard: "discard", OpDiv: "div", OpDp2: "dp2", OpDp3: "dp3",
	OpDp4: "dp4", OpElse: "else", OpEndif: "endif", OpEndloop: "endloop", OpEq: "eq", OpExp: "exp", OpFrc: "frc",
	OpFtoi: "ftoi", OpFtou: "ftou", OpGe: "ge", OpIadd: "iadd", OpIf: "if", OpIeq: "ieq", OpIge: "ige", OpIlt: "ilt",
	OpImad: "imad", OpImax: "imax", OpImin: "imin", OpImul: "imul", OpIne: "ine", OpIneg: "ineg", OpIshl: "ishl",
	OpIshr: "ishr", OpItof: "itof", OpLd: "ld", OpLog: "log", OpLoop: "loop", OpLt: "lt", OpMad: "mad", OpMin: "min",
	OpMax: "max", OpMov: "mov", OpMovc: "movc", OpMul: "mul", OpNe: "ne", OpNot: "not", OpOr: "or", OpResinfo: "resinfo",
	OpRet: "ret", OpRoundNe: "round_ne", OpRoundNi: "round_ni", OpRoundPi: "round_pi", OpRoundZ: "round_z", OpRsq: "rsq",
	OpSample: "sample", OpSampleC: "sample_c", OpSampleCLz: "sample_c_lz", OpSampleL: "sample_l", OpSampleD: "sample_d",
	OpSampleB: "sample_b", OpSqrt: "sqrt", OpSincos: "sincos", OpUdiv: "udiv", OpUlt: "ult", OpUge: "uge", OpUmul: "umul",
	OpUmax: "umax", OpUmin: "umin", OpUshr: "ushr", OpUtof: "utof", OpXor: "xor",
	opDclResource: "dcl_resource", opDclConstBuffer: "dcl_constantbuffer", opDclSampler: "dcl_sampler",
	opDclInput: "dcl_input", opDclInputSgv: "dcl_input_sgv", opDclInputSiv: "dcl_input_siv", opDclInputPs: "dcl_input_ps",
	opDclInputPsSgv: "dcl_input_ps_sgv", opDclInputPsSiv: "dcl_input_ps_siv", opDclOutput: "dcl_output",
	opDclOutputSiv: "dcl_output_siv", opDclTemps: "dcl_temps", opDclIndexableTmp: "dcl_indexableTemp",
	OpLod: "lod", OpRcp: "rcp", OpF32tof16: "f32tof16", OpF16tof32: "f16tof32", OpCountbits: "countbits",
	OpFirstbitHi: "firstbit_hi", OpFirstbitLo: "firstbit_lo", OpUbfe: "ubfe", OpIbfe: "ibfe", OpBfi: "bfi",
	OpBfrev: "bfrev", OpRtxCoarse: "deriv_rtx_coarse", OpRtxFine: "deriv_rtx_fine", OpRtyCoarse: "deriv_rty_coarse",
	OpRtyFine: "deriv_rty_fine",
}

var typeNames = map[uint32]string{typeTemp: "r", typeInput: "v", typeOutput: "o", typeIndexable: "x",
	typeSampler: "s", typeResource: "t", typeCB: "cb", typeNull: "null", typeDepth: "oDepth"}

// Disasm lists the program in a DXBC container, one instruction a line.
func Disasm(blob []byte) string {
	if len(blob) < 32 || string(blob[:4]) != "DXBC" {
		return "not a DXBC container"
	}
	n := binary.LittleEndian.Uint32(blob[28:])
	var w []uint32
	for i := uint32(0); i < n; i++ {
		o := binary.LittleEndian.Uint32(blob[32+4*i:])
		tag := string(blob[o : o+4])
		size := binary.LittleEndian.Uint32(blob[o+4:])
		if tag == "SHEX" || tag == "SHDR" {
			for j := uint32(0); j < size/4; j++ {
				w = append(w, binary.LittleEndian.Uint32(blob[o+8+4*j:]))
			}
		}
	}
	if len(w) < 2 {
		return "no program"
	}
	var sb strings.Builder
	indent := 0
	for p := 2; p < len(w); {
		tok := w[p]
		op := tok & 0x7ff
		l := int(tok >> 24 & 0x7f)
		if l == 0 {
			break
		}
		end := p + l
		if end > len(w) {
			end = len(w)
		}
		name := opNames[op]
		if name == "" {
			name = fmt.Sprintf("op%d", op)
		}
		if op == OpElse || op == OpEndif || op == OpEndloop {
			indent--
		}
		if tok&Sat != 0 && op < 88 {
			name += "_sat"
		}
		if op == OpIf || op == OpBreakc || op == OpDiscard || op == OpContinuec {
			if tok&NonZero != 0 {
				name += "_nz"
			} else {
				name += "_z"
			}
		}
		q := p + 1
		for q < end && w[q-1]>>31 != 0 && q == p+1 { // extended opcode tokens
			q++
		}
		var args []string
		switch op {
		case opDclTemps:
			args = append(args, fmt.Sprint(w[q]))
			q = end
		case opDclIndexableTmp:
			args = append(args, fmt.Sprintf("x%d[%d], %d", w[q], w[q+1], w[q+2]))
			q = end
		}
		for q < end {
			s, nq := disOperand(w, q)
			if nq <= q {
				break
			}
			args = append(args, s)
			q = nq
			if (op == opDclInputSgv || op == opDclInputSiv || op == opDclOutputSiv || op == opDclInputPsSiv ||
				op == opDclInputPsSgv || op == opDclResource || op == opDclConstBuffer) && q < end {
				for ; q < end; q++ {
					args = append(args, fmt.Sprintf("%#x", w[q]))
				}
			}
		}
		fmt.Fprintf(&sb, "%s%s %s\n", strings.Repeat("  ", max(indent, 0)), name, strings.Join(args, ", "))
		if op == OpIf || op == OpElse || op == OpLoop {
			indent++
		}
		p += l
	}
	return sb.String()
}

func disOperand(w []uint32, q int) (string, int) {
	t := w[q]
	q++
	nc := t & 3
	mode := t >> 2 & 3
	typ := t >> 12 & 0xff
	dims := int(t >> 20 & 3)
	mod := ""
	if t>>31 != 0 {
		m := w[q] >> 6 & 0xff
		q++
		switch m {
		case 1:
			mod = "-"
		case 2:
			mod = "|"
		case 3:
			mod = "-|"
		}
	}
	if typ == typeImm32 {
		n := 1
		if nc == 2 {
			n = 4
		}
		var vs []string
		for i := 0; i < n; i++ {
			v := w[q+i]
			f := math.Float32frombits(v)
			if v != 0 && math.Abs(float64(f)) > 1e-6 && math.Abs(float64(f)) < 1e9 {
				vs = append(vs, fmt.Sprintf("%g", f))
			} else {
				vs = append(vs, fmt.Sprintf("%#x", v))
			}
		}
		return mod + "l(" + strings.Join(vs, ", ") + ")", q + n
	}
	name := typeNames[typ]
	if name == "" {
		name = fmt.Sprintf("type%d", typ)
	}
	s := mod + name
	for k := 0; k < dims; k++ {
		rep := t >> (22 + 3*k) & 7
		idx := ""
		switch rep {
		case 0:
			idx = fmt.Sprint(w[q])
			q++
		case 2, 3:
			imm := uint32(0)
			if rep == 3 {
				imm = w[q]
				q++
			}
			r, nq := disOperand(w, q)
			q = nq
			idx = r
			if imm != 0 {
				idx += fmt.Sprintf(" + %d", imm)
			}
		}
		if k == 0 && (typ == typeTemp || typ == typeInput || typ == typeOutput || typ == typeSampler || typ == typeResource) {
			s += idx
		} else if k == 0 && typ == typeIndexable || k == 0 && typ == typeCB {
			s += idx
		} else {
			s += "[" + idx + "]"
		}
	}
	if nc == 2 {
		c := t >> 4 & 0xff
		switch mode {
		case 0:
			if c != 15 {
				s += "."
				for i := 0; i < 4; i++ {
					if c>>i&1 != 0 {
						s += string("xyzw"[i])
					}
				}
			}
		case 1:
			sw := ""
			for i := 0; i < 4; i++ {
				sw += string("xyzw"[c>>(2*i)&3])
			}
			if sw != "xyzw" {
				s += "." + sw
			}
		case 2:
			s += "." + string("xyzw"[c&3])
		}
	}
	if mod == "|" || mod == "-|" {
		s += "|"
	}
	return s, q
}
