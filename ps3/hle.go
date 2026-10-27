package ps3

// System functions (the PS3's firmware libraries) are RetroPixels' own Go
// code. Each has an entry point in RetroPixels' area holding primary opcode
// 1 with its number; the import tables point at a function descriptor
// (address, TOC) for that entry.

import (
	"crypto/sha1"
	"encoding/binary"
	"math"
	"os"
)

type hleFunc func(m *Machine, t *Thread)

type hleEntry struct {
	name    string
	fn      hleFunc
	special bool // not a normal function: don't return to LR
}

// registry of system functions by name, filled by the hle_*.go files
var hleFuncs = map[string]hleFunc{}

func reg(name string, fn hleFunc) { hleFuncs[name] = fn }

// nidOf is a function's NID: the start of SHA-1(name + suffix), little-endian.
func nidOf(name string) uint32 {
	suffix := []byte{0x67, 0x59, 0x65, 0x99, 0x04, 0x25, 0x04, 0x90, 0x56, 0x64, 0x27, 0x49, 0x94, 0x89, 0x74, 0x1A}
	h := sha1.Sum(append([]byte(name), suffix...))
	return binary.LittleEndian.Uint32(h[:4])
}

var nidNames map[uint32]string

func (m *Machine) initHLE() {
	if nidNames == nil {
		nidNames = map[uint32]string{}
		for n := range hleFuncs {
			nidNames[nidOf(n)] = n
		}
		for _, n := range knownNames {
			nidNames[nidOf(n)] = n
		}
	}
	m.hle = append(m.hle, hleEntry{name: "(none)"})
}

// hleEntryFor returns the OPD of the system function (making it once).
func (m *Machine) hleEntryFor(module string, nid uint32) uint32 {
	name, ok := nidNames[nid]
	if !ok {
		name = module + "_" + hex32(nid)
	}
	if a, ok := m.hleAddr[name]; ok {
		return a
	}
	fn := hleFuncs[name]
	idx := uint32(len(m.hle))
	m.hle = append(m.hle, hleEntry{name: name, fn: fn})
	code := m.sysAlloc(8, 8)
	m.Mem.Write32(code, 0x04000000|idx)
	m.Mem.Write32(code+4, 0x4e800020) // blr (never reached)
	opd := m.sysAlloc(8, 8)
	m.Mem.Write32(opd, code)
	m.Mem.Write32(opd+4, 0)
	m.hleAddr[name] = opd
	return opd
}

// hleSpecial makes an entry point (code address) for RetroPixels' own use.
func (m *Machine) hleSpecial(name string, fn hleFunc) uint32 {
	idx := uint32(len(m.hle))
	m.hle = append(m.hle, hleEntry{name: name, fn: fn, special: true})
	code := m.sysAlloc(8, 8)
	m.Mem.Write32(code, 0x04000000|idx)
	return code
}

// hleOPD makes a function descriptor for a Go function the game can call
// (callbacks RetroPixels hands the game).
func (m *Machine) hleOPD(name string, fn hleFunc) uint32 {
	idx := uint32(len(m.hle))
	m.hle = append(m.hle, hleEntry{name: name, fn: fn})
	code := m.sysAlloc(8, 8)
	m.Mem.Write32(code, 0x04000000|idx)
	opd := m.sysAlloc(8, 8)
	m.Mem.Write32(opd, code)
	return opd
}

func (m *Machine) hleCall(c *PPU, n uint32) {
	t := m.cur
	if int(n) >= len(m.hle) {
		m.fail("bad system function call %d", n)
		return
	}
	e := m.hle[n]
	if !e.special {
		c.PC = uint32(c.LR)
	}
	if e.fn == nil {
		m.Missing[e.name]++
		if m.Missing[e.name] <= 3 {
			m.Logf("missing system function %s(%x, %x, %x, %x) from %08X", e.name, t.GPR[3], t.GPR[4], t.GPR[5], t.GPR[6], uint32(c.LR)-4)
		}
		t.GPR[3] = 0
		return
	}
	if traceHLE {
		m.Logf("%s(%x, %x, %x, %x) from %08X", e.name, t.GPR[3], t.GPR[4], t.GPR[5], t.GPR[6], uint32(t.LR)-4)
	}
	e.fn(m, t)
	if t.state != thReady {
		c.Stop()
	}
}

var traceHLE = os.Getenv("PS3_TRACE") != ""

func hex32(v uint32) string {
	const d = "0123456789ABCDEF"
	b := make([]byte, 8)
	for i := 7; i >= 0; i-- {
		b[i] = d[v&15]
		v >>= 4
	}
	return string(b)
}

// argument helpers
func (t *Thread) arg(i int) uint32   { return uint32(t.GPR[3+i]) }
func (t *Thread) arg64(i int) uint64 { return t.GPR[3+i] }
func (t *Thread) argS(i int) int32   { return int32(t.GPR[3+i]) }
func (t *Thread) ret(v uint32)       { t.GPR[3] = uint64(v) }
func (t *Thread) retS(v int32)       { t.GPR[3] = uint64(int64(v)) }
func (t *Thread) farg(i int) float64 { return math.Float64frombits(t.FPR[1+i]) }

// errors (CELL_* codes)
const (
	cellOK      = 0
	eAGAIN      = 0x80010001
	eINVAL      = 0x80010002
	eNOSYS      = 0x80010003
	eNOMEM      = 0x80010004
	eSRCH       = 0x80010005
	eNOENT      = 0x80010006
	eNOEXEC     = 0x80010007
	eDEADLK     = 0x80010008
	ePERM       = 0x80010009
	eBUSY       = 0x8001000A
	eTIMEDOUT   = 0x8001000B
	eABORT      = 0x8001000C
	eFAULT      = 0x8001000D
	eSTAT       = 0x8001000F
	eALIGN      = 0x80010010
	eEXIST      = 0x80010014
	eISDIR      = 0x80010012
	eNOTDIR     = 0x8001002E
	eBADF       = 0x8001002A
	eCANCELED   = 0x80010013
	eNOTMOUNTED = 0x8001003A
	eIO         = 0x8001002B
)
