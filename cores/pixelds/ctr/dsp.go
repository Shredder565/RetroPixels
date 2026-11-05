package ctr

import (
	"encoding/binary"
	"fmt"
)

// dsp::DSP, the sound processor's service. Games load the DSP's program,
// ask it (through the audio pipe) where its shared structures are in DSP
// memory, and then exchange an audio frame with it every 160 samples
// (about 4.9 ms), signalled through an interrupt event. This version
// answers all of that so games run; it doesn't mix sound yet.

const (
	dspRegion0 = 0x1ff50000 // the two copies of the shared structures
	dspRegion1 = 0x1ff70000
	dspFrame   = int64(cpuHz) * 160 * 1000 / 32728498 // ticks per audio frame
)

// Where the shared structures are, as DSP word addresses (0x8000 is the
// start of a region), in the order the audio pipe reports them: frame
// counter, source configurations, source statuses, ADPCM coefficients,
// DSP configuration, DSP status, final samples, intermediate mix samples,
// compressor, debug, and five more.
var dspStructs = [15]uint16{
	0x8000, 0x8010, 0x9000, 0x9200, 0x9400, 0x9500, 0x9600, 0x9800,
	0xa400, 0xa500, 0xa600, 0xaa00, 0xae00, 0xb200, 0xb600,
}

type dspService struct {
	m         *Machine
	semaphore *event
	irq       map[uint32]*event // (interrupt << 8 | channel) -> event
	pipe      []byte            // what the audio pipe has to read
	running   bool
	frame     uint16
	started   bool
}

func newDSP(m *Machine) service { return &dspService{m: m, irq: map[uint32]*event{}} }

func (*dspService) name() string { return "dsp::DSP" }

func (d *dspService) handle(c *ipc) {
	k := c.m.kern
	switch c.id() {
	case 0x0001: // RecvData(register) -> value: the DSP is ready
		c.push(1)
	case 0x0002: // RecvDataIsReady(register) -> ready
		c.push(1)
	case 0x0007, 0x0017: // SetSemaphore, SetSemaphoreMask
	case 0x000c: // ConvertProcessAddressFromDspDram(address) -> address
		c.push(c.arg(0)<<1 + 0x1ff40000)
	case 0x000d: // WriteProcessPipe(channel, size, ..., buffer)
		ch, size := c.arg(0), c.arg(1)
		buf := c.arg(3)
		if ch == 2 && size >= 2 {
			switch c.m.mem.Read16(buf) {
			case 0: // initialise: report where the structures are
				p := make([]byte, 2+2*len(dspStructs))
				binary.LittleEndian.PutUint16(p, uint16(len(dspStructs)))
				for i, a := range dspStructs {
					binary.LittleEndian.PutUint16(p[2+2*i:], a)
				}
				d.pipe = p
				d.running = true
				d.startFrames()
			case 1, 3: // shut down, sleep
				d.running = false
			case 2: // wake up
				d.running = true
			}
			d.signal(2, 2)
		}
	case 0x0010: // ReadPipeIfPossible(channel, peer, size) -> size read, static buffer
		size := int(c.arg(2) & 0xffff)
		n := 0
		addr, room := c.staticBuf(0)
		if c.arg(0) == 2 {
			n = min(size, len(d.pipe), int(room))
			c.m.mem.writeBytes(addr, d.pipe[:n])
			d.pipe = d.pipe[n:]
		}
		c.push(uint32(n))
		c.trans = append(c.trans, uint32(n)<<14|2, addr)
	case 0x0011: // LoadComponent(size, program mask, data mask, buffer) -> loaded
		c.push(1)
		c.trans = append(c.trans, c.arg(0)<<4|0xa, c.arg(4))
	case 0x0012, 0x0013, 0x0014, 0x0021: // UnloadComponent, FlushDataCache, InvalidateDataCache, ForceHeadphoneOut
	case 0x0015: // RegisterInterruptEvents(interrupt, channel, ..., event)
		key := c.arg(0)<<8 | c.arg(1)
		if e, ok := k.get(c.arg(3)).(*event); ok {
			d.irq[key] = e
		} else {
			delete(d.irq, key)
		}
	case 0x0016: // GetSemaphoreEventHandle -> event
		if d.semaphore == nil {
			d.semaphore = &event{label: "DSP semaphore"}
		}
		c.handles(k.add(d.semaphore))
	case 0x001f: // GetHeadphoneStatus -> plugged in
		c.push(0)
	default:
		key := fmt.Sprintf("dsp %04x", c.id())
		if !c.m.svcs.log[key] {
			c.m.svcs.log[key] = true
			c.m.logf("service dsp::DSP: command %04x answered with success", c.id())
		}
	}
}

func (d *dspService) signal(irq, ch uint32) {
	if e := d.irq[irq<<8|ch]; e != nil {
		d.m.kern.signal(e)
	}
}

// startFrames starts the audio frames: each one advances the frame
// counter in one of the two regions and tells the game.
func (d *dspService) startFrames() {
	if d.started {
		return
	}
	d.started = true
	d.m.kern.every(dspFrame, func() {
		if !d.running {
			return
		}
		d.frame++
		region := uint32(dspRegion0)
		if d.frame&1 != 0 {
			region = dspRegion1
		}
		pa := region - 0x1ff00000 + dspBase
		if p := d.m.mem.physSlice(pa, 2); p != nil {
			binary.LittleEndian.PutUint16(p, d.frame)
		}
		d.signal(2, 2)
	})
}
