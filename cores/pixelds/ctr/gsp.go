package ctr

import "encoding/binary"

// GSP (gsp::Gpu): the graphics service. It gives the application shared
// memory with an interrupt queue, framebuffer slots and a command queue,
// writes the LCD and GPU registers for it, and runs the GX commands: DMA,
// memory fills, display transfers, texture copies and GPU command lists.

// Interrupts the GSP reports.
const (
	intPSC0 = iota
	intPSC1
	intVBlankTop
	intVBlankBottom
	intPPF
	intP3D
	intDMA
)

type lcdScreen struct {
	fb     [2]uint32 // physical addresses
	format uint32
	stride uint32
	sel    uint32
}

type gspState struct {
	shm      *memBlock
	shmH     uint32
	event    *event
	regs     map[uint32]uint32
	forceOff bool
}

type gspService struct{ m *Machine }

func newGSP(m *Machine) *gspService { return &gspService{m: m} }

func (*gspService) name() string { return "gsp::Gpu" }

func (v *gspService) st() *gspState {
	m := v.m
	if m.gsp == nil {
		m.gsp = &gspState{regs: map[uint32]uint32{}}
		m.gsp.shm, m.gsp.shmH = m.kern.serviceBlock(0x1000, "GSP shared memory")
	}
	return m.gsp
}

// shm reads and writes GSP shared memory.
func (g *gspState) at(m *Machine, off uint32) []byte {
	return m.mem.physSlice(g.shm.pages[off/pageSize]+off%pageSize, 4)
}

func (g *gspState) r32(m *Machine, off uint32) uint32 {
	return binary.LittleEndian.Uint32(g.at(m, off))
}
func (g *gspState) w32(m *Machine, off, v uint32) { binary.LittleEndian.PutUint32(g.at(m, off), v) }

func (v *gspService) handle(c *ipc) {
	g := v.st()
	m := c.m
	switch c.id() {
	case 0x0001: // WriteHWRegs(reg, size, ..., data)
		reg, size, data := c.arg(0), c.arg(1), c.arg(3)
		for i := uint32(0); i < size; i += 4 {
			m.writeReg(reg+i, m.mem.Read32(data+i))
		}
	case 0x0002: // WriteHWRegsWithMask(reg, size, ..., data, ..., mask)
		reg, size, data, mask := c.arg(0), c.arg(1), c.arg(3), c.arg(5)
		for i := uint32(0); i < size; i += 4 {
			mk := m.mem.Read32(mask + i)
			m.writeReg(reg+i, m.readReg(reg+i)&^mk|m.mem.Read32(data+i)&mk)
		}
	case 0x0003: // WriteHWRegRepeat(reg, size, ..., data)
		reg, size, data := c.arg(0), c.arg(1), c.arg(3)
		for i := uint32(0); i < size; i += 4 {
			m.writeReg(reg, m.mem.Read32(data+i))
		}
	case 0x0004: // ReadHWRegs(reg, size) -> static buffer
		reg, size := c.arg(0), c.arg(1)
		addr, _ := c.staticBuf(0)
		for i := uint32(0); i < size; i += 4 {
			m.mem.Write32(addr+i, m.readReg(reg+i))
		}
		c.trans = append(c.trans, size<<14|2, addr)
	case 0x0005: // SetBufferSwap(screen, info)
		var info [7]uint32
		for i := range info {
			info[i] = c.arg(1 + i)
		}
		m.setFramebuffer(c.arg(0), info)
	case 0x000b: // SetLcdForceBlack(on)
		g.forceOff = c.arg(0)&0xff != 0
	case 0x000c: // TriggerCmdReqQueue
		m.runGXQueue()
	case 0x0013: // RegisterInterruptRelayQueue(flags, event) -> thread index, shared memory
		if e, ok := m.kern.get(c.arg(2)).(*event); ok {
			g.event = e
		}
		c.result(0x2a07)
		c.push(0)
		c.handles(g.shmH)
	case 0x0016, 0x0015: // AcquireRight, TryAcquireRight
	case 0x0018: // ImportDisplayCaptureInfo
		for s := 0; s < 2; s++ {
			l := &m.lcd[s]
			c.push(l.fb[l.sel&1], l.fb[l.sel&1], l.format, l.stride)
		}
	}
}

// setFramebuffer takes a framebuffer description (from SetBufferSwap or
// the shared memory slots): active, left address, right address, stride,
// format, select, attribute.
func (m *Machine) setFramebuffer(screen uint32, info [7]uint32) {
	if screen > 1 {
		return
	}
	l := &m.lcd[screen]
	l.fb[0] = m.mem.virtToPhys(info[1])
	if l.fb[0] == 0 {
		l.fb[0] = info[1]
	}
	l.fb[1] = l.fb[0]
	l.stride = info[3]
	l.format = info[4]
	l.sel = 0
}

// vblank delivers the VBlank interrupts and takes framebuffer updates.
func (m *Machine) gspVBlank() {
	g := m.gsp
	if g == nil {
		return
	}
	for s := uint32(0); s < 2; s++ {
		base := 0x200 + s*0x40
		hdr := g.r32(m, base)
		if hdr>>8&0xff != 0 {
			idx := hdr & 0xff
			var info [7]uint32
			for i := range info {
				info[i] = g.r32(m, base+4+idx*0x1c+uint32(i)*4)
			}
			m.setFramebuffer(s, info)
			g.w32(m, base, hdr&^0xff00)
		}
	}
	m.gspInterrupt(intVBlankTop)
	m.gspInterrupt(intVBlankBottom)
}

// gspInterrupt puts an interrupt in the relay queue and signals the event.
func (m *Machine) gspInterrupt(id uint32) {
	g := m.gsp
	if g == nil || g.event == nil {
		return
	}
	hdr := g.r32(m, 0)
	cur, count := hdr&0xff, hdr>>8&0xff
	if count >= 0x34 {
		hdr |= 1 << 16 // missed
	} else {
		slot := (cur + count) % 0x34
		b := g.at(m, 0xc+slot&^3)
		b[slot&3] = byte(id)
		count++
		hdr = hdr&^0xff00 | count<<8
	}
	g.w32(m, 0, hdr)
	m.kern.signal(g.event)
}

// runGXQueue runs the commands waiting in the shared memory queue.
func (m *Machine) runGXQueue() {
	g := m.gsp
	if g == nil {
		return
	}
	const q = 0x800
	for {
		hdr := g.r32(m, q)
		idx, count := hdr&0xff, hdr>>8&0xff
		if count == 0 {
			return
		}
		var cmd [8]uint32
		for i := range cmd {
			cmd[i] = g.r32(m, q+0x20+idx*0x20+uint32(i)*4)
		}
		idx = (idx + 1) % 15
		count--
		g.w32(m, q, hdr&^0xffff|idx|count<<8)
		m.gxCommand(cmd)
	}
}

func (m *Machine) gxCommand(cmd [8]uint32) {
	switch cmd[0] & 0xff {
	case 0x00: // RequestDma(src, dst, size)
		src, dst, n := cmd[1], cmd[2], cmd[3]
		for i := uint32(0); i < n; i++ {
			m.mem.Write8(dst+i, m.mem.Read8(src+i))
		}
		m.gspInterrupt(intDMA)
	case 0x01: // ProcessCommandList(addr, size)
		m.gpu.runCommandList(m.mem.virtToPhys(cmd[1]), cmd[2])
		m.gspInterrupt(intP3D)
	case 0x02: // MemoryFill
		for i := 0; i < 2; i++ {
			start, val, end := cmd[1+3*i], cmd[2+3*i], cmd[3+3*i]
			ctl := cmd[7] >> (16 * uint(i)) & 0xffff
			if start == 0 {
				continue
			}
			m.memoryFill(m.mem.virtToPhys(start), m.mem.virtToPhys(end), val, ctl)
			m.gspInterrupt(uint32(intPSC0 + i))
		}
	case 0x03: // DisplayTransfer(in, out, in dims, out dims, flags)
		m.displayTransfer(m.mem.virtToPhys(cmd[1]), m.mem.virtToPhys(cmd[2]), cmd[3], cmd[4], cmd[5])
		m.gspInterrupt(intPPF)
	case 0x04: // TextureCopy(in, out, size, in dims, out dims, flags)
		m.textureCopy(m.mem.virtToPhys(cmd[1]), m.mem.virtToPhys(cmd[2]), cmd[3], cmd[4], cmd[5])
		m.gspInterrupt(intPPF)
	case 0x05: // FlushCacheRegions
	}
}

func (m *Machine) memoryFill(start, end, val, ctl uint32) {
	if end <= start {
		return
	}
	b := m.mem.physSlice(start, end-start)
	if b == nil {
		return
	}
	switch ctl >> 8 & 3 {
	case 0: // 16-bit
		for i := 0; i+1 < len(b); i += 2 {
			binary.LittleEndian.PutUint16(b[i:], uint16(val))
		}
	case 2, 3: // 32-bit
		for i := 0; i+3 < len(b); i += 4 {
			binary.LittleEndian.PutUint32(b[i:], val)
		}
	default: // 24-bit
		for i := 0; i+2 < len(b); i += 3 {
			b[i], b[i+1], b[i+2] = byte(val), byte(val>>8), byte(val>>16)
		}
	}
}

// pixel formats of framebuffers and display transfers
var fmtBytes = [8]int{4, 3, 2, 2, 2, 4, 4, 4}

// decodePixel reads a pixel as 0xRRGGBBAA.
func decodePixel(b []byte, f uint32) uint32 {
	switch f {
	case 0: // RGBA8 (bytes A B G R)
		return uint32(b[3])<<24 | uint32(b[2])<<16 | uint32(b[1])<<8 | uint32(b[0])
	case 1: // RGB8 (bytes B G R)
		return uint32(b[2])<<24 | uint32(b[1])<<16 | uint32(b[0])<<8 | 0xff
	case 2: // RGB565
		v := uint32(binary.LittleEndian.Uint16(b))
		r, g, bl := v>>11&31, v>>5&63, v&31
		return (r<<3|r>>2)<<24 | (g<<2|g>>4)<<16 | (bl<<3|bl>>2)<<8 | 0xff
	case 3: // RGB5A1
		v := uint32(binary.LittleEndian.Uint16(b))
		r, g, bl := v>>11&31, v>>6&31, v>>1&31
		a := uint32(0)
		if v&1 != 0 {
			a = 0xff
		}
		return (r<<3|r>>2)<<24 | (g<<3|g>>2)<<16 | (bl<<3|bl>>2)<<8 | a
	case 4: // RGBA4
		v := uint32(binary.LittleEndian.Uint16(b))
		return (v>>12&15)*0x11<<24 | (v>>8&15)*0x11<<16 | (v>>4&15)*0x11<<8 | (v&15)*0x11
	}
	return 0
}

func encodePixel(b []byte, f, c uint32) {
	r, g, bl, a := c>>24, c>>16&0xff, c>>8&0xff, c&0xff
	switch f {
	case 0:
		b[0], b[1], b[2], b[3] = byte(a), byte(bl), byte(g), byte(r)
	case 1:
		b[0], b[1], b[2] = byte(bl), byte(g), byte(r)
	case 2:
		binary.LittleEndian.PutUint16(b, uint16(r>>3<<11|g>>2<<5|bl>>3))
	case 3:
		binary.LittleEndian.PutUint16(b, uint16(r>>3<<11|g>>3<<6|bl>>3<<1|a>>7))
	case 4:
		binary.LittleEndian.PutUint16(b, uint16(r>>4<<12|g>>4<<8|bl>>4<<4|a>>4))
	}
}

// morton is the position of (x, y) inside an 8×8 tile.
func morton(x, y uint32) uint32 {
	return x&1 | y&1<<1 | x&2<<1 | y&2<<2 | x&4<<2 | y&4<<3
}

// tiledOffset: pixel (x, y) of an image in 8×8 tiles, w pixels wide.
func tiledOffset(x, y, w uint32) uint32 {
	return ((y>>3)*(w>>3)+x>>3)*64 + morton(x&7, y&7)
}

func (m *Machine) displayTransfer(in, out, inDim, outDim, flags uint32) {
	inW, inH := inDim&0xffff, inDim>>16
	outW, outH := outDim&0xffff, outDim>>16
	inF, outF := flags>>8&7, flags>>12&7
	if flags&8 != 0 { // raw copy without conversion: a texture copy
		return
	}
	scale := flags >> 24 & 3
	sx, sy := uint32(1), uint32(1)
	switch scale {
	case 1:
		sx = 2
	case 2:
		sx, sy = 2, 2
	}
	if inF > 4 || outF > 4 || inW == 0 || outW == 0 {
		return
	}
	ib, ob := uint32(fmtBytes[inF]), uint32(fmtBytes[outF])
	src := m.mem.physSlice(in, inW*inH*ib)
	dst := m.mem.physSlice(out, outW*outH*ob)
	if src == nil || dst == nil {
		return
	}
	toTiled := flags&2 != 0
	flip := flags&1 != 0
	for y := uint32(0); y < outH; y++ {
		for x := uint32(0); x < outW; x++ {
			var r, g, b, a uint32
			for dy := uint32(0); dy < sy; dy++ {
				for dx := uint32(0); dx < sx; dx++ {
					ix, iy := x*sx+dx, y*sy+dy
					if ix >= inW || iy >= inH {
						continue
					}
					var o uint32
					if toTiled { // linear in, tiled out
						o = (iy*inW + ix) * ib
					} else {
						o = tiledOffset(ix, iy, inW) * ib
					}
					c := decodePixel(src[o:], inF)
					r += c >> 24
					g += c >> 16 & 0xff
					b += c >> 8 & 0xff
					a += c & 0xff
				}
			}
			n := sx * sy
			c := r/n<<24 | g/n<<16 | b/n<<8 | a/n
			oy := y
			if flip {
				oy = outH - 1 - y
			}
			var o uint32
			if toTiled {
				o = tiledOffset(x, oy, outW) * ob
			} else {
				o = (oy*outW + x) * ob
			}
			encodePixel(dst[o:], outF, c)
		}
	}
}

func (m *Machine) textureCopy(in, out, size, inDim, outDim uint32) {
	inW, inGap := (inDim&0xffff)*16, (inDim>>16)*16
	outW, outGap := (outDim&0xffff)*16, (outDim>>16)*16
	if inW == 0 || outW == 0 {
		return
	}
	var si, so, done, li, lo uint32
	for done < size {
		b := m.mem.physSlice(in+si, 1)
		d := m.mem.physSlice(out+so, 1)
		if b == nil || d == nil {
			return
		}
		d[0] = b[0]
		done++
		si++
		so++
		li++
		lo++
		if li == inW {
			si += inGap
			li = 0
		}
		if lo == outW {
			so += outGap
			lo = 0
		}
	}
}

// ---------------- hardware registers ----------------

// LCD framebuffer registers (offsets from 0x1EB00000)
const (
	regTopFB0    = 0x400468
	regTopFB1    = 0x40046c
	regTopFormat = 0x400470
	regTopSelect = 0x400478
	regTopStride = 0x400490
	regBotFB0    = 0x400568
	regBotFB1    = 0x40056c
	regBotFormat = 0x400570
	regBotSelect = 0x400578
	regBotStride = 0x400590
	regGPUBase   = 0x401000
)

func (m *Machine) writeReg(reg, v uint32) {
	g := m.gsp
	g.regs[reg] = v
	top, bot := &m.lcd[0], &m.lcd[1]
	switch reg {
	case regTopFB0:
		top.fb[0] = v
	case regTopFB1:
		top.fb[1] = v
	case regTopFormat:
		top.format = v
	case regTopSelect:
		top.sel = v & 1
	case regTopStride:
		top.stride = v
	case regBotFB0:
		bot.fb[0] = v
	case regBotFB1:
		bot.fb[1] = v
	case regBotFormat:
		bot.format = v
	case regBotSelect:
		bot.sel = v & 1
	case regBotStride:
		bot.stride = v
	}
	if reg >= regGPUBase && reg < regGPUBase+0x1000 {
		m.gpu.writeReg((reg-regGPUBase)/4, v, 0xffffffff)
	}
}

func (m *Machine) readReg(reg uint32) uint32 {
	if reg >= regGPUBase && reg < regGPUBase+0x1000 {
		return m.gpu.reg[(reg-regGPUBase)/4]
	}
	return m.gsp.regs[reg]
}

// screen draws one LCD into the frame: the top screen is 400×240, the
// bottom 320×240, both kept rotated in memory (240 pixels per line).
func (m *Machine) drawScreen(s int, out []uint32, outW, ox, oy, w int) {
	l := &m.lcd[s]
	f := l.format & 7
	if f > 4 || (m.gsp != nil && m.gsp.forceOff) {
		return
	}
	bpp := uint32(fmtBytes[f])
	stride := l.stride
	if stride == 0 {
		stride = 240 * bpp
	}
	fb := l.fb[l.sel&1]
	mem := m.mem.physSlice(fb, uint32(w)*stride)
	if mem == nil {
		return
	}
	for x := 0; x < w; x++ {
		col := mem[uint32(x)*stride:]
		for y := 0; y < 240; y++ {
			o := uint32(239-y) * bpp
			c := decodePixel(col[o:], f)
			out[(oy+y)*outW+ox+x] = c >> 8
		}
	}
}
