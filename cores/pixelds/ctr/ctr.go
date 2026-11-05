package ctr

import (
	"encoding/binary"
	"errors"
	"fmt"
	"math"
	"time"

	"retropixels/cores/pixelds/arm"
)

const (
	cpuHz   = 268111856
	FPS     = 59.831
	TopW    = 400
	BottomW = 320
	ScreenH = 240
)

// Options for a 3DS machine.
type Options struct {
	SDCard   string // folder used as the SD card
	SaveData string // folder for this game's save data
	UserName string
	Log      func(string)
}

// Machine is a 3DS running one application.
type Machine struct {
	opts    Options
	cpu     *arm.CPU
	mem     *memory
	kern    *kernel
	svcs    *services
	apt     *aptState
	gsp     *gspState
	hid     *hidState
	gpu     *gpu
	lcd     [2]lcdScreen
	frame   []uint32
	keys    uint32
	circle  [2]int
	touchOn bool
	touchX  int
	touchY  int
	romfs   []byte
	title   string
	// a game's program ID and product code (CTR-P-XXXX)
	programID   uint64
	fontHandle  uint32
	productCode string
	exited      bool
	err         error
	logged      int
	frameEnd    int64
}

// ErrExited: the program ended.
var ErrExited = errors.New("the program has ended")

func (m *Machine) logf(format string, a ...any) {
	if m.opts.Log == nil || m.logged > 400 {
		return
	}
	m.logged++
	m.opts.Log(fmt.Sprintf(format, a...))
}

func (m *Machine) fail(format string, a ...any) {
	if m.err == nil {
		m.err = fmt.Errorf(format, a...)
		m.logf("stopped: %v", m.err)
	}
	m.cpu.Halted = true
	m.kern.cur = nil
}

type cp15 struct{ m *Machine }

func (c cp15) MRC(op1, cn, cm, op2 uint32) uint32 {
	if cn == 13 && cm == 0 && op2 == 3 && c.m.kern.cur != nil { // the thread's TLS
		return c.m.kern.cur.tls
	}
	if cn == 0 && cm == 0 && op2 == 5 { // CPU ID: core 0
		return 0
	}
	return 0
}

func (c cp15) MCR(op1, cn, cm, op2, v uint32) {} // caches and barriers

func newMachine(opts Options) *Machine {
	m := &Machine{opts: opts, frame: make([]uint32, TopW*ScreenH*2)}
	m.mem = newMemory()
	m.cpu = arm.New(m.mem, arm.V6K)
	m.cpu.SVCLow = true
	m.cpu.Unaligned = true
	m.cpu.CP15 = cp15{m}
	m.kern = newKernel(m)
	m.svcs = newServices(m)
	m.gpu = newGPU(m)
	m.cpu.SWI = func(n uint32) bool {
		m.kern.svc(n)
		return true
	}
	m.cpu.Undefined = func(op uint32, thumb bool) bool {
		m.fail("unknown instruction %08x at %08x", op, m.cpu.PC())
		return true
	}
	m.mem.fault = func(a uint32, write bool) {
		what := "read"
		if write {
			what = "write"
		}
		m.fail("%s at unmapped address %08x (pc %08x)", what, a, m.cpu.PC())
	}
	m.setupSystemPages()
	return m
}

// setupSystemPages maps the configuration memory, the shared page, the
// thread-local storage, VRAM and DSP memory.
func (m *Machine) setupSystemPages() {
	pages, _ := m.mem.allocPages(2)
	m.mem.mapPhys(configMem, pages[0], pageSize)
	m.mem.mapPhys(sharedPage, pages[1], pageSize)
	tls, _ := m.mem.allocPages(8)
	for i, pa := range tls {
		m.mem.mapPhys(tlsBase+uint32(i)*pageSize, pa, pageSize)
	}
	m.mem.mapPhys(vramVBase, vramBase, vramSize)
	m.mem.mapPhys(dspVBase, dspBase, dspSize)

	cfg := m.mem.physSlice(pages[0], pageSize)
	le := binary.LittleEndian
	le.PutUint32(cfg[0x00:], 0x02370000) // kernel version 2.55
	le.PutUint32(cfg[0x04:], 0x00008002) // UPDATEFLAG / NS title id low (unused)
	le.PutUint32(cfg[0x14:], 0x00008002)
	le.PutUint32(cfg[0x30:], 1)          // ENVINFO: retail
	le.PutUint32(cfg[0x40:], 0)          // APPMEMTYPE
	le.PutUint32(cfg[0x44:], appMemSize) // APPMEMALLOC
	le.PutUint32(cfg[0x48:], 0x02c00000) // SYSMEMALLOC
	le.PutUint32(cfg[0x4c:], 0x01400000) // BASEMEMALLOC
	le.PutUint32(cfg[0x60:], 0x02370000) // FIRM version
	le.PutUint32(cfg[0x64:], 0x00000000) // FIRM system core version
	sh := m.mem.physSlice(pages[1], pageSize)
	sh[0x02] = 1    // RUNNING_HW: retail
	sh[0x04] = 0    // MCU HW info
	sh[0x67] = 0x0b // battery: adapter, charging, level 2
	sh[0x66] = 3    // wifi strength
	m.updateTime()
}

// updateTime writes the date and time into the shared page.
func (m *Machine) updateTime() {
	pa := m.mem.virtToPhys(sharedPage)
	sh := m.mem.physSlice(pa, pageSize)
	ms := uint64(time.Now().UnixMilli() - time.Date(1900, 1, 1, 0, 0, 0, 0, time.UTC).UnixMilli())
	if _, off := time.Now().Zone(); off != 0 {
		ms += uint64(off) * 1000
	}
	le := binary.LittleEndian
	le.PutUint32(sh[0:], 0)
	le.PutUint64(sh[0x20:], ms)
	le.PutUint64(sh[0x28:], uint64(m.cpu.Cycles))
	le.PutUint32(sh[0x30:], 0xffffffff) // drift
	le.PutUint32(sh[0x34:], 0)
}

// RunFrame runs one frame (1/59.83 s).
func (m *Machine) RunFrame() error {
	if m.err != nil {
		return m.err
	}
	if m.exited {
		return ErrExited
	}
	m.frameEnd += cpuHz / 60
	k := m.kern
	for m.cpu.Cycles < m.frameEnd && m.err == nil && !m.exited {
		until := m.frameEnd
		if e := k.nextEvent(); e < until {
			until = e
		}
		if k.cur == nil {
			m.cpu.Cycles = until // nothing to run until then
		} else {
			if until > m.cpu.Cycles+20000 {
				until = m.cpu.Cycles + 20000
			}
			m.cpu.Run(until)
		}
		k.tick()
	}
	m.gspVBlank()
	m.hidUpdate()
	if m.frameEnd%(cpuHz) < cpuHz/60 {
		m.updateTime()
	}
	m.render()
	return m.err
}

func (m *Machine) render() {
	clear(m.frame)
	m.drawScreen(0, m.frame, TopW, 0, 0, TopW)
	m.drawScreen(1, m.frame, TopW, (TopW-BottomW)/2, ScreenH, BottomW)
}

// Frame is the picture: the top screen (400×240) over the bottom one
// (320×240, centred), as 0xRRGGBB.
func (m *Machine) Frame() []uint32 { return m.frame }

// SetKeys sets the buttons held (Key*) and the circle pad (-156 to 156).
func (m *Machine) SetKeys(keys uint32, cx, cy int) {
	m.keys = keys
	m.circle = [2]int{cx, cy}
}

// SetTouch sets the stylus on the bottom screen (x 0-319, y 0-239).
func (m *Machine) SetTouch(on bool, x, y int) {
	m.touchOn, m.touchX, m.touchY = on, x, y
}

// Title is the program's name.
func (m *Machine) Title() string { return m.title }

// ProductCode is a game's code (CTR-P-XXXX), or "3DSX" for homebrew.
func (m *Machine) ProductCode() string {
	if m.productCode == "" {
		return "3DSX"
	}
	return m.productCode
}

// Err is why the machine stopped, if it did.
func (m *Machine) Err() error { return m.err }

// TakeAudio returns the sound made since the last call (none yet).
func (m *Machine) TakeAudio() []float32 { return nil }

func mathFloat32bits(f float32) uint32 { return math.Float32bits(f) }
