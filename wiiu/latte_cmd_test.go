package wiiu

import (
	"strings"
	"testing"
)

const tCmd = 0x10500000

func putWords(g *Latte, addr uint32, w []uint32) {
	for i, v := range w {
		g.mem.Write32(addr+uint32(i)*4, v)
	}
}

func cmdGPU(t *testing.T) (*Latte, *[]string) {
	g := newTestGPU()
	var log []string
	g.m.Log = func(s string) { log = append(log, s) }
	g.m.warned = map[string]bool{}
	return g, &log
}

func noWarnings(t *testing.T, log *[]string) {
	t.Helper()
	for _, s := range *log {
		if strings.HasPrefix(s, "gpu:") {
			t.Errorf("warning: %s", s)
		}
	}
}

// A command buffer whose size ends inside its last packet: the GPU reads the
// packet to its end anyway.
func TestPM4PacketPastEnd(t *testing.T) {
	g, log := cmdGPU(t)
	b := &pm4{}
	b.set(regCB_TARGET_MASK, 0x12)
	b.set(regPA_CL_VPORT_XSCALE_0, 1, 2, 3, 4)
	putWords(g, tCmd, b.w)
	g.execMem(tCmd, uint32(len(b.w)-2))
	if g.reg(regCB_TARGET_MASK) != 0x12 || g.reg(regPA_CL_VPORT_XSCALE_0+12) != 4 {
		t.Errorf("registers: %x %x", g.reg(regCB_TARGET_MASK), g.reg(regPA_CL_VPORT_XSCALE_0+12))
	}
	for _, s := range *log {
		if strings.Contains(s, "truncated") {
			t.Error(s)
		}
	}
}

// An indirect buffer given by its physical address, as a program writing
// its own packets does.
func TestPM4PhysicalIndirectBuffer(t *testing.T) {
	g, log := cmdGPU(t)
	b := &pm4{}
	b.set(regCB_TARGET_MASK, 0x34)
	putWords(g, tCmd, b.w)
	top := &pm4{}
	top.packet(itINDIRECT_BUFFER_PR, toPhysical(tCmd), 0, uint32(len(b.w)))
	g.exec(top.w)
	if g.reg(regCB_TARGET_MASK) != 0x34 {
		t.Errorf("CB_TARGET_MASK %x", g.reg(regCB_TARGET_MASK))
	}
	noWarnings(t, log)
}

// LOAD_* loads registers from memory; with shadowing on, SET_* writes go
// back there.
func TestPM4LoadAndShadow(t *testing.T) {
	g, log := cmdGPU(t)
	const area = 0x10600000
	at := func(r uint32) uint32 { return area + r - baseContext }
	g.mem.Write32(at(regCB_TARGET_MASK), 0xa)
	g.mem.Write32(at(regCB_SHADER_MASK), 0xb)
	g.mem.Write32(at(regDB_DEPTH_CONTROL), 0xc)
	b := &pm4{}
	b.packet(itCONTEXT_CONTROL, ctxShadowOn, ctxShadowOn)
	b.packet(itLOAD_CONTEXT_REG, area, 0, (regCB_TARGET_MASK-baseContext)/4, 2, (regDB_DEPTH_CONTROL-baseContext)/4, 1)
	g.exec(b.w)
	if g.reg(regCB_TARGET_MASK) != 0xa || g.reg(regCB_SHADER_MASK) != 0xb || g.reg(regDB_DEPTH_CONTROL) != 0xc {
		t.Fatalf("loaded %x %x %x", g.reg(regCB_TARGET_MASK), g.reg(regCB_SHADER_MASK), g.reg(regDB_DEPTH_CONTROL))
	}
	b = &pm4{}
	b.set(regCB_SHADER_MASK, 0x77)
	b.set(regDB_DEPTH_CONTROL, 0x88)
	b.set(regCB_COLOR_CONTROL, 0x99) // not loaded, so not shadowed
	g.exec(b.w)
	if v := g.mem.Read32(at(regCB_SHADER_MASK)); v != 0x77 {
		t.Errorf("shadow of CB_SHADER_MASK %x", v)
	}
	if v := g.mem.Read32(at(regDB_DEPTH_CONTROL)); v != 0x88 {
		t.Errorf("shadow of DB_DEPTH_CONTROL %x", v)
	}
	if v := g.mem.Read32(at(regCB_COLOR_CONTROL)); v != 0 {
		t.Errorf("CB_COLOR_CONTROL shadowed: %x", v)
	}
	b = &pm4{}
	b.packet(itCONTEXT_CONTROL, 0x80000000, 0x80000000)
	b.set(regCB_TARGET_MASK, 0x99)
	g.exec(b.w)
	if g.reg(regCB_TARGET_MASK) != 0x99 || g.mem.Read32(at(regCB_TARGET_MASK)) != 0xa {
		t.Errorf("shadowing off: reg %x memory %x", g.reg(regCB_TARGET_MASK), g.mem.Read32(at(regCB_TARGET_MASK)))
	}
	noWarnings(t, log)
}

func TestPM4MemoryPackets(t *testing.T) {
	g, log := cmdGPU(t)
	const a = 0x10700000
	putWords(g, a, []uint32{0, 0x55555555, 0, 0, 0, 0, 0, 0})
	b := &pm4{}
	b.packet(itMEM_WRITE, toPhysical(a)|2, 0x40000, 0x11223344, 0x99999999)
	b.packet(itMEM_WRITE, a+8, 0, 1, 2)
	b.packet(itMEM_WRITE, a+16, 0x20000, 0x11223344, 0x55667788)
	b.packet(itMEM_SEMAPHORE, a+24, 6<<29)
	b.packet(itMEM_SEMAPHORE, a+24, 6<<29)
	b.packet(itMEM_SEMAPHORE, a+24, 7<<29)
	b.packet(itEVENT_WRITE_EOP, 0x504, a+32, 2<<29, 0x0d0c0b0a, 0x01020304)
	b.packet(itWAIT_REG_MEM, 3|0x10, a, 0, 0x11223344, 0xffffffff, 10)
	b.packet(itSTRMOUT_BUF_UPDATE, 3<<1|1<<8, a+40, 0, 0, 0)
	g.setReg(regVGT_STRMOUT_BUFFER_OFFSET0+16, 0x123)
	g.exec(b.w)
	want := []uint32{0x11223344, 0x55555555, 1, 2, 0x44332211, 0x88776655, 0x01000000, 0, 0x01020304, 0x0d0c0b0a, 0x123}
	for i, w := range want {
		if v := g.mem.Read32(a + uint32(i)*4); v != w {
			t.Errorf("word %d: %08x, want %08x", i, v, w)
		}
	}
	noWarnings(t, log)
}

func TestPM4Draws(t *testing.T) {
	g, log := cmdGPU(t)
	b := &pm4{}
	b.packet(itINDEX_TYPE, 0)
	b.packet(itNUM_INSTANCES, 0)
	b.packet(itDRAW_INDEX_2, 3, toPhysical(tCmd), 0, 3, 0)
	b.packet(itDRAW_INDEX_AUTO, 3, 2)
	b.packet(itDRAW_INDEX_IMMD, 3, 1, 0x00010000, 2)
	b.w = append(b.w, pm4Filler)
	b.packet(itSET_PREDICATION, 0, 0)
	b.packet(itSURFACE_SYNC, coherTCAction, 1, tTex>>8, 10)
	g.exec(b.w)
	if g.Draws != 3 || g.numInstances != 1 {
		t.Errorf("draws %d, instances %d", g.Draws, g.numInstances)
	}
	noWarnings(t, log)
}

func TestPM4VertexSemanticClear(t *testing.T) {
	g, _ := cmdGPU(t)
	g.setReg(regSQ_VTX_SEMANTIC_0+4, 5)
	b := &pm4{}
	b.set(regSQ_VTX_SEMANTIC_CLEAR, 0xffffffff)
	g.exec(b.w)
	if v := g.reg(regSQ_VTX_SEMANTIC_0 + 4); v != 0xff {
		t.Errorf("semantic 1 %x", v)
	}
}

// Each core records its own display list; the main core's packets still go
// to the GPU meanwhile.
func TestDisplayListPerCore(t *testing.T) {
	g, log := cmdGPU(t)
	m := g.m
	m.gpu = g
	m.gx2.mainCore = 1
	on := func(core int) { m.running = &Thread{Core: core} }
	const dlA, dlB = 0x10800000, 0x10810000
	set := func(addr, v uint32) {
		b := &pm4{}
		b.set(addr, v)
		m.emit(b)
	}
	on(0)
	m.beginDisplayList(dlA, 0x100)
	on(2)
	m.beginDisplayList(dlB, 0x100)
	on(0)
	set(regCB_TARGET_MASK, 1)
	on(2)
	set(regCB_SHADER_MASK, 2)
	on(1)
	set(regDB_DEPTH_CONTROL, 3)
	if g.reg(regDB_DEPTH_CONTROL) != 3 || g.reg(regCB_TARGET_MASK) != 0 {
		t.Fatal("main core's packets didn't run, or a display list's did")
	}
	on(0)
	nA := m.endDisplayList()
	on(2)
	nB := m.endDisplayList()
	if nA != 32 || nB != 32 {
		t.Fatalf("sizes %d %d", nA, nB)
	}
	g.execMem(dlA, nA/4)
	if g.reg(regCB_TARGET_MASK) != 1 || g.reg(regCB_SHADER_MASK) != 0 {
		t.Error("list A has the wrong packets")
	}
	g.execMem(dlB, nB/4)
	if g.reg(regCB_SHADER_MASK) != 2 {
		t.Error("list B has the wrong packets")
	}
	// a packet that doesn't fit is left out whole
	on(1)
	m.beginDisplayList(dlA, 16)
	set(regCB_TARGET_MASK, 4)
	m.emit(&pm4{w: []uint32{pm4Type3(itSET_CONTEXT_REG, 3), 0, 1, 2}})
	if n := m.endDisplayList(); n != 16 {
		t.Errorf("full list: %d bytes", n)
	}
	noWarnings(t, log)
}

// GX2SetupContextState makes a state that shadows the registers, and a
// display list that loads it again.
func TestContextState(t *testing.T) {
	g, log := cmdGPU(t)
	m := g.m
	m.gpu = g
	const ctx = 0x10900000
	m.setupContextState(ctx, 0)
	n := m.Mem.Read32(ctx + ctxLoadDLSize)
	if n == 0 || n > 0x300 || n%32 != 0 {
		t.Fatalf("load list size %d", n)
	}
	m.setRegs(regCB_TARGET_MASK, 0x5a)
	off := uint32(ctx + ctxAreaContext + (regCB_TARGET_MASK - baseContext))
	if v := m.Mem.Read32(off); v != 0x5a {
		t.Fatalf("shadowed value %x", v)
	}
	m.setContextState(0)
	m.setRegs(regCB_TARGET_MASK, 0x11)
	if m.Mem.Read32(off) != 0x5a {
		t.Fatal("written with shadowing off")
	}
	m.setContextState(ctx)
	if v := g.reg(regCB_TARGET_MASK); v != 0x5a {
		t.Errorf("restored %x", v)
	}
	if v := g.reg(regPA_CL_VTE_CNTL); v != 0x43f {
		t.Errorf("default PA_CL_VTE_CNTL %x", v)
	}
	noWarnings(t, log)
}
