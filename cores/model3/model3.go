// Package model3 is the Sega Model 3 arcade board, RetroPixels' own
// emulation of it: the PowerPC 603e main board (cores/model3/ppc) with its
// memory map, interrupt controller, I/O board, EEPROM, clock and backup RAM;
// the tile generator; the Real3D Pro-1000 3D board, drawn by RetroPixels' own
// renderer; and the sound boards (SCSP with its 68000, and the MPEG music
// board).
//
// It is written from the hardware's behaviour. Supermodel (the Sega Model 3
// emulator) was used only as a reference to compare pictures and sound
// against; none of its code is used.
package model3

import (
	"encoding/binary"
	"fmt"
	"os"
	"time"

	"retropixels/cores/model3/ppc"
)

// Machine is one Model 3 board running a game.
type Machine struct {
	game *GameDef
	step int // 0x10, 0x15, 0x20, 0x21

	cpu *ppc.CPU

	ram         []byte // 8 MB, big endian
	crom        []byte // fixed program ROM, 8 MB at 0xFF800000
	bankedCROM  []byte // 128 MB, seen 8 MB at a time at 0xFF000000
	cromBank    uint32 // offset of the bank in bankedCROM
	cromBankReg uint8
	vrom        []byte
	backupRAM   []byte // 128 KB, battery backed
	securityRAM []byte

	irq    irqCtl
	tg     tileGen
	gpu    *real3D
	pci    pciBridge
	jtag   *jtag
	eeprom eeprom
	scsi   scsi

	inputBank         uint8
	adcChannel        uint8
	serialFIFO1       uint8
	serialFIFO2       uint8
	outputs           [2]uint8
	midiCtrl          uint8
	securityFirstRead bool
	crypt             *crypto

	sound *soundBoard
	drive *driveBoard

	// Controls
	Inputs Inputs

	// Now returns the time for the clock chip (time.Now by default).
	Now func() time.Time

	frame      uint64
	gpusReady  bool
	Picture    [screenW * screenH]uint32 // the last finished frame, 0x00RRGGBB
	render     *renderer
	nvramDirty bool
}

// Inputs are the cabinet's controls.
type Inputs struct {
	Coin1, Coin2, Start1, Start2, Test, Service bool
	// Analog joystick (Star Wars Trilogy): -1..1, right and up positive
	StickX, StickY   float32
	Trigger, Event   bool
	Trigger2, Event2 bool
	// Digital joystick and buttons for other games
	Up, Down, Left, Right bool
	Buttons               [6]bool
}

// Error is a problem loading or running a game, worded for the player.
type Error string

func (e Error) Error() string { return string(e) }

// New builds the board for a game from its ROM regions.
func New(g *GameDef, roms *ROMs) (*Machine, error) {
	m := &Machine{game: g, step: g.Step, Now: time.Now}
	m.ram = make([]byte, 0x800000)
	m.crom = roms.CROM
	m.bankedCROM = roms.BankedCROM
	m.vrom = roms.VROM
	m.backupRAM = make([]byte, 0x20000)
	m.securityRAM = make([]byte, 0x20000)
	m.crypt = newCrypto(g.Key, func(a uint32) uint16 {
		if a < 0x8000 {
			return binary.BigEndian.Uint16(m.securityRAM[a*4:])
		}
		return 0
	})
	for i := range m.eeprom.Words {
		m.eeprom.Words[i] = 0xFFFF
	}
	m.gpu = newReal3D(m.step, m.vrom)
	m.jtag = newJTAG(m.step)
	m.jtag.onMode = func(asic int, v uint32) {
		if asic >= 0 && asic < len(m.gpu.modeword) {
			m.gpu.modeword[asic] = v
		}
	}
	var pvr uint32 = ppc.PVR603R
	ratio := 10 // 4 bus clocks at 66 MHz, processor at 166 MHz (2.5x)
	switch m.step {
	case 0x15:
		pvr, ratio = ppc.PVR603E, 6
	case 0x10:
		pvr, ratio = ppc.PVR603R, 4
	}
	m.cpu = ppc.New(m, m.ram, m.crom, pvr, ratio)
	m.irq.line = m.cpu.SetIRQ
	m.render = newRenderer(m.gpu)
	m.sound = newSoundBoard(roms, g)
	m.drive = newDriveBoard(g)
	m.drive.m = m
	m.Reset()
	return m, nil
}

// CPUHz is the main processor's clock.
func (m *Machine) CPUHz() int {
	switch m.step {
	case 0x10:
		return 66000000
	case 0x15:
		return 100000000
	}
	return 166000000
}

// Reset is the board's reset button (backup RAM and the EEPROM are kept).
func (m *Machine) Reset() {
	clear(m.ram)
	m.setCROMBank(0xFF)
	m.securityFirstRead = true
	m.crypt.reset()
	m.inputBank, m.serialFIFO1, m.serialFIFO2, m.adcChannel = 0, 0, 0, 0
	m.midiCtrl = 0
	m.cpu.Reset()
	m.irq.enable, m.irq.pending = 0, 0
	m.cpu.SetIRQ(false)
	m.pci.reset()
	m.scsi.reset()
	m.eeprom.reset()
	m.tg.reset()
	m.gpu.reset()
	m.jtag.reset()
	m.sound.reset()
	m.drive.reset()
	m.gpusReady = false
	m.frame = 0
}

func (m *Machine) setCROMBank(v uint8) {
	m.cromBankReg = v
	idx := uint32(^v & 0xF)
	m.cromBank = (idx * 0x800000) % uint32(len(m.bankedCROM))
}

// Halted says why the processor stopped, if it did.
func (m *Machine) Halted() error {
	if m.cpu.Halted != nil {
		return m.cpu.Halted
	}
	return nil
}

// CPU is the main processor (for debugging).
func (m *Machine) CPU() *ppc.CPU { return m.cpu }

// ---------------------------------------------------------------- frame

// RunFrame runs one video frame (1/57.524 s) and draws its picture.
func (m *Machine) RunFrame() {
	hz := m.CPUHz()
	frameCycles := int(float32(hz) / 57.52416)
	lineCycles := frameCycles / 424
	vblank := lineCycles * 40

	if m.gpusReady {
		m.tg.clear()
		m.gpu.beginVBlank()
		// run until the vblank interrupt is acknowledged
		for m.irq.enable&irqVBlank != 0 && m.irq.pending&irqVBlank != 0 && vblank > 1000 {
			m.cpu.Run(1000)
			vblank -= 1000
		}
		// the sound board's MIDI interrupts, while the game has them on
		for n := 0; m.midiCtrl&0x20 != 0 && n <= 128; n++ {
			if m.irq.enable&irqSound == 0 {
				break
			}
			m.irq.assert(irqSound)
			m.cpu.Run(1000)
			vblank -= 1000
		}
		if vblank > 0 {
			m.cpu.Run(vblank)
		}
		m.irq.assert(0x0D)
	}

	flipLine := int(m.tg.regs[0x08/4])
	for line := 0; line < screenH; line++ {
		if line == flipLine {
			m.gpu.flipPingPong()
		}
		if line == screenH-1 {
			m.irq.assert(irqVBlank)
		}
		m.tg.drawLine(line)
		m.cpu.Run(lineCycles)
	}
	m.gpusReady = true

	m.compose()
	m.sound.runFrame()
	m.drive.runFrame()
	m.frame++
}

// compose draws the frame: the bottom tile layers, the 3D picture, the top
// tile layers.
func (m *Machine) compose() {
	m.gpu.frameReady = false
	m.render.draw()
	m.render.composite(m.tg.bottom[:], m.tg.top[:], m.Picture[:])
}

// ---------------------------------------------------------------- bus

func (m *Machine) Read8(a uint32) uint8 {
	switch a >> 24 {
	case 0xFF:
		return m.bankedCROM[m.cromBank+a&0x7FFFFF]
	case 0xC2:
		return m.dmaRead8(a & 0xFF)
	case 0xC1, 0xF9:
		return m.scsiRead(a & 0xFF)
	case 0xF0, 0xFE:
		switch a >> 16 & 0xFF {
		case 0x04:
			return m.readInputs(a & 0x3F)
		case 0x08:
			if a&0xF == 4 {
				return 0x83
			}
			return 0
		case 0x0C, 0x0D:
			return m.backupRAM[a&0x1FFFF]
		case 0x10:
			return m.readSysReg(a & 0x3F)
		case 0x14:
			switch a & 3 {
			case 1:
				return 3
			case 0:
				return rtcRead(a>>2&0xF, m.Now())
			}
			return 0
		case 0x18, 0x19:
			return m.securityRAM[a&0x1FFFF]
		}
	case 0xF1:
		if a < 0xF1120000 {
			return m.tg.vram[a&0x1FFFFF]
		}
	}
	return 0xFF
}

func (m *Machine) Read16(a uint32) uint16 {
	switch a >> 24 {
	case 0xFF:
		return binary.BigEndian.Uint16(m.bankedCROM[m.cromBank+a&0x7FFFFF:])
	case 0xF0, 0xFE:
		switch a >> 16 & 0xFF {
		case 0x0C, 0x0D:
			return binary.BigEndian.Uint16(m.backupRAM[a&0x1FFFF:])
		case 0x04, 0x10, 0x14, 0x08:
			return uint16(m.Read8(a))<<8 | uint16(m.Read8(a+1))
		case 0xC0:
			return 0xFFFF
		}
		if a>>16&0xFF >= 0xE0 && a>>16&0xFF <= 0xEF {
			return uint16(m.pciConfigRead(16, a&3))
		}
	case 0xF1:
		if a < 0xF1120000 {
			return binary.BigEndian.Uint16(m.tg.vram[a&0x1FFFFF:])
		}
	}
	return 0xFFFF
}

func (m *Machine) Read32(a uint32) uint32 {
	switch a >> 24 {
	case 0xFF:
		return binary.BigEndian.Uint32(m.bankedCROM[m.cromBank+a&0x7FFFFF:])
	case 0x84:
		return bswap32(m.gpu.readReg(a & 0x3F))
	case 0xC2:
		return bswap32(m.dmaRead32(a & 0xFF))
	case 0xC1, 0xF9:
		r := a & 0xFF
		return uint32(m.scsiRead(r))<<24 | uint32(m.scsiRead(r+1))<<16 | uint32(m.scsiRead(r+2))<<8 | uint32(m.scsiRead(r+3))
	case 0xF0, 0xFE:
		switch sub := a >> 16 & 0xFF; {
		case sub == 0x04:
			r := a & 0x3F
			return uint32(m.readInputs(r))<<24 | uint32(m.readInputs(r+1))<<16 | uint32(m.readInputs(r+2))<<8 | uint32(m.readInputs(r+3))
		case sub == 0x0C || sub == 0x0D:
			return binary.BigEndian.Uint32(m.backupRAM[a&0x1FFFF:])
		case sub == 0x10:
			r := a & 0x3F
			return uint32(m.readSysReg(r))<<24 | uint32(m.readSysReg(r+1))<<16 | uint32(m.readSysReg(r+2))<<8 | uint32(m.readSysReg(r+3))
		case sub >= 0xE0 && sub <= 0xEF:
			return m.pciConfigRead(32, 0)
		case sub == 0x14:
			return uint32(rtcRead(a>>2&0xF, m.Now()))<<24 | 0x00030000
		case sub == 0x18 || sub == 0x19:
			return binary.BigEndian.Uint32(m.securityRAM[a&0x1FFFF:])
		case sub == 0x1A:
			return m.readSecurity(a & 0x3F)
		case sub == 0x08:
			return 0xFFFFFFFF
		}
	case 0xF1:
		if a < 0xF1120000 {
			return binary.BigEndian.Uint32(m.tg.vram[a&0x1FFFFF:])
		}
		if a >= 0xF1180000 && a < 0xF1180100 {
			return bswap32(m.tg.regs[(a&0xFF)/4])
		}
	}
	return 0xFFFFFFFF
}

func (m *Machine) Write8(a uint32, v uint8) {
	if debugWatch != 0 && a <= debugWatch && a+1 > debugWatch {
		fmt.Printf("frame %d pc %08X lr %08X write8 %08X = %X\n", m.frame, m.cpu.PC, m.cpu.LR, a, v)
	}
	switch a >> 24 {
	case 0xC2:
		m.dmaWrite8(a&0xFF, v)
	case 0xC1, 0xF9:
		m.scsiWrite(a&0xFF, v)
	case 0xF0, 0xFE:
		switch a >> 16 & 0xFF {
		case 0x04:
			m.writeInputs(a&0x3F, v)
		case 0x08:
			switch a & 0xF {
			case 0:
				m.sound.writeMIDI(v)
				m.irq.deassert(irqSound)
			case 4:
				m.midiCtrl = v
				if v&0x20 == 0 {
					m.irq.deassert(irqSound)
				}
			}
		case 0x0C, 0x0D:
			m.backupRAM[a&0x1FFFF] = v
			m.nvramDirty = true
		case 0x10:
			m.writeSysReg(a&0x3F, v)
		case 0x18, 0x19:
			m.securityRAM[a&0x1FFFF] = v
		}
	case 0xF1:
		if a < 0xF1120000 {
			m.tg.write8(a&0x1FFFFF, v)
		}
	case 0xF8:
		m.pci.regs[a&0xFF] = v
	}
}

func (m *Machine) Write16(a uint32, v uint16) {
	if debugWatch != 0 && a <= debugWatch && a+2 > debugWatch {
		fmt.Printf("frame %d pc %08X lr %08X write16 %08X = %X\n", m.frame, m.cpu.PC, m.cpu.LR, a, v)
	}
	switch a >> 24 {
	case 0xF0, 0xFE:
		switch a >> 16 & 0xFF {
		case 0x0C, 0x0D:
			binary.BigEndian.PutUint16(m.backupRAM[a&0x1FFFF:], v)
			m.nvramDirty = true
		case 0x04, 0x10, 0x08:
			m.Write8(a, uint8(v>>8))
			m.Write8(a+1, uint8(v))
		}
	case 0xF1:
		if a < 0xF1120000 {
			m.tg.write16(a&0x1FFFFF, v)
		}
	case 0xF8:
		m.pci.regs[a&0xFF] = uint8(v >> 8)
		m.pci.regs[(a+1)&0xFF] = uint8(v)
	}
}

func (m *Machine) Write32(a uint32, v uint32) {
	if debugWatch != 0 && a <= debugWatch && a+4 > debugWatch {
		fmt.Printf("frame %d pc %08X lr %08X write32 %08X = %X\n", m.frame, m.cpu.PC, m.cpu.LR, a, v)
	}
	switch a >> 24 {
	case 0x88:
		m.gpu.flush()
	case 0x8C:
		m.gpu.writeCullLo(a, bswap32(v))
	case 0x8E:
		m.gpu.writeCullHi(a, bswap32(v))
	case 0x90:
		m.gpu.writeTexturePort(bswap32(v))
	case 0x94:
		m.gpu.writeFIFO(bswap32(v))
	case 0x98:
		m.gpu.writePoly(a, bswap32(v))
	case 0x9C:
		m.gpu.config[(a&0xF)/4] = bswap32(v)
	case 0xC2:
		m.dmaWrite32(a&0xFF, bswap32(v))
	case 0xC1, 0xF9:
		r := a & 0xFF
		m.scsiWrite(r, uint8(v>>24))
		m.scsiWrite(r+1, uint8(v>>16))
		m.scsiWrite(r+2, uint8(v>>8))
		m.scsiWrite(r+3, uint8(v))
	case 0xF0, 0xFE:
		sub := a >> 16 & 0xFF
		switch {
		case sub == 0x04:
			r := a & 0x3F
			m.writeInputs(r, uint8(v>>24))
			m.writeInputs(r+1, uint8(v>>16))
			m.writeInputs(r+2, uint8(v>>8))
			m.writeInputs(r+3, uint8(v))
		case sub == 0x0C || sub == 0x0D:
			binary.BigEndian.PutUint32(m.backupRAM[a&0x1FFFF:], v)
			m.nvramDirty = true
		case sub == 0x08:
		case sub >= 0xC0 && sub <= 0xEF:
			if a >= 0xFEC00000 && a < 0xFEE00000 {
				m.pci.setAddress(v)
			} else if a >= 0xFEE00000 && a < 0xFEF00000 {
				m.pciConfigWrite(32, 0, v)
			}
		case sub == 0x10:
			r := a & 0x3F
			m.writeSysReg(r, uint8(v>>24))
			m.writeSysReg(r+1, uint8(v>>16))
			m.writeSysReg(r+2, uint8(v>>8))
			m.writeSysReg(r+3, uint8(v))
		case sub == 0x18 || sub == 0x19:
			binary.BigEndian.PutUint32(m.securityRAM[a&0x1FFFF:], v)
		case sub == 0x1A:
			m.writeSecurity(a&0x3F, v)
		}
	case 0xF1:
		if a < 0xF1120000 {
			m.tg.write32(a&0x1FFFFF, v)
		} else if a >= 0xF1180000 && a < 0xF1180100 {
			reg := a & 0xFF
			w := bswap32(v)
			if reg == 0x10 {
				m.irq.deassert(w & 0xFF)
			}
			m.tg.writeReg(reg, w)
			if a == 0xF118000C {
				m.gpu.tilegenDrawFrame()
			}
		}
	case 0xF8:
		r := a & 0xFF
		m.pci.regs[r] = uint8(v >> 24)
		m.pci.regs[(r+1)&0xFF] = uint8(v >> 16)
		m.pci.regs[(r+2)&0xFF] = uint8(v >> 8)
		m.pci.regs[(r+3)&0xFF] = uint8(v)
	}
}

// ---------------------------------------------------------------- PCI configuration

func (m *Machine) pciConfigRead(bits int, off uint32) uint32 {
	p := &m.pci
	if p.device == 0 {
		r := p.reg + off
		switch bits {
		case 8:
			return uint32(p.regs[r&0xFF])
		case 16:
			return uint32(p.regs[r&0xFF])<<8 | uint32(p.regs[(r+1)&0xFF])
		}
		return uint32(p.regs[r&0xFF])<<24 | uint32(p.regs[(r+1)&0xFF])<<16 | uint32(p.regs[(r+2)&0xFF])<<8 | uint32(p.regs[(r+3)&0xFF])
	}
	reg := (p.reg >> 2) & 0x3C
	var id uint32
	switch p.device {
	case 13: // Real3D
		id = m.gpu.pciID()
	case 14: // 53C810 SCSI
		id = 0x00011000
	case 16:
		return 0
	default:
		return 0
	}
	if reg != 0 || bits == 8 {
		return 0
	}
	d := bswap32(id)
	if bits == 16 {
		return d >> ((2 - off) * 8) & 0xFFFF
	}
	return d
}

func (m *Machine) pciConfigWrite(bits int, off uint32, v uint32) {
	p := &m.pci
	if p.device == 0 {
		r := p.reg + off
		switch bits {
		case 8:
			p.regs[r&0xFF] = uint8(v)
		case 16:
			p.regs[r&0xFF] = uint8(v >> 8)
			p.regs[(r+1)&0xFF] = uint8(v)
		default:
			p.regs[r&0xFF] = uint8(v >> 24)
			p.regs[(r+1)&0xFF] = uint8(v >> 16)
			p.regs[(r+2)&0xFF] = uint8(v >> 8)
			p.regs[(r+3)&0xFF] = uint8(v)
		}
	}
}

// ---------------------------------------------------------------- system registers

func (m *Machine) readSysReg(r uint32) uint8 {
	switch r & 0x3F {
	case 0x08:
		return m.cromBankReg
	case 0x14:
		return uint8(m.irq.enable)
	case 0x18:
		return uint8(m.irq.pending)
	case 0x1C:
		return 0xFF
	case 0x10:
		if m.jtag.read() {
			return 1 << 5
		}
		return 0
	}
	return 0xFF
}

func (m *Machine) writeSysReg(r uint32, v uint8) {
	switch r & 0x3F {
	case 0x08:
		m.setCROMBank(v)
	case 0x14:
		m.irq.enable = uint32(v)
	case 0x18:
		m.irq.deassert(uint32(v))
	case 0x0C:
		m.jtag.write(v>>6&1 != 0, v>>2&1 != 0, v>>5&1 != 0, v>>7&1 != 0)
	}
}

// ---------------------------------------------------------------- Real3D DMA

func (m *Machine) dmaRead8(r uint32) uint8 {
	switch r {
	case 0xC:
		return m.gpu.dmaStatus
	case 0xE:
		return m.gpu.dmaConfig
	}
	return 0
}

func (m *Machine) dmaWrite8(r uint32, v uint8) {
	switch r {
	case 0xD:
		if v&1 != 0 {
			m.gpu.dmaStatus &^= 1
			m.irq.deassert(irqDMA)
		}
	case 0xE:
		m.gpu.dmaConfig = v
	}
}

func (m *Machine) dmaRead32(r uint32) uint32 {
	if r == 0x14 {
		return m.gpu.dmaData
	}
	return 0
}

func (m *Machine) dmaWrite32(r uint32, v uint32) {
	g := m.gpu
	switch r {
	case 0x00:
		g.dmaSrc = v
	case 0x04:
		g.dmaDst = v
	case 0x08:
		g.dmaLen = v
		m.dmaCopy()
		if g.dmaConfig&1 != 0 {
			g.dmaStatus |= 1
			m.irq.assert(irqDMA)
		}
	case 0x10:
		if v&0x20000000 != 0 {
			g.dmaData = 0x16C311DB
		} else if v&0x80000000 != 0 {
			g.dmaData = g.readReg(v & 0x3F)
		}
	case 0x14:
		g.dmaData = 0xFFFFFFFF
	}
}

func (m *Machine) dmaCopy() {
	g := m.gpu
	rev := g.dmaConfig&0x80 != 0
	for g.dmaLen != 0 {
		var d uint32
		if g.dmaSrc < uint32(len(m.ram))-3 {
			d = binary.BigEndian.Uint32(m.ram[g.dmaSrc:])
		} else {
			d = m.Read32(g.dmaSrc)
		}
		if rev {
			d = bswap32(d)
		}
		if g.dmaDst < uint32(len(m.ram))-3 {
			binary.BigEndian.PutUint32(m.ram[g.dmaDst:], d)
			m.cpu.InvalidateRAM(g.dmaDst, 4)
		} else {
			m.Write32(g.dmaDst, d)
		}
		g.dmaSrc += 4
		g.dmaDst += 4
		g.dmaLen--
	}
}

// ---------------------------------------------------------------- security board

func (m *Machine) readSecurity(r uint32) uint32 {
	switch r {
	case 0x00:
		return 0
	case 0x1C:
		if m.securityFirstRead {
			m.securityFirstRead = false
			return 0xFFFF0000
		}
		return uint32(m.crypt.decrypt()) << 16
	}
	return 0xFFFFFFFF
}

func (m *Machine) writeSecurity(r uint32, v uint32) {
	switch r {
	case 0x10, 0x14:
		m.crypt.setAddressLow(0)
		m.crypt.setAddressHigh(0)
		m.securityFirstRead = true
	case 0x18:
		k := uint16(v >> 16)
		m.crypt.setSubKey(k>>8 | k<<8)
	}
}

// ---------------------------------------------------------------- inputs

func axis(v float32) uint8 {
	x := int(0x80 + v*127)
	if x < 0 {
		x = 0
	}
	if x > 255 {
		x = 255
	}
	return uint8(x)
}

func (m *Machine) readInputs(r uint32) uint8 {
	in := &m.Inputs
	bit := func(b bool, n uint) uint8 {
		if b {
			return 1 << n
		}
		return 0
	}
	switch r & 0x3F {
	case 0x00:
		return m.inputBank
	case 0x04:
		d := uint8(0xFF)
		if m.inputBank&1 == 0 {
			d &^= bit(in.Coin1, 0) | bit(in.Coin2, 1) | bit(in.Test, 2) | bit(in.Service, 3) | bit(in.Start1, 4) | bit(in.Start2, 5)
		} else {
			d = d&0xDF | uint8(m.eeprom.read())<<5
		}
		return d
	case 0x08:
		d := uint8(0xFF)
		switch m.game.Inputs {
		case InputAnalogJoystick:
			d &^= bit(in.Trigger, 5) | bit(in.Trigger2, 4) | bit(in.Event, 0) | bit(in.Event2, 1)
		default:
			d &^= bit(in.Up, 5) | bit(in.Down, 4) | bit(in.Left, 7) | bit(in.Right, 6)
			d &^= bit(in.Buttons[0], 0) | bit(in.Buttons[1], 1) | bit(in.Buttons[2], 2) | bit(in.Buttons[3], 3)
		}
		return d
	case 0x0C:
		d := uint8(0xFF)
		if m.drive.attached() {
			d = m.drive.read()
		}
		return d
	case 0x10:
		return m.outputs[0]
	case 0x14:
		return m.outputs[1]
	case 0x18:
		return 0x7F
	case 0x2C:
		return m.serialFIFO1
	case 0x30:
		return m.serialFIFO2
	case 0x34:
		return 0
	case 0x3C:
		var adc [8]uint8
		if m.game.Inputs == InputAnalogJoystick {
			adc[0] = axis(in.StickY)
			adc[1] = axis(in.StickX)
		}
		d := adc[m.adcChannel&7]
		m.adcChannel++
		return d
	}
	return 0xFF
}

func (m *Machine) writeInputs(r uint32, v uint8) {
	switch r & 0x3F {
	case 0x00:
		m.eeprom.write(v>>6&1 != 0, v>>7&1 != 0, v>>5&1 != 0)
		m.inputBank = v
	case 0x10:
		m.drive.write(v)
		m.outputs[0] = v
	case 0x14:
		m.outputs[1] = v
	case 0x24:
		if v == 0x87 {
			m.serialFIFO1, m.serialFIFO2 = 0, 0
		}
	case 0x28:
		m.serialFIFO2 = v
	case 0x3C:
		m.adcChannel = v & 7
	}
}

var debugWatch = func() uint32 {
	var w uint32
	fmt.Sscanf(os.Getenv("M3_WATCH"), "%x", &w)
	return w
}()

// DebugTileRegs returns the tile generator's registers (for tools).
func (m *Machine) DebugTileRegs() [64]uint32 { return m.tg.regs }

// DebugMemory returns main RAM and the tile generator's memory (for tools).
func (m *Machine) DebugMemory() (ram, vram []byte) { return m.ram, m.tg.vram[:] }
