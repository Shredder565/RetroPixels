package wiiu

// The system libraries.
//
// Wii U programs call into Cafe OS libraries (coreinit.rpl, vpad.rpl, gx2.rpl
// and so on). RetroPixels doesn't run Nintendo's libraries; it has its own
// version of each function a program imports, written in Go. Every function
// gets an entry point in the system area holding a library-call instruction
// (see cpu.go), so a program calls it like any other function.
//
// A function reads its arguments from r3..r10 / f1..f8 as the PowerPC ABI
// passes them and leaves its result in r3 (or f1). A function that has to
// wait (locking a held mutex, sleeping, waiting for the next frame) calls
// t.block: the thread stops, and runs the same call again when it's woken,
// with its arguments still in place.

import (
	"fmt"
	"math"
	"sort"
	"strconv"
	"strings"
)

type hleFunc func(m *Machine, c *CPU, t *Thread)

type hleEntry struct {
	lib, name string
	fn        hleFunc
	addr      uint32
}

const thunkBase = SystemBase + 0x1000 // library entry points, 8 bytes each

// library functions and data, by library then name (filled by the init
// functions of coreinit.go, vpad.go, ...)
var hleFuncs = map[string]map[string]hleFunc{}

// data exports: size, and a function to fill it in (or nil)
type hleDataDef struct {
	size uint32
	init func(m *Machine, addr uint32)
}

var hleData = map[string]map[string]hleDataDef{}

func reg(lib, name string, fn hleFunc) {
	if hleFuncs[lib] == nil {
		hleFuncs[lib] = map[string]hleFunc{}
	}
	hleFuncs[lib][name] = fn
}

func regData(lib, name string, size uint32, init func(m *Machine, addr uint32)) {
	if hleData[lib] == nil {
		hleData[lib] = map[string]hleDataDef{}
	}
	hleData[lib][name] = hleDataDef{size, init}
}

func libName(lib string) string {
	lib = strings.ToLower(lib)
	lib = strings.TrimSuffix(lib, ".rpl")
	return lib
}

// thunk returns the entry point of lib.name, making it the first time.
func (m *Machine) thunk(lib, name string) uint32 {
	key := lib + "." + name
	if i, ok := m.hleIndex[key]; ok {
		return m.hle[i].addr
	}
	fn := hleFuncs[lib][name]
	if fn == nil {
		fn = unimplemented(lib, name)
	}
	i := uint32(len(m.hle))
	addr := thunkBase + i*8
	m.hle = append(m.hle, hleEntry{lib, name, fn, addr})
	m.hleIndex[key] = i
	m.Mem.Write32(addr, 0x04000000|i)
	m.Mem.Write32(addr+4, 0x4e800020) // blr (for disassemblers)
	m.Mem.InvalidateCode(addr, 8)
	return addr
}

// HasFunc reports whether RetroPixels has its own version of lib.name.
func HasFunc(lib, name string) bool { return hleFuncs[libName(lib)][name] != nil }

func unimplemented(lib, name string) hleFunc {
	return func(m *Machine, c *CPU, t *Thread) {
		key := lib + "." + name
		if !m.warned[key] {
			m.warned[key] = true
			m.logf("not yet in RetroPixels: %s (called from %08X)", key, c.LR)
		}
		c.GPR[3] = 0
	}
}

// dataExport returns the address of a library data symbol.
func (m *Machine) dataExport(lib, name string) uint32 {
	key := lib + "." + name
	if a, ok := m.dataAddr[key]; ok {
		return a
	}
	def, ok := hleData[lib][name]
	if !ok {
		// a function used as data (a function pointer variable) or unknown
		if hleFuncs[lib][name] != nil {
			def = hleDataDef{4, func(m *Machine, a uint32) { m.Mem.Write32(a, m.thunk(lib, name)) }}
		} else {
			m.logf("not yet in RetroPixels: data %s", key)
			def = hleDataDef{0x100, nil}
		}
	}
	a := m.sysAlloc(def.size, 64)
	m.dataAddr[key] = a
	if def.init != nil {
		def.init(m, a)
	}
	return a
}

// Import implements Importer for the loader.
func (m *Machine) Import(lib, name string, data bool) (uint32, error) {
	lib = libName(lib)
	if mod := m.moduleNamed(lib); mod != nil {
		if a, ok := mod.Exports[name]; ok {
			return a, nil
		}
		return 0, fmt.Errorf("%s has no %s", lib, name)
	}
	// one of the title's own libraries
	if hleFuncs[lib] == nil && hleData[lib] == nil && !knownLibs[lib] {
		if m.loading[lib] {
			return 0, fmt.Errorf("%s imports itself in a loop", lib)
		}
		if m.loading == nil {
			m.loading = map[string]bool{}
		}
		m.loading[lib] = true
		mod, err := m.titleLibrary(lib)
		delete(m.loading, lib)
		if err != nil {
			return 0, err
		}
		if mod != nil {
			if a, ok := mod.Exports[name]; ok {
				return a, nil
			}
			return 0, fmt.Errorf("%s has no %s", lib, name)
		}
	}
	if data {
		return m.dataExport(lib, name), nil
	}
	return m.thunk(lib, name), nil
}

func (m *Machine) callHLE(c *CPU, n uint32) {
	if int(n) >= len(m.hle) {
		c.fail(n|0x04000000, "unknown library call")
		return
	}
	e := &m.hle[n]
	t := m.current[c.Core]
	if m.Trace != nil {
		m.Trace(fmt.Sprintf("[%d] %s.%s(%08X %08X %08X %08X) from %08X", c.Core, e.lib, e.name,
			c.GPR[3], c.GPR[4], c.GPR[5], c.GPR[6], c.LR))
	}
	if t != nil {
		t.blocked = false
	}
	e.fn(m, c, t)
	if t != nil && t.blocked {
		c.PC -= 4 // run this call again when the thread wakes
		c.Stop()
		return
	}
	if m.jumped {
		m.jumped = false
		return
	}
	c.PC = c.LR
}

// sysAlloc takes memory from the system area (for RetroPixels' own data).
func (m *Machine) sysAlloc(size, align uint32) uint32 {
	if align < 8 {
		align = 8
	}
	a := (m.sysNext + align - 1) &^ (align - 1)
	m.sysNext = a + size
	if m.sysNext > SystemBase+SystemSize {
		panic("wiiu: system area full")
	}
	m.Mem.Fill(a, 0, size)
	return a
}

// argument helpers

func arg(c *CPU, i int) uint32   { return c.GPR[3+i] }
func argF(c *CPU, i int) float64 { return c.FPR[1+i] }
func ret(c *CPU, v uint32)       { c.GPR[3] = v }
func retBool(c *CPU, v bool)     { c.GPR[3] = b2u(v) }
func ret64(c *CPU, v uint64)     { c.GPR[3], c.GPR[4] = uint32(v>>32), uint32(v) }
func retF(c *CPU, v float64)     { c.FPR[1] = v }
func arg64(c *CPU, i int) uint64 { return uint64(c.GPR[3+i])<<32 | uint64(c.GPR[4+i]) }
func b2u(v bool) uint32 {
	if v {
		return 1
	}
	return 0
}

// varargs: reads the "..." arguments of a call, as the PowerPC SysV ABI
// passes them (integers in r3..r10, doubles in f1..f8, the rest on the stack)
type varargs struct {
	m        *Machine
	c        *CPU
	gpr, fpr int
	stack    uint32
	// a va_list instead of registers
	list     bool
	regSave  uint32
	listAddr uint32
}

func newVarargs(m *Machine, c *CPU, firstGPR int) *varargs {
	return &varargs{m: m, c: c, gpr: firstGPR, fpr: 0, stack: c.GPR[1] + 8}
}

// a va_list: { u8 gpr; u8 fpr; u16 pad; void *overflow; void *regSave }
func newVaList(m *Machine, c *CPU, va uint32) *varargs {
	mem := m.Mem
	return &varargs{m: m, c: c, gpr: int(mem.Read8(va)), fpr: int(mem.Read8(va + 1)),
		stack: mem.Read32(va + 4), regSave: mem.Read32(va + 8), list: true, listAddr: va}
}

func (v *varargs) u32() uint32 {
	if v.gpr < 8 {
		v.gpr++
		if v.list {
			return v.m.Mem.Read32(v.regSave + uint32(v.gpr-1)*4)
		}
		return v.c.GPR[3+v.gpr-1]
	}
	x := v.m.Mem.Read32(v.stack)
	v.stack += 4
	return x
}

func (v *varargs) u64() uint64 {
	if v.gpr&1 != 0 { // pairs start at r3, r5, r7, r9
		v.gpr++
	}
	if v.gpr < 7 {
		hi := v.u32()
		lo := v.u32()
		return uint64(hi)<<32 | uint64(lo)
	}
	v.gpr = 8
	v.stack = (v.stack + 7) &^ 7
	x := v.m.Mem.Read64(v.stack)
	v.stack += 8
	return x
}

func (v *varargs) f64() float64 {
	if v.fpr < 8 {
		v.fpr++
		if v.list {
			return math.Float64frombits(v.m.Mem.Read64(v.regSave + 32 + uint32(v.fpr-1)*8))
		}
		return v.c.FPR[v.fpr]
	}
	v.stack = (v.stack + 7) &^ 7
	x := math.Float64frombits(v.m.Mem.Read64(v.stack))
	v.stack += 8
	return x
}

// sprintf formats a C printf format string with the call's arguments.
func (m *Machine) sprintf(format string, va *varargs) string {
	var b strings.Builder
	for i := 0; i < len(format); i++ {
		ch := format[i]
		if ch != '%' {
			b.WriteByte(ch)
			continue
		}
		i++
		if i >= len(format) {
			break
		}
		// flags, width, precision, length
		start := i
		for i < len(format) && strings.IndexByte("-+ #0", format[i]) >= 0 {
			i++
		}
		flags := format[start:i]
		width := ""
		if i < len(format) && format[i] == '*' {
			width = strconv.Itoa(int(int32(va.u32())))
			i++
		} else {
			for i < len(format) && format[i] >= '0' && format[i] <= '9' {
				width += string(format[i])
				i++
			}
		}
		prec := ""
		hasPrec := false
		if i < len(format) && format[i] == '.' {
			hasPrec = true
			i++
			if i < len(format) && format[i] == '*' {
				prec = strconv.Itoa(int(int32(va.u32())))
				i++
			} else {
				for i < len(format) && format[i] >= '0' && format[i] <= '9' {
					prec += string(format[i])
					i++
				}
			}
			if prec == "" {
				prec = "0"
			}
		}
		long := 0
		for i < len(format) && strings.IndexByte("hlLqjzt", format[i]) >= 0 {
			if format[i] == 'l' || format[i] == 'q' || format[i] == 'L' || format[i] == 'j' {
				long++
			}
			i++
		}
		if i >= len(format) {
			break
		}
		conv := format[i]
		spec := "%" + flags + width
		if hasPrec {
			spec += "." + prec
		}
		switch conv {
		case 'd', 'i':
			if long >= 2 {
				fmt.Fprintf(&b, spec+"d", int64(va.u64()))
			} else {
				fmt.Fprintf(&b, spec+"d", int32(va.u32()))
			}
		case 'u':
			if long >= 2 {
				fmt.Fprintf(&b, spec+"d", va.u64())
			} else {
				fmt.Fprintf(&b, spec+"d", va.u32())
			}
		case 'x', 'X', 'o':
			if long >= 2 {
				fmt.Fprintf(&b, spec+string(conv), va.u64())
			} else {
				fmt.Fprintf(&b, spec+string(conv), va.u32())
			}
		case 'p':
			fmt.Fprintf(&b, "0x%08x", va.u32())
		case 'c':
			fmt.Fprintf(&b, "%"+flags+width+"c", rune(byte(va.u32())))
		case 's':
			s := m.Mem.String(va.u32(), 4096)
			if !hasPrec && width == "" {
				b.WriteString(s)
			} else {
				fmt.Fprintf(&b, spec+"s", s)
			}
		case 'f', 'F', 'e', 'E', 'g', 'G', 'a', 'A':
			if conv == 'F' {
				conv = 'f'
			}
			if conv == 'a' || conv == 'A' {
				conv = 'x'
			}
			if !hasPrec && (conv == 'f' || conv == 'e' || conv == 'E') {
				spec += ".6"
			}
			fmt.Fprintf(&b, spec+string(conv), va.f64())
		case 'n':
			va.u32()
		case '%':
			b.WriteByte('%')
		default:
			b.WriteByte('%')
			b.WriteByte(conv)
		}
	}
	return b.String()
}

// functions near addr, for crash reports
func (m *Machine) symbolize(addr uint32) string {
	if addr >= thunkBase && addr < thunkBase+uint32(len(m.hle))*8 {
		e := m.hle[(addr-thunkBase)/8]
		return e.lib + "." + e.name
	}
	for _, mod := range m.Modules {
		syms := mod.Symbols
		i := sort.Search(len(syms), func(i int) bool { return syms[i].Addr > addr })
		if i > 0 {
			s := syms[i-1]
			if s.Size == 0 || addr < s.Addr+s.Size+0x1000 {
				return fmt.Sprintf("%s+0x%x", s.Name, addr-s.Addr)
			}
		}
	}
	return ""
}
