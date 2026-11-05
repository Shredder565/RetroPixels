// Package nds is the Nintendo DS machine of PixelDS, RetroPixels' own DS and
// 3DS emulator, written for RetroPixels from the hardware's documented
// behaviour (no code from other DS emulators).
//
// The machine: the ARM946E-S main processor with its caches' tightly coupled
// memories and protection unit registers (cp15.go), the ARM7TDMI sound and
// I/O processor, 4 MB of main memory, the shared and private work RAM, the
// nine VRAM banks, both 2D graphics engines (gpu2d.go), the 3D engine
// (gpu3d.go), sixteen-channel sound (spu.go), timers, DMA, the IPC
// registers and FIFOs, the hardware divider and square root, the game card
// with its save chip (cart.go), the firmware, touch screen and power chips on
// the SPI bus (spi.go) and the real-time clock (rtc.go).
//
// Games start the way the DS's own boot menu starts them (direct boot): the
// header and both programs are copied from the card, and RetroPixels' BIOS
// (bios.go) answers the BIOS calls, so no Nintendo BIOS or firmware file is
// needed.
package nds

import (
	"encoding/binary"
	"errors"
	"fmt"
	"strings"
	"time"

	"retropixels/cores/pixelds/arm"
)

// Clocks (in ARM9 cycles, the machine's time unit: 67.027964 MHz).
const (
	ARM9Hz       = 67027964
	lineCycles   = 2130 * 2 // 355 dots of 6 ARM7 cycles
	hblankStart  = 1606 * 2 // HBlank flag rises shortly after the 256 visible dots
	linesPerScr  = 263
	visibleLines = 192
	frameCycles  = lineCycles * linesPerScr
	FPS          = float64(ARM9Hz) / frameCycles // 59.8261
	sliceCycles  = 64
	never        = int64(1) << 62
)

// Screen size: two 256x192 screens.
const (
	ScreenW = 256
	ScreenH = 192
)

// Buttons in input bit order.
var Buttons = []string{"A", "B", "Select", "Start", "Right", "Left", "Up", "Down", "R", "L", "X", "Y", "Lid"}

// Machine is one DS running a game.
type Machine struct {
	now int64 // ARM9 cycles since power-on

	arm9, arm7 *arm.CPU
	bus9       *bus9
	bus7       *bus7
	cp15       *cp15

	mainRAM  []byte // 4 MB
	sharedWR []byte // 32 KB
	arm7WRAM []byte // 64 KB
	bios9    []byte // RetroPixels' stubs
	bios7    []byte
	wramcnt  uint8

	vram vram
	pal  []byte // 2 KB: engine A BG, A OBJ, B BG, B OBJ
	oam  []byte // 2 KB: engine A, engine B

	irq9, irq7     irqCtl
	timers9        [4]timer
	timers7        [4]timer
	dma9, dma7     [4]dma
	dmaFill        [4]uint32
	ipc            ipc
	math           mathUnit
	gpuA, gpuB     *engine2D
	gpu3d          *gpu3D
	spu            *spu
	cart           *cart
	spi            *spiBus
	rtc            rtc
	powcnt1        uint16
	exmemcnt       uint16
	postflg9       uint8
	postflg7       uint8
	haltcnt        uint8
	rcnt           uint16
	keycnt9        uint16
	keycnt7        uint16
	exmemcnt7      uint16
	powcnt2        uint16
	bios7prot      uint16
	vcount         int
	dispstat9      uint16
	dispstat7      uint16
	lineStart      int64 // when the current line began
	hblankDone     bool
	frameDone      bool
	nextLineEvent  int64
	keys           uint32 // Buttons bits, 1 = held
	touchX, touchY int
	touching       bool
	dispCapCnt     uint32
	capturing      bool

	frame []uint32 // 256x384: top screen then bottom screen

	header []byte
	title  string
	code   string
}

// Options are what the frontend hands the machine.
type Options struct {
	SampleRate int
	Save       []byte
	Now        func() time.Time
	Firmware   []byte // a firmware image to use instead of the made-up one (optional)
	UserName   string
}

// New loads a DS game (an .nds file).
func New(rom []byte, o Options) (*Machine, error) {
	if len(rom) < 0x200 {
		return nil, errors.New("this file is too small to be a DS game")
	}
	h := rom[:0x200]
	arm9Off, arm9Entry, arm9Addr, arm9Size := le32(h, 0x20), le32(h, 0x24), le32(h, 0x28), le32(h, 0x2c)
	arm7Off, arm7Entry, arm7Addr, arm7Size := le32(h, 0x30), le32(h, 0x34), le32(h, 0x38), le32(h, 0x3c)
	if arm9Size > 0x3bfe00 || arm7Size > 0x3bfe00 || int(arm9Off+arm9Size) > len(rom) || int(arm7Off+arm7Size) > len(rom) {
		return nil, errors.New("this file isn't a DS game (its header doesn't describe one)")
	}
	if o.SampleRate <= 0 {
		o.SampleRate = 48000
	}
	if o.Now == nil {
		o.Now = time.Now
	}
	m := &Machine{
		mainRAM:  make([]byte, 4<<20),
		sharedWR: make([]byte, 32<<10),
		arm7WRAM: make([]byte, 64<<10),
		pal:      make([]byte, 2048),
		oam:      make([]byte, 2048),
		frame:    make([]uint32, ScreenW*ScreenH*2),
		header:   append([]byte(nil), h...),
	}
	m.title = strings.TrimRight(string(h[0:12]), "\x00 ")
	m.code = string(h[12:16])
	m.bios9 = wordsToBytes(arm9BIOS, 4<<10)
	m.bios7 = wordsToBytes(arm7BIOS, 16<<10)
	m.vram.init()
	m.cart = newCart(m, rom, o.Save)
	m.spi = newSPI(m, o.Firmware, o.UserName)
	m.rtc.now = o.Now
	m.rtc.status1 = 2 // 24-hour clock
	m.gpuA = newEngine2D(m, true)
	m.gpuB = newEngine2D(m, false)
	m.gpu3d = newGPU3D(m)
	m.spu = newSPU(m, o.SampleRate)

	m.cp15 = newCP15(m)
	m.bus9 = &bus9{m: m}
	m.bus7 = &bus7{m: m}
	m.arm9 = arm.New(m.bus9, arm.V5TE)
	m.arm9.CP15 = m.cp15
	m.arm9.Cost = 2
	m.arm7 = arm.New(m.bus7, arm.V4T)
	m.arm9.SWI = m.swi9
	m.arm7.SWI = m.swi7
	m.irq9.cpu, m.irq7.cpu = m.arm9, m.arm7
	for i := range m.timers9 {
		m.timers9[i] = timer{m: m, irq: &m.irq9, n: i}
		m.timers7[i] = timer{m: m, irq: &m.irq7, n: i}
	}
	for i := range m.dma9 {
		m.dma9[i] = dma{m: m, arm9: true, n: i}
		m.dma7[i] = dma{m: m, n: i}
	}

	m.directBoot(rom, arm9Off, arm9Entry, arm9Addr, arm9Size, arm7Off, arm7Entry, arm7Addr, arm7Size)
	m.nextLineEvent = hblankStart
	return m, nil
}

func le32(b []byte, o int) uint32 { return binary.LittleEndian.Uint32(b[o:]) }

func wordsToBytes(w []uint32, size int) []byte {
	b := make([]byte, size)
	for i, v := range w {
		binary.LittleEndian.PutUint32(b[4*i:], v)
	}
	return b
}

// directBoot sets the machine up the way the DS's boot menu leaves it when a
// game starts.
func (m *Machine) directBoot(rom []byte, arm9Off, arm9Entry, arm9Addr, arm9Size, arm7Off, arm7Entry, arm7Addr, arm7Size uint32) {
	// the card header, where the boot menu leaves it
	copy(m.mainRAM[0x3ffe00:], rom[:0x170])
	// the card's chip ID and the boot menu's notes
	id := m.cart.chipID
	for _, a := range []uint32{0x3ff800, 0x3ff804, 0x3ffc00, 0x3ffc04} {
		binary.LittleEndian.PutUint32(m.mainRAM[a:], id)
	}
	hcrc := binary.LittleEndian.Uint16(rom[0x15e:])
	scrc := binary.LittleEndian.Uint16(rom[0x6c:])
	binary.LittleEndian.PutUint16(m.mainRAM[0x3ff808:], hcrc)
	binary.LittleEndian.PutUint16(m.mainRAM[0x3ffc08:], hcrc)
	binary.LittleEndian.PutUint16(m.mainRAM[0x3ff80a:], scrc)
	binary.LittleEndian.PutUint16(m.mainRAM[0x3ffc0a:], scrc)
	binary.LittleEndian.PutUint16(m.mainRAM[0x3ff850:], 0x5835)
	binary.LittleEndian.PutUint16(m.mainRAM[0x3ffc10:], 0x5835)
	binary.LittleEndian.PutUint16(m.mainRAM[0x3ffc30:], 0xffff)
	binary.LittleEndian.PutUint16(m.mainRAM[0x3ffc40:], 1) // booted from a card
	// the user's settings from the firmware
	copy(m.mainRAM[0x3ffc80:], m.spi.fw.userSettings())

	// the two programs
	for i := uint32(0); i < arm9Size; i += 4 {
		m.bus9.Write32(arm9Addr+i, le32(rom, int(arm9Off+i)))
	}
	for i := uint32(0); i < arm7Size; i += 4 {
		m.bus7.Write32(arm7Addr+i, le32(rom, int(arm7Off+i)))
	}

	m.wramcnt = 3
	m.vram.wramcnt = 3
	m.postflg9, m.postflg7 = 1, 1
	m.powcnt1 = 0x820f
	m.exmemcnt = 0x6000
	m.spu.bias = 0x200

	c9 := m.arm9
	c9.VectorBase = 0xffff0000
	c9.SetMode(arm.ModeIRQ)
	c9.R[13] = 0x03003f80
	c9.SetMode(arm.ModeSVC)
	c9.R[13] = 0x03003fc0
	c9.SetMode(arm.ModeSYS)
	c9.R[13] = 0x03002f7c
	c9.R[12], c9.R[14] = arm9Entry, arm9Entry
	c9.CPSR = arm.ModeSYS
	c9.SetPC(arm9Entry)

	c7 := m.arm7
	c7.SetMode(arm.ModeIRQ)
	c7.R[13] = 0x0380ff80
	c7.SetMode(arm.ModeSVC)
	c7.R[13] = 0x0380ffc0
	c7.SetMode(arm.ModeSYS)
	c7.R[13] = 0x0380fd80
	c7.R[12], c7.R[14] = arm7Entry, arm7Entry
	c7.CPSR = arm.ModeSYS
	c7.SetPC(arm7Entry)
}

// Title is the game's name from its header.
func (m *Machine) Title() string { return m.title }

// GameCode is the four-letter code from the header ("AMCE", ...).
func (m *Machine) GameCode() string { return m.code }

// Frame is the last picture: the top screen above the bottom screen,
// 0x00RRGGBB.
func (m *Machine) Frame() []uint32 { return m.frame }

// SetKeys sets the held buttons (Buttons order).
func (m *Machine) SetKeys(bits uint32) { m.keys = bits }

// SetTouch puts the stylus on the bottom screen at x, y (pixels), or lifts
// it when down is false.
func (m *Machine) SetTouch(x, y int, down bool) {
	if x < 0 {
		x = 0
	}
	if x > 255 {
		x = 255
	}
	if y < 0 {
		y = 0
	}
	if y > 191 {
		y = 191
	}
	m.touchX, m.touchY, m.touching = x, y, down
}

// TakeAudio returns the stereo samples made since the last call.
func (m *Machine) TakeAudio() []float32 { return m.spu.take() }

// Save is the save chip's contents, or nil when the game hasn't used it.
func (m *Machine) Save() []byte { return m.cart.backup.data() }

// SaveChanged reports (and clears) whether the save changed.
func (m *Machine) SaveChanged() bool { return m.cart.backup.takeDirty() }

// RunFrame runs until the next frame is finished.
func (m *Machine) RunFrame() {
	m.frameDone = false
	for !m.frameDone {
		target := m.now + sliceCycles
		if ev := m.nextEvent(); ev < target {
			target = ev
		}
		if target > m.now {
			m.arm9.Run(target)
			m.arm7.Run(target >> 1)
			m.now = target
		}
		m.runEvents()
	}
}

func (m *Machine) nextEvent() int64 {
	t := m.nextLineEvent
	for i := range m.timers9 {
		if e := m.timers9[i].overflowAt; e < t {
			t = e
		}
		if e := m.timers7[i].overflowAt; e < t {
			t = e
		}
	}
	if m.cart.doneAt < t {
		t = m.cart.doneAt
	}
	if m.spi.doneAt < t {
		t = m.spi.doneAt
	}
	if m.gpu3d.busyUntil < t && m.gpu3d.waiting() {
		t = m.gpu3d.busyUntil
	}
	return t
}

func (m *Machine) runEvents() {
	for i := range m.timers9 {
		if m.timers9[i].overflowAt <= m.now {
			m.timers9[i].overflow()
		}
		if m.timers7[i].overflowAt <= m.now {
			m.timers7[i].overflow()
		}
	}
	if m.cart.doneAt <= m.now {
		m.cart.finish()
	}
	if m.spi.doneAt <= m.now {
		m.spi.finish()
	}
	if m.gpu3d.waiting() && m.gpu3d.busyUntil <= m.now {
		m.gpu3d.run()
	}
	for m.nextLineEvent <= m.now {
		m.lineEvent()
	}
}

// lineEvent: HBlank begins, or a new line begins.
func (m *Machine) lineEvent() {
	if !m.hblankDone {
		m.hblankDone = true
		m.nextLineEvent = m.lineStart + lineCycles
		m.dispstat9 |= 2
		m.dispstat7 |= 2
		if m.vcount < visibleLines {
			m.gpuA.renderLine(m.vcount)
			m.gpuB.renderLine(m.vcount)
			if m.capturing {
				m.captureLine(m.vcount)
			}
			m.finishLine(m.vcount)
			m.dmaEvent9(dmaHBlank)
		}
		if m.dispstat9&0x10 != 0 {
			m.irq9.raise(irqHBlank)
		}
		if m.dispstat7&0x10 != 0 {
			m.irq7.raise(irqHBlank)
		}
		return
	}
	// a new line
	m.hblankDone = false
	m.gpu3d.checkIRQ()
	m.lineStart += lineCycles
	m.nextLineEvent = m.lineStart + hblankStart
	m.dispstat9 &^= 2
	m.dispstat7 &^= 2
	m.vcount++
	if m.vcount == linesPerScr {
		m.vcount = 0
	}
	m.spu.runTo(m.now)
	switch m.vcount {
	case 0:
		m.dispstat9 &^= 1
		m.dispstat7 &^= 1
		m.frameDone = true
		m.gpuA.frameStart()
		m.gpuB.frameStart()
		m.capturing = m.dispCapCnt&(1<<31) != 0
	case visibleLines:
		m.dispstat9 |= 1
		m.dispstat7 |= 1
		if m.dispstat9&8 != 0 {
			m.irq9.raise(irqVBlank)
		}
		if m.dispstat7&8 != 0 {
			m.irq7.raise(irqVBlank)
		}
		m.dmaEvent9(dmaVBlank)
		m.dmaEvent7(dmaVBlank)
		m.gpu3d.vblank()
		if m.capturing {
			m.capturing = false
			m.dispCapCnt &^= 1 << 31
		}
	case linesPerScr - 48:
		// the 3D engine starts drawing the next frame 48 lines before it's shown
		m.gpu3d.renderFrame()
	}
	if m.vcount < visibleLines {
		m.dmaEvent9(dmaDisplayStart)
	}
	m.checkVCount()
}

func (m *Machine) checkVCount() {
	match := func(ds uint16) bool { return int(ds>>8|ds<<1&0x100) == m.vcount }
	if match(m.dispstat9) {
		m.dispstat9 |= 4
		if m.dispstat9&0x20 != 0 {
			m.irq9.raise(irqVCount)
		}
	} else {
		m.dispstat9 &^= 4
	}
	if match(m.dispstat7) {
		m.dispstat7 |= 4
		if m.dispstat7&0x20 != 0 {
			m.irq7.raise(irqVCount)
		}
	} else {
		m.dispstat7 &^= 4
	}
}

// finishLine puts line y of both engines on the two screens.
func (m *Machine) finishLine(y int) {
	top, bottom := m.gpuA, m.gpuB
	if m.powcnt1&0x8000 == 0 {
		top, bottom = m.gpuB, m.gpuA
	}
	copy(m.frame[y*ScreenW:], top.out[:])
	copy(m.frame[(ScreenH+y)*ScreenW:], bottom.out[:])
	if m.powcnt1&1 == 0 { // the screens are off
		for i := 0; i < ScreenW; i++ {
			m.frame[y*ScreenW+i] = 0
			m.frame[(ScreenH+y)*ScreenW+i] = 0
		}
	}
}

func (m *Machine) String() string { return fmt.Sprintf("DS game %q (%s)", m.title, m.code) }
