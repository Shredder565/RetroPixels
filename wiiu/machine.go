// Package wiiu is RetroPixels' Wii U emulator, written from scratch.
//
// The Wii U runs programs on Cafe OS. Rather than running Nintendo's
// operating system (which RetroPixels doesn't have and doesn't need), the
// emulator runs the program's own PowerPC code (cpu.go, fpu.go) and gives it
// RetroPixels' own versions of the system libraries it calls (coreinit,
// vpad, gx2 and the rest: hle.go and the files named after each library).
// This is the approach Cemu and decaf-emu take too.
//
// What runs today: homebrew programs (.rpx) that draw with OSScreen or GX2,
// read the GamePad and use threads, memory heaps and the other core OS
// services. The GPU (Latte) has a software renderer (latte_*.go) and, on
// Windows, a Direct3D 11 backend that translates its shaders
// (latte_dxbc*.go, latte_d3d_windows.go).
package wiiu

import (
	"errors"
	"fmt"
	"os"
	"sort"
	"strings"
	"sync/atomic"
)

const (
	CoreHz        = 1243125000                   // Espresso clock
	BusHz         = 248625000                    // bus clock
	TimerHz       = BusHz / 4                    // the time base (OSGetTime ticks per second)
	FPS           = 59.94                        // TV refresh
	TicksPerFrame = uint64(TimerHz * 100 / 5994) // time base ticks per frame

	sliceLen = 2000 // instructions a core runs before the next core's turn
)

// Machine is a Wii U running one program.
type Machine struct {
	Mem   *Memory
	CPU   [3]*CPU
	Files *FileSystem

	Modules []*Module

	// library entry points
	hle      []hleEntry
	hleIndex map[string]uint32
	dataAddr map[string]uint32
	warned   map[string]bool
	sysNext  uint32

	// threads
	threads      map[uint32]*Thread
	threadList   []*Thread
	current      [3]*Thread
	running      *Thread // the thread whose instructions are running now
	mainThread   *Thread
	defaultTh    [3]*Thread
	nextThreadID uint32
	runSeq       uint64
	mutexes      map[uint32]bool
	alarms       []*Alarm
	alarmTh      [3]*Thread
	alarmQueue   [3][]Callback
	alarmContext uint32

	// time
	tb       uint64 // time base at the start of the current slice
	frameEnd uint64
	Frame    uint64 // frames run
	Budget   int    // instructions each core may run per frame
	ranBase  [3]uint64

	heaps  *Heaps
	screen *OSScreen
	gx2    *GX2
	gpu    *Latte
	audio  *AX
	pad    *GamePad
	procui *ProcUI

	argv    uint32
	dataEnd uint32 // end of the loaded modules' data (the heaps start after)

	Exited   bool
	ExitCode uint32
	Err      error

	jumped bool // a library call set PC itself
	fs     *fsState

	bootTime           uint64
	rumbleUntil        uint64
	initQueue          []*Module // loaded modules whose entry point hasn't run yet
	dynReturn          map[*Thread]uint32
	loading            map[string]bool // title libraries being loaded
	homeMenu           bool
	intsOff            [3]bool
	dynHandles         map[uint32]string
	dynAlloc           uint32
	fontAddr, fontSize uint32 // the system font, once a program asks
	fontTried          bool
	gpuNote            string               // why there's no host GPU
	missing            []string             // functions called that aren't here yet
	vmNext             uint32               // the next free virtual address (coreinit_vm.go)
	vmMaps             map[uint32]uint32    // mapped virtual pages: their physical address
	lcache             [3]lockedCache       // each core's locked cache
	flockWord          uint32               // the C library's file lock (stubs_more.go)
	hleCalls           []uint32             // how often each library call was made (for the log)
	hleLast            uint32               // the latest library call (for the log)
	apertures          map[uint32]*aperture // open tiling apertures by handle
	apAddrs            []uint32             // the last aperture surface's element addresses
	apAddrsKey         apertureKey
	nextAperture       uint32
	zstreams           map[uint32]*zInflate   // zlib inflate streams by z_stream address
	conts              map[uint32][]guestCont // by thread: what HLE functions do after calls into the program
	dynFree            uint32
	perThread          map[string]uint32

	// Log gets the program's console output (OSReport) and RetroPixels'
	// notes about it.
	Log func(string)
	// Trace, when set, gets every library call.
	Trace func(string)

	logBuf []string
}

// New loads a program (.rpx) and gets it ready to run. files is the title's
// file system (nil for a program on its own).
func New(rpx []byte, name string, files *FileSystem) (*Machine, error) {
	m := newMachine(files)
	if err := m.load(rpx, name); err != nil {
		return nil, err
	}
	return m, nil
}

func newMachine(files *FileSystem) *Machine {
	m := &Machine{
		Mem:      NewMemory(),
		Files:    files,
		hleIndex: map[string]uint32{},
		dataAddr: map[string]uint32{},
		warned:   map[string]bool{},
		threads:  map[uint32]*Thread{},
		mutexes:  map[uint32]bool{},
		sysNext:  SystemBase + 0x200000,
		Budget:   4000000,
	}
	if m.Files == nil {
		m.Files = NewFileSystem()
	}
	if jitSupported && os.Getenv("RETROPIXELS_WIIU_JIT") != "0" {
		m.Mem.jit = newJIT(m.Mem)
	}
	m.Mem.Fault = func(addr uint32, write bool) {
		c := m.runningCPU()
		pc := uint32(0)
		if c != nil {
			pc = c.PC - 4
		}
		what := "read from"
		if write {
			what = "write to"
		}
		err := fmt.Errorf("the program tried to %s %08X, which isn't memory (at %08X%s)", what, addr, pc, m.where(pc))
		if m.Err == nil {
			m.Err = err
		}
		if c != nil {
			c.Stop()
		}
	}
	for i := range m.CPU {
		c := &CPU{Mem: m.Mem, Core: i, Context: &Context{}}
		c.HLE = m.callHLE
		cc := c
		c.TB = func() uint64 { return m.tbOf(cc) }
		m.CPU[i] = c
	}
	m.heaps = newHeaps()
	m.screen = newOSScreen()
	m.gx2 = newGX2()
	m.gpu = newLatte(m)
	m.pad = &GamePad{}
	m.procui = &ProcUI{}
	m.initSystemData()
	return m
}

func (m *Machine) where(pc uint32) string {
	if s := m.symbolize(pc); s != "" {
		return ", in " + s
	}
	return ""
}

func (m *Machine) runningCPU() *CPU {
	for _, c := range m.CPU {
		if m.current[c.Core] != nil && m.current[c.Core] == m.running {
			return c
		}
	}
	return nil
}

// tbOf is the time base as core c sees it now.
func (m *Machine) tbOf(c *CPU) uint64 {
	n := c.Ran - m.ranBase[c.Core]
	return m.tb + n*TicksPerFrame/uint64(m.Budget)
}

// Time is the time base now.
func (m *Machine) Time() uint64 {
	if c := m.runningCPU(); c != nil {
		return m.tbOf(c)
	}
	return m.tb
}

func (m *Machine) logf(format string, a ...interface{}) {
	s := fmt.Sprintf(format, a...)
	if m.Log != nil {
		m.Log(s)
	}
	m.logBuf = append(m.logBuf, s)
	if len(m.logBuf) > 200 {
		m.logBuf = m.logBuf[len(m.logBuf)-200:]
	}
}

// RecentLog is the last lines of the program's log.
func (m *Machine) RecentLog() []string { return append([]string(nil), m.logBuf...) }

func (m *Machine) mainModule() *Module {
	if len(m.Modules) == 0 {
		return nil
	}
	return m.Modules[0]
}

func (m *Machine) moduleNamed(name string) *Module {
	name = libName(name)
	for _, mod := range m.Modules {
		if libName(mod.Name) == name {
			return mod
		}
	}
	return nil
}

// trampolines for calls that are too far for a direct branch
func (m *Machine) tramp(target uint32) uint32 {
	a := m.sysAlloc(16, 16)
	m.Mem.Write32(a, 0x3d800000|(target>>16))      // lis r12, hi
	m.Mem.Write32(a+4, 0x618c0000|(target&0xffff)) // ori r12, r12, lo
	m.Mem.Write32(a+8, 0x7d8903a6)                 // mtctr r12
	m.Mem.Write32(a+12, 0x4e800420)                // bctr
	return a
}

func (m *Machine) load(rpx []byte, name string) error {
	if err := ParseRPL(rpx); err != nil {
		return fmt.Errorf("that doesn't look like a Wii U program (%v)", err)
	}
	mod, err := m.loadModule(rpx, name, false)
	if err != nil {
		return err
	}
	_ = mod
	m.startMain()
	return nil
}

// loadModule loads an RPX or RPL. RPLs go after what's already loaded
// (including modules still loading, whose imports load this one).
func (m *Machine) loadModule(b []byte, name string, relocate bool) (*Module, error) {
	plan := LoadPlan{}
	if relocate {
		plan.Relocate = true
		textEnd, dataEnd := uint32(CodeBase), uint32(DataBase)
		loadEnd := uint32(LoaderBase)
		for _, mod := range m.Modules {
			textEnd = max(textEnd, mod.TextHi)
			dataEnd = max(dataEnd, mod.DataHi)
		}
		loadEnd += uint32(len(m.Modules)) * 0x100000
		plan.TextAt = (textEnd + 0xfff) &^ 0xfff
		plan.DataAt = (dataEnd + 0xfff) &^ 0xfff
		plan.LoadAt = loadEnd
	}
	var placed *Module
	plan.Placed = func(mod *Module) {
		placed = mod
		m.Modules = append(m.Modules, mod)
		mod.TLSIndex = uint32(len(m.Modules))
	}
	mod, err := LoadRPL(m.Mem, b, name, plan, m, m.tramp)
	if err != nil {
		if placed != nil {
			for i, x := range m.Modules {
				if x == placed {
					m.Modules = append(m.Modules[:i], m.Modules[i+1:]...)
					break
				}
			}
		}
		if m.Err != nil {
			return nil, m.Err
		}
		return nil, err
	}
	if placed == nil { // (a loader without the callback)
		m.Modules = append(m.Modules, mod)
		mod.TLSIndex = uint32(len(m.Modules))
	}
	m.Mem.InvalidateCode(mod.TextLo, mod.TextHi-mod.TextLo)
	// entry points run dependencies first: the order loads finish in
	m.initQueue = append(m.initQueue, mod)
	if mod.DataHi > m.dataEnd {
		m.dataEnd = mod.DataHi
	}
	if m.Err != nil {
		return nil, m.Err
	}
	return mod, nil
}

// titleLibrary loads one of the title's own RPLs from its code folder, if
// it has it (nil if not).
func (m *Machine) titleLibrary(lib string) (*Module, error) {
	b, err := m.Files.ReadCode(lib + ".rpl")
	if err != nil {
		return nil, nil
	}
	mod, err := m.loadModule(b, lib+".rpl", true)
	if err != nil {
		return nil, fmt.Errorf("couldn't load %s.rpl: %v", lib, err)
	}
	return mod, nil
}

// nextInit takes the next module whose entry point should run (not the
// main program's).
func (m *Machine) nextInit() *Module {
	for len(m.initQueue) > 0 {
		mod := m.initQueue[0]
		m.initQueue = m.initQueue[1:]
		if mod != m.mainModule() && mod.Entry != 0 {
			return mod
		}
	}
	return nil
}

// startMain makes the heaps and the main thread.
func (m *Machine) startMain() {
	main := m.mainModule()
	m.heaps.setup(m)

	// the default threads of the three cores; the program runs on core 1's
	stack := main.Info.StackSize
	if stack < 0x10000 {
		stack = 0x10000
	}
	for core := 0; core < 3; core++ {
		addr := m.sysAlloc(osThreadSize, 64)
		size := uint32(0x10000)
		if core == 1 {
			size = stack
		}
		st := m.heaps.allocSystem(m, size, 64) + size
		t := m.newThread(addr, 0, 0, 0, st, size, 16, 1<<core)
		t.Attr = 1 << core
		m.defaultTh[core] = t
		t.State = thDormant
		t.Suspend = 0
		if core == 1 {
			// argv: the program's name
			argv := m.sysAlloc(16, 8)
			arg0 := m.sysAlloc(uint32(len(main.Name)+1), 8)
			m.Mem.WriteString(arg0, main.Name)
			m.Mem.Write32(argv, arg0)
			m.argv = argv
			// __RPStartup runs the libraries' entry points, then main's
			t.Ctx.PC = m.thunk("coreinit", "__RPStartup")
			t.State = thReady
			m.mainThread = t
			m.Mem.Write32(addr+osThreadName, m.cstrAlloc("{SYS Thread Default Core 1}"))
		}
		m.syncThread(t)
	}
	m.frameEnd = TicksPerFrame
}

func (m *Machine) cstrAlloc(s string) uint32 {
	a := m.sysAlloc(uint32(len(s)+1), 4)
	m.Mem.WriteString(a, s)
	return a
}

// exitProgram ends the program (exit, or the main thread returning).
func (m *Machine) exitProgram(code uint32) {
	if !m.Exited {
		m.logf("the program ended (exit code %d)", int32(code))
	}
	m.Exited = true
	m.ExitCode = code
	for _, c := range m.CPU {
		c.Stop()
	}
}

// RunFrame runs the program for one TV frame.
func (m *Machine) RunFrame() {
	if m.Exited || m.Err != nil {
		return
	}
	m.pad.sample()
	for m.tb < m.frameEnd && !m.Exited && m.Err == nil {
		m.wakeTimers()
		ran := false
		for core := 0; core < 3; core++ {
			if m.runCore(core) {
				ran = true
			}
			if m.Exited || m.Err != nil {
				break
			}
		}
		if ran {
			m.tb += sliceLen * TicksPerFrame / uint64(m.Budget)
		} else {
			// every core is idle: skip to the next timer or the frame's end
			next := m.nextTimer()
			if next == 0 || next > m.frameEnd {
				next = m.frameEnd
			}
			if next <= m.tb {
				next = m.tb + 1
			}
			m.tb = next
		}
	}
	if m.tb >= m.frameEnd {
		m.vsync()
	}
}

// runCore runs one slice on a core; false when it had nothing to run.
func (m *Machine) runCore(core int) bool {
	c := m.CPU[core]
	t := m.current[core]
	if t == nil || t.State != thReady {
		t = m.pick(core)
		m.setCurrent(core, t)
	} else if n := m.pick(core); n != nil && n.Priority <= t.Priority {
		// a higher-priority thread, or one of the same priority's turn
		if n.Priority < t.Priority || n.LastRun < t.LastRun {
			t = n
			m.setCurrent(core, t)
		}
	}
	m.ranBase[core] = c.Ran
	if t == nil {
		return false
	}
	m.running = t
	t.Core = core
	left := sliceLen
	for left > 0 && !m.Exited && m.Err == nil {
		before := c.Ran
		c.Run(left)
		left -= int(c.Ran - before)
		if c.Err != nil {
			m.Err = m.crashReport(c, t)
			break
		}
		if t.blocked || t.State != thReady || m.current[core] != t {
			t.blocked = false
			m.syncThread(t)
			// another thread can use the rest of the slice
			n := m.pick(core)
			m.setCurrent(core, n)
			if n == nil {
				break
			}
			t = n
			m.running = t
			t.Core = core
		}
	}
	m.runSeq++
	t.LastRun = m.runSeq
	m.running = nil
	return true
}

func (m *Machine) setCurrent(core int, t *Thread) {
	prev := m.current[core]
	if prev != nil && prev != t {
		prev.Ctx.ResValid = false
	}
	m.current[core] = t
	c := m.CPU[core]
	if t != nil {
		c.Context = &t.Ctx
		t.Ctx.ResValid = false
	} else {
		c.Context = &Context{}
	}
}

func (m *Machine) crashReport(c *CPU, t *Thread) error {
	var b strings.Builder
	b.WriteString(c.Err.Error())
	if s := m.symbolize(c.Err.(*CPUError).PC); s != "" {
		b.WriteString(" in " + s)
	}
	fmt.Fprintf(&b, "\nLR %08X%s", c.LR, m.where(c.LR))
	return errors.New(b.String())
}

// vsync ends a frame: the next frame starts, and threads waiting for it go on.
func (m *Machine) vsync() {
	m.Frame++
	m.frameEnd += TicksPerFrame
	m.gx2.vsync(m)
	m.wake(keyVsync)
}

// ThreadsSummary describes the threads, for debugging.
func (m *Machine) ThreadsSummary() string {
	var b strings.Builder
	ts := append([]*Thread(nil), m.threadList...)
	sort.Slice(ts, func(i, j int) bool { return ts[i].ID < ts[j].ID })
	for _, t := range ts {
		fmt.Fprintf(&b, "thread %d %q state %d prio %d pc %08X%s wait %x\n", t.ID, m.Mem.String(t.Name, 64),
			t.State, t.Priority, t.Ctx.PC, m.where(t.Ctx.PC), t.WaitKey)
		fmt.Fprintf(&b, "    called from %08X%s%s\n", t.Ctx.LR, m.where(t.Ctx.LR), m.backtrace(t.Ctx.GPR[1]))
	}
	return b.String()
}

// Stats is a one-line summary of how the program is getting on, for logs.
func (m *Machine) Stats() string {
	return fmt.Sprintf("frame %d, pictures shown %d, draws %d, renderer %s", m.Frame, m.gx2.Flips, m.gpu.Draws, m.Renderer())
}

// backtrace follows a thread's stack frames (the back chain at each
// frame's start, the caller's return address 4 bytes into the next).
func (m *Machine) backtrace(sp uint32) string {
	var b strings.Builder
	for i := 0; i < 6; i++ {
		if sp == 0 || sp&3 != 0 || !m.Mem.Mapped(sp) {
			break
		}
		next := m.Mem.Read32(sp)
		if next <= sp || !m.Mem.Mapped(next) {
			break
		}
		lr := m.Mem.Read32(next + 4)
		fmt.Fprintf(&b, "\n      < %08X%s", lr, m.where(lr))
		sp = next
	}
	return b.String()
}

// BusyCalls lists the library calls made most since the last time it was
// asked, for the log: a program stuck in a loop shows what it's waiting on.
func (m *Machine) BusyCalls() string {
	type nc struct {
		n    uint32
		name string
	}
	var l []nc
	for i, n := range m.hleCalls {
		if n > 0 {
			l = append(l, nc{n, m.hle[i].lib + "." + m.hle[i].name})
			m.hleCalls[i] = 0
		}
	}
	sort.Slice(l, func(i, j int) bool { return l[i].n > l[j].n })
	if len(l) > 15 {
		l = l[:15]
	}
	parts := make([]string, len(l))
	for i, x := range l {
		parts[i] = fmt.Sprintf("%s x%d", x.name, x.n)
	}
	return strings.Join(parts, ", ")
}

// LastCall is the latest library call made. It may be read while the
// machine runs (for a watchdog), so it's only a hint.
func (m *Machine) LastCall() string {
	n := atomic.LoadUint32(&m.hleLast)
	if int(n) >= len(m.hle) {
		return "?"
	}
	return m.hle[n].lib + "." + m.hle[n].name
}

// CorePCs is where each core last stopped, as a hint for a watchdog (it may
// be read while the machine runs).
func (m *Machine) CorePCs() string {
	var b strings.Builder
	for i, c := range m.CPU {
		fmt.Fprintf(&b, " core %d pc %08X lr %08X", i, c.PC, c.LR)
	}
	return b.String()
}
