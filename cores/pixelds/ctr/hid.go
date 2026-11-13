package ctr

import "encoding/binary"

// HID: buttons, the circle pad and the touch screen, written each frame
// into shared memory the way the 3DS's HID module does, with its events.

// 3DS button bits
const (
	KeyA      = 1 << 0
	KeyB      = 1 << 1
	KeySelect = 1 << 2
	KeyStart  = 1 << 3
	KeyRight  = 1 << 4
	KeyLeft   = 1 << 5
	KeyUp     = 1 << 6
	KeyDown   = 1 << 7
	KeyR      = 1 << 8
	KeyL      = 1 << 9
	KeyX      = 1 << 10
	KeyY      = 1 << 11
	KeyZL     = 1 << 14
	KeyZR     = 1 << 15
	keyTouch  = 1 << 20
	keyCRight = 1 << 28
	keyCLeft  = 1 << 29
	keyCUp    = 1 << 30
	keyCDown  = 1 << 31
)

type hidState struct {
	shm    *memBlock
	shmH   uint32
	events [5]*event
	idx    uint32
	tidx   uint32
	prev   uint32
}

type hidService struct {
	n string
	m *Machine
}

func newHID(m *Machine, name string) *hidService { return &hidService{n: name, m: m} }

func (v *hidService) name() string { return v.n }

func (v *hidService) st() *hidState {
	m := v.m
	if m.hid == nil {
		h := &hidState{}
		h.shm, h.shmH = m.kern.serviceBlock(0x2b0, "HID shared memory")
		for i := range h.events {
			h.events[i] = &event{label: "HID"}
		}
		m.hid = h
	}
	return m.hid
}

func (v *hidService) handle(c *ipc) {
	h := v.st()
	k := c.m.kern
	switch c.id() {
	case 0x000a: // GetIPCHandles -> memory block, 5 events
		hs := []uint32{h.shmH}
		for _, e := range h.events {
			hs = append(hs, k.add(e))
		}
		c.handles(hs...)
	case 0x0015: // GetGyroscopeLowRawToDpsCoefficient -> float
		c.push(0x3f800000)
	case 0x0016: // GetGyroscopeLowCalibrateParam -> 0x14 bytes
		c.push(0, 0, 0, 0, 0)
	case 0x0017: // GetSoundVolume -> 0-63
		c.push(63)
	}
}

func (h *hidState) put(m *Machine, off uint32, v uint32) {
	binary.LittleEndian.PutUint32(m.mem.physSlice(h.shm.pages[0]+off, 4), v)
}

func (h *hidState) get(m *Machine, off uint32) uint32 {
	return binary.LittleEndian.Uint32(m.mem.physSlice(h.shm.pages[0]+off, 4))
}

// hidUpdate writes this frame's input and signals the pad event.
func (m *Machine) hidUpdate() {
	h := m.hid
	if h == nil {
		return
	}
	tick := uint64(m.cpu.Cycles)
	keys := m.keys
	cx, cy := m.circle[0], m.circle[1]
	switch {
	case cx > 40:
		keys |= keyCRight
	case cx < -40:
		keys |= keyCLeft
	}
	switch {
	case cy > 40:
		keys |= keyCUp
	case cy < -40:
		keys |= keyCDown
	}
	if m.touchOn {
		keys |= keyTouch
	}
	// pad
	put64 := func(off uint32, v uint64) { h.put(m, off, uint32(v)); h.put(m, off+4, uint32(v>>32)) }
	prevTick := uint64(h.get(m, 0)) | uint64(h.get(m, 4))<<32
	put64(8, prevTick)
	put64(0, tick)
	h.idx = (h.idx + 1) & 7
	h.put(m, 0x10, h.idx)
	e := 0x28 + h.idx*0x10
	h.put(m, e, keys)
	h.put(m, e+4, keys&^h.prev)
	h.put(m, e+8, h.prev&^keys)
	h.put(m, e+12, uint32(uint16(int16(cx)))|uint32(uint16(int16(cy)))<<16)
	h.put(m, 0x18, uint32(uint16(int16(cx)))|uint32(uint16(int16(cy)))<<16)
	h.prev = keys
	// touch
	prevT := uint64(h.get(m, 0xa8)) | uint64(h.get(m, 0xac))<<32
	put64(0xb0, prevT)
	put64(0xa8, tick)
	h.tidx = (h.tidx + 1) & 7
	h.put(m, 0xb8, h.tidx)
	te := 0xc8 + h.tidx*8
	if m.touchOn {
		h.put(m, te, uint32(m.touchX)|uint32(m.touchY)<<16)
		h.put(m, te+4, 1)
	} else {
		h.put(m, te, 0)
		h.put(m, te+4, 0)
	}
	// accelerometer and gyroscope: at rest
	put64(0x108, tick)
	put64(0x158, tick)
	m.kern.signal(h.events[0])
	m.kern.signal(h.events[1])
}
