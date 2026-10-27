package ps3

// SPU threads and thread groups (the kernel side of the SPUs): the LV2
// syscalls that make and run them, their channels (mailboxes, signal
// notification, events, the decrementer) and the MFC, which moves data
// between local store and main memory. DMA completes at once.

import (
	"fmt"
	"os"
	"strconv"
)

type spuSeg struct{ typ, ls, size, addr uint32 }

const (
	spuGInit = iota
	spuGRunning
	spuGStopped
)

type spuGroup struct {
	id       uint32
	name     string
	num      int
	prio     int32
	threads  []*spuThread
	state    int
	cause    uint32
	status   uint32
	runQueue *eventQueue
	excQueue *eventQueue
	suspend  bool
}

type spuThread struct {
	id   uint32
	name string
	g    *spuGroup
	idx  int
	c    *SPUCore

	entry uint32
	segs  []spuSeg
	args  [4]uint64

	running    bool
	exitStatus uint32
	exited     bool

	inMbox            []uint32
	spin              int  // empty channel polls in a row
	polled            bool // stopped polling (so it's idle, not busy)
	outMbox, outIntr  uint32
	outMboxN, outIntN bool
	snr               [2]uint32
	snrValid          [2]bool
	snrCfg            uint32

	evMask, evStat uint32
	decVal         uint32
	decTB          uint64
	decNeg         bool

	lsa, eah, eal, size, tag uint32
	tagMask, tagUpd          uint32
	atomicStat               uint32
	atomicValid              bool
	resAddr                  uint32
	resData                  [128]byte
	resValid                 bool
	stallStat                uint32
	stalled                  map[uint32]*spuList // tag -> list waiting for a stall ack

	spup [64]*eventQueue
	spuq map[uint32]*eventQueue
}

type spuList struct {
	put       bool
	lsa, list uint32
	eah       uint32
	left      uint32 // bytes of list left
	tag       uint32
}

type spuState struct {
	groups  []*spuGroup
	threads []*spuThread
}

type spursState struct{}

const (
	spuEvLR = 0x400
	spuEvS1 = 0x200
	spuEvS2 = 0x100
	spuEvLE = 0x80
	spuEvME = 0x40
	spuEvTM = 0x20
	spuEvMB = 0x10
	spuEvQV = 0x8
	spuEvSN = 0x2
	spuEvTG = 0x1

	spuThreadBase = 0xF0000000
	spuThreadSize = 0x100000

	spuEventUserKey = 0xFFFFFFFF53505501
	spuGroupRunKey  = 0xFFFFFFFF53505500
)

func (m *Machine) spuInit() {
	if m.spus == nil {
		m.spus = &spuState{}
	}
}

// spuIdle reports whether no SPU has work.
func (m *Machine) spuIdle() bool {
	if m.spus == nil {
		return true
	}
	for _, s := range m.spus.threads {
		if s.running && !s.g.suspend {
			return false
		}
	}
	return true
}

// runSPUs runs every running SPU for about n instructions; it returns the
// most instructions any of them ran.
func (m *Machine) runSPUs(n uint64) int {
	m.audioTick()
	if m.spus == nil {
		return 0
	}
	most := 0
	for _, s := range m.spus.threads {
		if !s.running || s.g.suspend {
			continue
		}
		left := int(n)
		s.polled = false
		for left > 0 && s.running {
			r := s.c.Run(left)
			if spuProf != nil {
				k := s.c.PC
				if s.c.stalled {
					k |= 0x80000000
				}
				spuProf[k] += r
			}
			left -= r
			if r > most && !s.polled {
				most = r
			}
			if s.c.Err != nil {
				if m.Err == nil {
					m.Err = fmt.Errorf("SPU thread %s: %w", s.name, s.c.Err)
					m.Logf("STOP: %v", m.Err)
				}
				s.running = false
				return most
			}
			if s.c.stalled || r == 0 {
				break
			}
		}
	}
	return most
}

var spuProf map[uint32]int

// PS3_SPUWATCH=<ls address>: log DMA reads that land on it (debugging).
var spuWatch = func() uint32 {
	v, _ := strconv.ParseUint(os.Getenv("PS3_SPUWATCH"), 16, 32)
	return uint32(v)
}()

func init() {
	if os.Getenv("PS3_SPUPROF") != "" {
		spuProf = map[uint32]int{}
	}
}

// ---- channels

func (s *spuThread) dec(m *Machine) uint32 {
	return s.decVal - uint32(m.tb-s.decTB)
}

func (s *spuThread) events(m *Machine) uint32 {
	if s.resValid && s.evMask&spuEvLR != 0 {
		var cur [128]byte
		m.Mem.ReadBytes(s.resAddr, cur[:])
		if cur != s.resData {
			s.resValid = false
			s.evStat |= spuEvLR
		}
	}
	if int32(s.dec(m)) < 0 {
		if !s.decNeg {
			s.decNeg = true
			s.evStat |= spuEvTM
		}
	} else {
		s.decNeg = false
	}
	if s.snrValid[0] {
		s.evStat |= spuEvS1
	}
	if s.snrValid[1] {
		s.evStat |= spuEvS2
	}
	if len(s.inMbox) > 0 {
		s.evStat |= spuEvMB
	}
	if !s.outMboxN {
		s.evStat |= spuEvLE
	}
	if !s.outIntN {
		s.evStat |= spuEvME
	}
	if s.stallStat != 0 {
		s.evStat |= spuEvSN
	}
	s.evStat |= spuEvQV
	return s.evStat & s.evMask
}

func (m *Machine) spuRdCh(s *spuThread, ch uint32) (uint32, bool) {
	switch ch {
	case 0: // SPU_RdEventStat
		e := s.events(m)
		return e, e != 0
	case 3, 4: // SPU_RdSigNotify1/2
		i := ch - 3
		if !s.snrValid[i] {
			return 0, false
		}
		s.snrValid[i] = false
		v := s.snr[i]
		s.snr[i] = 0
		s.evStat &^= spuEvS1 >> i
		return v, true
	case 8: // SPU_RdDec
		return s.dec(m), true
	case 11:
		return s.evMask, true
	case 12:
		return s.tagMask, true
	case 13: // SPU_RdMachStat
		return 0, true
	case 15:
		return s.c.SRR0, true
	case 24: // MFC_RdTagStat
		return s.tagMask, true
	case 25: // MFC_RdListStallStat
		v := s.stallStat
		s.stallStat = 0
		return v, true
	case 27: // MFC_RdAtomicStat
		if !s.atomicValid {
			return 0, false
		}
		s.atomicValid = false
		return s.atomicStat, true
	case 29: // SPU_RdInMbox
		if len(s.inMbox) == 0 {
			return 0, false
		}
		v := s.inMbox[0]
		s.inMbox = s.inMbox[1:]
		return v, true
	}
	m.Logf("SPU %s: read of channel %d", s.name, ch)
	return 0, true
}

// spuChCount answers rchcnt. A thread that keeps finding nothing on an
// input channel is polling: after spinPolls empty reads it waits (the
// instruction is retried later) so the time goes to the other threads.
func (m *Machine) spuChCount(s *spuThread, ch uint32) uint32 {
	n := m.spuChCount1(s, ch)
	switch ch {
	case 0, 3, 4, 24, 25, 27, 29:
		if n == 0 {
			s.spin++
			if s.spin >= spinPolls {
				s.spin = 0
				s.c.stalled = true
				s.polled = true
			}
		} else {
			s.spin = 0
		}
	}
	return n
}

const spinPolls = 32

func (m *Machine) spuChCount1(s *spuThread, ch uint32) uint32 {
	switch ch {
	case 0:
		if s.events(m) != 0 {
			return 1
		}
		return 0
	case 3, 4:
		if s.snrValid[ch-3] {
			return 1
		}
		return 0
	case 21: // MFC_Cmd: queue space
		return 16
	case 25:
		if s.stallStat != 0 {
			return 1
		}
		return 0
	case 27:
		if s.atomicValid {
			return 1
		}
		return 0
	case 28:
		if s.outMboxN {
			return 0
		}
		return 1
	case 29:
		return uint32(len(s.inMbox))
	case 30:
		if s.outIntN {
			return 0
		}
		return 1
	}
	return 1
}

func (m *Machine) spuWrCh(s *spuThread, ch, v uint32) bool {
	switch ch {
	case 1: // SPU_WrEventMask
		s.evMask = v
	case 2: // SPU_WrEventAck
		s.evStat &^= v
	case 7: // SPU_WrDec
		s.decVal = v
		s.decTB = m.tb
		s.decNeg = int32(v) < 0
	case 9, 69, 70, 71, 72: // sync request, bookmarks, performance monitor
	case 14:
		s.c.SRR0 = v
	case 16:
		s.lsa = v
	case 17:
		s.eah = v
	case 18:
		s.eal = v
	case 19:
		s.size = v & 0xffff
	case 20:
		s.tag = v & 31
	case 21:
		m.spuMFC(s, v&0xff)
	case 22:
		s.tagMask = v
	case 23:
		s.tagUpd = v
	case 26: // MFC_WrListStallAck
		if l, ok := s.stalled[v&31]; ok {
			delete(s.stalled, v&31)
			m.spuRunList(s, l)
		}
	case 28: // SPU_WrOutMbox
		if s.outMboxN {
			return false
		}
		s.outMbox, s.outMboxN = v, true
	case 30: // SPU_WrOutIntrMbox: the LV2 event protocol
		return m.spuIntrMbox(s, v)
	default:
		m.Logf("SPU %s: write %x to channel %d", s.name, v, ch)
	}
	return true
}

func (m *Machine) spuIntrMbox(s *spuThread, v uint32) bool {
	code := v >> 24
	if !s.outMboxN {
		m.Logf("SPU %s: event %x with an empty outbound mailbox", s.name, v)
		return true
	}
	data := s.outMbox
	switch {
	case code < 64: // sys_spu_thread_send_event
		spup := code & 63
		if len(s.inMbox) > 0 {
			s.outMboxN = false
			s.inMbox = []uint32{eBUSY}
			return true
		}
		q := s.spup[spup]
		res := uint32(eNOTCONN)
		if q != nil {
			res = q.push(event{spuEventUserKey, uint64(s.id), uint64(spup)<<32 | uint64(v&0xffffff), uint64(data)})
			if res == eBUSY {
				return false // queue full: try again
			}
		}
		s.outMboxN = false
		s.inMbox = []uint32{res}
	case code < 128: // sys_spu_thread_throw_event
		spup := code & 63
		if q := s.spup[spup]; q != nil {
			if q.push(event{spuEventUserKey, uint64(s.id), uint64(spup)<<32 | uint64(v&0xffffff), uint64(data)}) == eBUSY {
				return false
			}
		}
		s.outMboxN = false
	case code == 128 || code == 192: // sys_event_flag_set_bit (impatient)
		s.outMboxN = false
		res := uint32(0)
		if f, ok := m.objects[data].(*eventFlag); ok {
			f.bits |= 1 << (v & 63)
		} else {
			res = eSRCH
		}
		if code == 128 {
			if len(s.inMbox) > 0 {
				s.inMbox = []uint32{eBUSY}
			} else {
				s.inMbox = []uint32{res}
			}
		}
	default:
		m.Logf("SPU %s: unknown event code %x", s.name, v)
		s.outMboxN = false
	}
	return true
}

func (m *Machine) spuStop(s *spuThread, code uint32) bool {
	if code >= picStopBase && code < picStopBase+picFuncs {
		m.picCall(s, code-picStopBase)
		return true
	}
	switch code {
	case 0x100: // yield
		return true
	case 0x101: // group exit
		st := uint32(0)
		if s.outMboxN {
			st, s.outMboxN = s.outMbox, false
		}
		m.spuGroupStop(s.g, 1, st)
		return true
	case 0x102: // thread exit
		if s.outMboxN {
			s.exitStatus, s.outMboxN = s.outMbox, false
		}
		s.exited = true
		s.running = false
		s.c.stop = true
		all := true
		for _, o := range s.g.threads {
			if o != nil && o.running {
				all = false
			}
		}
		if all {
			m.spuGroupStop(s.g, 2, 0)
		}
		return true
	case 0x110, 0x111: // receive event (try)
		if !s.outMboxN {
			m.Logf("SPU %s: receive event with no queue number", s.name)
			return true
		}
		if len(s.inMbox) > 0 {
			s.outMboxN = false
			s.inMbox = []uint32{eBUSY}
			return true
		}
		q := s.spuq[s.outMbox]
		if q == nil {
			s.outMboxN = false
			s.inMbox = []uint32{eINVAL}
			return true
		}
		if _, ok := m.objects[q.id]; !ok {
			s.outMboxN = false
			s.inMbox = []uint32{eCANCELED}
			return true
		}
		if len(q.events) == 0 {
			if code == 0x111 {
				s.outMboxN = false
				s.inMbox = []uint32{eBUSY}
				return true
			}
			return false // wait
		}
		e := q.events[0]
		q.events = q.events[1:]
		s.outMboxN = false
		s.inMbox = []uint32{0, uint32(e.d1), uint32(e.d2), uint32(e.d3)}
		return true
	}
	m.Logf("SPU %s: stop %x at %05x", s.name, code, s.c.PC-4)
	m.spuGroupStop(s.g, 1, code)
	return true
}

func (m *Machine) spuGroupStop(g *spuGroup, cause, status uint32) {
	for _, s := range g.threads {
		if s != nil {
			s.running = false
			s.c.stop = true
		}
	}
	g.state = spuGStopped
	g.cause, g.status = cause, status
	m.Logf("SPU group %s stopped (cause %d, status %x)", g.name, cause, status)
}

// ---- MFC

// spuEA maps an effective address; LS of the group's other threads is at
// 0xF0000000 + 0x100000 * index.
// spuBad stops the SPU on a DMA outside memory.
func (m *Machine) spuBad(s *spuThread, ea uint32, n int) bool {
	if ea >= spuThreadBase || n == 0 {
		return false
	}
	if m.Mem.Mapped(ea) && m.Mem.Mapped(ea+uint32(n)-1) {
		return false
	}
	s.c.fail(fmt.Sprintf("DMA at %08X (%x bytes, lsa %05x, lr %05x, sp %05x)", ea, n, s.lsa, s.c.GPR[0][0], s.c.GPR[1][0]), 0)
	return true
}

func (m *Machine) spuGetMem(s *spuThread, ea uint32, b []byte) {
	if m.spuBad(s, ea, len(b)) {
		return
	}
	if ea >= spuThreadBase {
		idx := int((ea - spuThreadBase) / spuThreadSize)
		off := (ea - spuThreadBase) % spuThreadSize
		if idx < len(s.g.threads) && s.g.threads[idx] != nil && off < lsSize {
			copy(b, s.g.threads[idx].c.LS[off:])
			return
		}
	}
	m.Mem.ReadBytes(ea, b)
}

func (m *Machine) spuPutMem(s *spuThread, ea uint32, b []byte) {
	if m.spuBad(s, ea, len(b)) {
		return
	}
	if ea >= spuThreadBase {
		idx := int((ea - spuThreadBase) / spuThreadSize)
		off := (ea - spuThreadBase) % spuThreadSize
		if idx < len(s.g.threads) && s.g.threads[idx] != nil {
			o := s.g.threads[idx]
			switch {
			case off < lsSize:
				copy(o.c.LS[off:], b)
				o.c.lsWrote(off, len(b))
			case off == 0x5400C || off == 0x5C00C:
				if len(b) >= 4 {
					m.spuWriteSNR(o, map[uint32]int{0x5400C: 0, 0x5C00C: 1}[off], be32(b))
				}
			}
			return
		}
	}
	m.Mem.WriteBytes(ea, b)
}

func be32(b []byte) uint32 {
	return uint32(b[0])<<24 | uint32(b[1])<<16 | uint32(b[2])<<8 | uint32(b[3])
}

func (m *Machine) spuWriteSNR(s *spuThread, i int, v uint32) {
	if s.snrCfg>>i&1 != 0 && s.snrValid[i] {
		s.snr[i] |= v
	} else {
		s.snr[i] = v
	}
	s.snrValid[i] = true
}

// spuDMALog (PS3_SPUDMALOG): logs the first transfer to each 64 KB area
var spuDMALog = func() map[string]int {
	if os.Getenv("PS3_SPUDMALOG") != "" {
		return map[string]int{}
	}
	return nil
}()

// spuDMALSA (PS3_SPUDMALSA=hex): logs transfers to 32 KB of local store from there
var spuDMALSA = func() uint32 {
	var v uint32
	fmt.Sscanf(os.Getenv("PS3_SPUDMALSA"), "%x", &v)
	return v
}()
var spuDMALSAn int
var dbgAud = os.Getenv("PS3_AUDDBG") != ""

func (m *Machine) spuTransfer(s *spuThread, put bool, lsa, ea, size uint32) {
	if size == 0 {
		return
	}
	if size > 0x4000 {
		size = 0x4000
	}
	lsa &= lsMask
	if spuDMALSA != 0 && picCalls > 0 && lsa < spuDMALSA+0xc000 && spuDMALSA < lsa+size && spuDMALSAn < 200 {
		spuDMALSAn++
		m.Logf("SPU DMA LS %s put=%v ea %08X size %x lsa %05x pc %05x", s.name, put, ea, size, lsa, s.c.PC)
	}
	if a := m.audio; dbgAud && a != nil && a.inited {
		if ea < a.indices+64 && a.indices < ea+size {
			m.Logf("AUDDBG idx get=%v ea %08X size %x pc %05x", !put, ea, size, s.c.PC)
		}
		for i := range a.ports {
			p := &a.ports[i]
			if p.open && ea >= p.addr && ea < p.addr+p.channels*p.blocks*audioBlock*4 {
				m.Logf("AUDDBG port%d put=%v off %x size %x readidx %d", i, put, ea-p.addr, size, m.Mem.Read64(p.index))
			}
		}
	}
	if spuDMALog != nil {
		k := fmt.Sprintf("%s %v %04Xxxxx", s.name, put, ea>>16)
		if spuDMALog[k]++; spuDMALog[k] == 1 {
			m.Logf("SPU DMA %s put=%v ea %08X size %x lsa %05x pc %05x", s.name, put, ea, size, lsa, s.c.PC)
		}
	}
	buf := make([]byte, size)
	if put {
		for i := range buf {
			buf[i] = s.c.LS[(lsa+uint32(i))&lsMask]
		}
		m.spuPutMem(s, ea, buf)
	} else {
		m.spuGetMem(s, ea, buf)
		if spuWatch != 0 && lsa <= spuWatch && spuWatch < lsa+size {
			m.Logf("SPU %s: get %x bytes from %08X to %05x (pc %05x lr %05x): watched word %08x", s.name, size, ea, lsa, s.c.PC, s.c.GPR[0][0],
				uint32(buf[spuWatch-lsa])<<24|uint32(buf[spuWatch-lsa+1])<<16|uint32(buf[spuWatch-lsa+2])<<8|uint32(buf[spuWatch-lsa+3]))
		}
		for i, x := range buf {
			s.c.LS[(lsa+uint32(i))&lsMask] = x
		}
		s.c.lsWrote(lsa, int(size))
	}
}

func (m *Machine) spuMFC(s *spuThread, cmd uint32) {
	switch {
	case cmd >= 0x20 && cmd < 0x60:
		put := cmd < 0x40
		if cmd&4 != 0 { // list
			l := &spuList{put: put, lsa: s.lsa, list: s.eal & lsMask &^ 7, eah: s.eah, left: s.size, tag: s.tag}
			m.spuRunList(s, l)
			return
		}
		m.spuTransfer(s, put, s.lsa, s.eal, s.size)
	case cmd == 0xD0: // GETLLAR
		ea := s.eal &^ 127
		if m.spuBad(s, ea, 128) {
			return
		}
		m.Mem.ReadBytes(ea, s.resData[:])
		copy(s.c.LS[s.lsa&lsMask&^127:], s.resData[:])
		s.c.lsWrote(s.lsa&lsMask&^127, 128)
		s.resAddr, s.resValid = ea, true
		s.atomicStat, s.atomicValid = 4, true
	case cmd == 0xB4: // PUTLLC
		ea := s.eal &^ 127
		if m.spuBad(s, ea, 128) {
			return
		}
		var cur [128]byte
		m.Mem.ReadBytes(ea, cur[:])
		if s.resValid && s.resAddr == ea && cur == s.resData {
			m.Mem.WriteBytes(ea, s.c.LS[s.lsa&lsMask&^127:][:128])
			s.atomicStat = 0
		} else {
			s.atomicStat = 1
		}
		s.resValid = false
		s.atomicValid = true
	case cmd == 0xB0 || cmd == 0xB8: // PUTLLUC, PUTQLLUC
		ea := s.eal &^ 127
		if m.spuBad(s, ea, 128) {
			return
		}
		m.Mem.WriteBytes(ea, s.c.LS[s.lsa&lsMask&^127:][:128])
		if s.resValid && s.resAddr == ea {
			s.resValid = false
			s.evStat |= spuEvLR
		}
		if cmd == 0xB0 {
			s.atomicStat, s.atomicValid = 2, true
		}
	case cmd >= 0xA0 && cmd <= 0xA2: // SNDSIG
		v := be32(s.c.LS[s.lsa&lsMask&^3:])
		var b [4]byte
		b[0], b[1], b[2], b[3] = byte(v>>24), byte(v>>16), byte(v>>8), byte(v)
		m.spuPutMem(s, s.eal, b[:])
	case cmd == 0xC0 || cmd == 0xC8 || cmd == 0xCC: // barrier, eieio, sync
	default:
		m.Logf("SPU %s: MFC command %x", s.name, cmd)
	}
}

func (m *Machine) spuRunList(s *spuThread, l *spuList) {
	for l.left >= 8 {
		sz := be32(s.c.LS[l.list:])
		ea := be32(s.c.LS[l.list+4:])
		l.list = (l.list + 8) & lsMask
		l.left -= 8
		n := sz & 0x7fff
		m.spuTransfer(s, l.put, l.lsa, ea, n)
		if n != 0 {
			l.lsa = (l.lsa + (n+15)&^15) & lsMask
		}
		if sz&0x80000000 != 0 && l.left >= 8 {
			if s.stalled == nil {
				s.stalled = map[uint32]*spuList{}
			}
			s.stalled[l.tag] = l
			s.stallStat |= 1 << l.tag
			return
		}
	}
}

// ---- syscalls

func (m *Machine) spuThreadByID(id uint32) *spuThread {
	s, _ := m.objects[id].(*spuThread)
	return s
}

func (m *Machine) spuStart(g *spuGroup) {
	for _, s := range g.threads {
		if s == nil {
			continue
		}
		c := s.c
		for i := range c.LS {
			c.LS[i] = 0
		}
		c.lsWrote(0, lsSize)
		for _, sg := range s.segs {
			switch sg.typ {
			case 1: // copy
				n := sg.size
				if sg.ls+n > lsSize {
					n = lsSize - sg.ls&lsMask
				}
				m.Mem.ReadBytes(sg.addr, c.LS[sg.ls&lsMask:][:n])
			case 2: // fill
				for a := sg.ls; a < sg.ls+sg.size && a+4 <= lsSize; a += 4 {
					c.setLS32(a, sg.addr)
				}
			}
		}
		c.GPR = [128]Vec{}
		for i, a := range s.args {
			c.GPR[3+i] = Vec{uint32(a >> 32), uint32(a), 0, 0}
		}
		c.PC = s.entry
		c.SRR0 = 0
		c.Err = nil
		s.inMbox = nil
		s.outMboxN, s.outIntN = false, false
		s.snr, s.snrValid = [2]uint32{}, [2]bool{}
		s.evMask, s.evStat = 0, 0
		s.decVal, s.decTB, s.decNeg = 0, m.tb, false
		s.tagMask, s.atomicValid, s.resValid, s.stallStat = 0, false, false, 0
		s.stalled = nil
		s.exited = false
		s.running = true
	}
	g.state = spuGRunning
	g.cause, g.status = 0, 0
	if g.runQueue != nil {
		g.runQueue.push(event{spuGroupRunKey, uint64(g.id), 0, 0})
	}
}

func (m *Machine) newSPUThread(g *spuGroup, idx int, name string) *spuThread {
	s := &spuThread{g: g, idx: idx, name: name, c: newSPUCore(), spuq: map[uint32]*eventQueue{}}
	s.id = m.newObj(s)
	c := s.c
	c.RdCh = func(_ *SPUCore, ch uint32) (uint32, bool) { return m.spuRdCh(s, ch) }
	c.WrCh = func(_ *SPUCore, ch, v uint32) bool { return m.spuWrCh(s, ch, v) }
	c.ChCount = func(_ *SPUCore, ch uint32) uint32 { return m.spuChCount(s, ch) }
	c.Stop = func(_ *SPUCore, code uint32) bool { return m.spuStop(s, code) }
	c.Halt = func(_ *SPUCore) {
		m.Logf("SPU %s: halt at %05x", s.name, s.c.PC-4)
		m.spuGroupStop(s.g, 1, 0)
	}
	m.spus.threads = append(m.spus.threads, s)
	return s
}

func init() {
	sysc(169, func(m *Machine, t *Thread) { // sys_spu_initialize
		m.spuInit()
		t.ret(0)
	})
	sysc(170, func(m *Machine, t *Thread) { // sys_spu_thread_group_create(id*, num, prio, attr*)
		m.spuInit()
		attr := t.arg(3)
		num := int(t.arg(1))
		if num < 1 || num > 6 {
			t.ret(eINVAL)
			return
		}
		g := &spuGroup{num: num, prio: t.argS(2), threads: make([]*spuThread, num)}
		if attr != 0 {
			if np := m.Mem.Read32(attr + 4); np != 0 {
				g.name = m.Mem.String(np, int(m.Mem.Read32(attr)))
			}
		}
		g.id = m.newObj(g)
		m.spus.groups = append(m.spus.groups, g)
		m.Mem.Write32(t.arg(0), g.id)
		m.Logf("SPU group %s (%x): %d threads", g.name, g.id, num)
		t.ret(0)
	})
	sysc(171, func(m *Machine, t *Thread) { // destroy
		g, ok := m.objects[t.arg(0)].(*spuGroup)
		if !ok {
			t.ret(eSRCH)
			return
		}
		if g.state == spuGRunning {
			t.ret(eBUSY)
			return
		}
		for _, s := range g.threads {
			if s != nil {
				delete(m.objects, s.id)
				for i, o := range m.spus.threads {
					if o == s {
						m.spus.threads = append(m.spus.threads[:i], m.spus.threads[i+1:]...)
						break
					}
				}
			}
		}
		delete(m.objects, g.id)
		t.ret(0)
	})
	sysc(172, func(m *Machine, t *Thread) { // sys_spu_thread_initialize(thread*, group, spu_num, img*, attr*, arg*)
		g, ok := m.objects[t.arg(1)].(*spuGroup)
		if !ok {
			t.ret(eSRCH)
			return
		}
		idx := int(t.arg(2))
		if idx >= g.num {
			t.ret(eINVAL)
			return
		}
		img, attr, arg := t.arg(3), t.arg(4), t.arg(5)
		name := ""
		if attr != 0 {
			if np := m.Mem.Read32(attr); np != 0 {
				name = m.Mem.String(np, int(m.Mem.Read32(attr+4)))
			}
		}
		if name == "" {
			name = fmt.Sprintf("%s/%d", g.name, idx)
		}
		s := m.newSPUThread(g, idx, name)
		s.entry = m.Mem.Read32(img + 4)
		segs, n := m.Mem.Read32(img+8), int(m.Mem.Read32(img+12))
		for i := 0; i < n; i++ {
			a := segs + uint32(i)*24
			s.segs = append(s.segs, spuSeg{m.Mem.Read32(a), m.Mem.Read32(a + 4), m.Mem.Read32(a + 8), m.Mem.Read32(a + 16)})
		}
		if arg != 0 {
			for i := range s.args {
				s.args[i] = m.Mem.Read64(arg + uint32(i)*8)
			}
		}
		g.threads[idx] = s
		m.Mem.Write32(t.arg(0), s.id)
		m.Logf("SPU thread %s (%x): entry %05x, %d segments", name, s.id, s.entry, n)
		t.ret(0)
	})
	sysc(166, func(m *Machine, t *Thread) { // sys_spu_thread_set_argument(id, arg*)
		s := m.spuThreadByID(t.arg(0))
		if s == nil {
			t.ret(eSRCH)
			return
		}
		for i := range s.args {
			s.args[i] = m.Mem.Read64(t.arg(1) + uint32(i)*8)
		}
		t.ret(0)
	})
	sysc(165, func(m *Machine, t *Thread) { // sys_spu_thread_get_exit_status(id, status*)
		s := m.spuThreadByID(t.arg(0))
		if s == nil {
			t.ret(eSRCH)
			return
		}
		if !s.exited {
			t.ret(eSTAT)
			return
		}
		m.Mem.Write32(t.arg(1), s.exitStatus)
		t.ret(0)
	})
	sysc(173, func(m *Machine, t *Thread) { // start
		g, ok := m.objects[t.arg(0)].(*spuGroup)
		if !ok {
			t.ret(eSRCH)
			return
		}
		if g.state == spuGRunning {
			t.ret(eSTAT)
			return
		}
		m.spuStart(g)
		m.Logf("SPU group %s started", g.name)
		t.ret(0)
	})
	sysc(174, func(m *Machine, t *Thread) { // suspend
		if g, ok := m.objects[t.arg(0)].(*spuGroup); ok {
			g.suspend = true
			t.ret(0)
			return
		}
		t.ret(eSRCH)
	})
	sysc(175, func(m *Machine, t *Thread) { // resume
		if g, ok := m.objects[t.arg(0)].(*spuGroup); ok {
			g.suspend = false
			t.ret(0)
			return
		}
		t.ret(eSRCH)
	})
	sysc(176, func(m *Machine, t *Thread) { t.ret(0) }) // yield
	sysc(177, func(m *Machine, t *Thread) {             // terminate(id, value)
		g, ok := m.objects[t.arg(0)].(*spuGroup)
		if !ok {
			t.ret(eSRCH)
			return
		}
		if g.state != spuGRunning {
			t.ret(eSTAT)
			return
		}
		m.spuGroupStop(g, 4, t.arg(1))
		t.ret(0)
	})
	sysc(178, func(m *Machine, t *Thread) { // join(id, cause*, status*)
		g, ok := m.objects[t.arg(0)].(*spuGroup)
		if !ok {
			t.ret(eSRCH)
			return
		}
		if g.state == spuGInit {
			t.ret(eSTAT)
			return
		}
		causeP, statusP := t.arg(1), t.arg(2)
		t.ret(0)
		m.block(t, "SPU group "+g.name, func() bool {
			if g.state != spuGStopped {
				return false
			}
			if causeP != 0 {
				m.Mem.Write32(causeP, g.cause)
			}
			if statusP != 0 {
				m.Mem.Write32(statusP, g.status)
			}
			g.state = spuGInit
			return true
		})
	})
	sysc(179, func(m *Machine, t *Thread) { // set_priority
		if g, ok := m.objects[t.arg(0)].(*spuGroup); ok {
			g.prio = t.argS(1)
		}
		t.ret(0)
	})
	sysc(180, func(m *Machine, t *Thread) { // get_priority
		if g, ok := m.objects[t.arg(0)].(*spuGroup); ok {
			m.Mem.Write32(t.arg(1), uint32(g.prio))
			t.ret(0)
			return
		}
		t.ret(eSRCH)
	})
	sysc(181, func(m *Machine, t *Thread) { // write_ls(id, lsa, value, type)
		s := m.spuThreadByID(t.arg(0))
		if s == nil {
			t.ret(eSRCH)
			return
		}
		a, v := t.arg(1)&lsMask, t.arg64(2)
		switch t.arg(3) {
		case 1:
			s.c.LS[a] = byte(v)
			s.c.lsWrote(a, 1)
		case 2:
			s.c.LS[a], s.c.LS[a+1] = byte(v>>8), byte(v)
			s.c.lsWrote(a, 2)
		case 4:
			s.c.setLS32(a, uint32(v))
		case 8:
			s.c.setLS32(a, uint32(v>>32))
			s.c.setLS32(a+4, uint32(v))
		default:
			t.ret(eINVAL)
			return
		}
		t.ret(0)
	})
	sysc(182, func(m *Machine, t *Thread) { // read_ls(id, lsa, value*, type)
		s := m.spuThreadByID(t.arg(0))
		if s == nil {
			t.ret(eSRCH)
			return
		}
		a := t.arg(1) & lsMask
		var v uint64
		switch t.arg(3) {
		case 1:
			v = uint64(s.c.LS[a])
		case 2:
			v = uint64(s.c.LS[a])<<8 | uint64(s.c.LS[a+1])
		case 4:
			v = uint64(s.c.ls32(a))
		case 8:
			v = uint64(s.c.ls32(a))<<32 | uint64(s.c.ls32(a+4))
		default:
			t.ret(eINVAL)
			return
		}
		m.Mem.Write64(t.arg(2), v)
		t.ret(0)
	})
	sysc(184, func(m *Machine, t *Thread) { // write_snr(id, number, value)
		s := m.spuThreadByID(t.arg(0))
		if s == nil {
			t.ret(eSRCH)
			return
		}
		if t.arg(1) > 1 {
			t.ret(eINVAL)
			return
		}
		m.spuWriteSNR(s, int(t.arg(1)), t.arg(2))
		t.ret(0)
	})
	sysc(185, func(m *Machine, t *Thread) { // group_connect_event(id, eq, et)
		g, ok := m.objects[t.arg(0)].(*spuGroup)
		q, ok2 := m.objects[t.arg(1)].(*eventQueue)
		if !ok || !ok2 {
			t.ret(eSRCH)
			return
		}
		switch t.arg(2) {
		case 1:
			g.runQueue = q
		case 2:
			g.excQueue = q
		}
		t.ret(0)
	})
	sysc(186, func(m *Machine, t *Thread) { // group_disconnect_event(id, et)
		if g, ok := m.objects[t.arg(0)].(*spuGroup); ok {
			switch t.arg(1) {
			case 1:
				g.runQueue = nil
			case 2:
				g.excQueue = nil
			}
		}
		t.ret(0)
	})
	sysc(187, func(m *Machine, t *Thread) { // set_spu_cfg(id, value)
		if s := m.spuThreadByID(t.arg(0)); s != nil {
			s.snrCfg = uint32(t.arg64(1)) & 3
			t.ret(0)
			return
		}
		t.ret(eSRCH)
	})
	sysc(188, func(m *Machine, t *Thread) { // get_spu_cfg(id, value*)
		if s := m.spuThreadByID(t.arg(0)); s != nil {
			m.Mem.Write64(t.arg(1), uint64(s.snrCfg))
			t.ret(0)
			return
		}
		t.ret(eSRCH)
	})
	sysc(190, func(m *Machine, t *Thread) { // write_spu_mb(id, value)
		s := m.spuThreadByID(t.arg(0))
		if s == nil {
			t.ret(eSRCH)
			return
		}
		s.inMbox = append(s.inMbox, t.arg(1))
		if len(s.inMbox) > 4 {
			s.inMbox = s.inMbox[len(s.inMbox)-4:]
		}
		t.ret(0)
	})
	sysc(191, func(m *Machine, t *Thread) { // connect_event(id, eq, et, spup)
		s := m.spuThreadByID(t.arg(0))
		q, ok := m.objects[t.arg(1)].(*eventQueue)
		if s == nil || !ok {
			t.ret(eSRCH)
			return
		}
		spup := t.arg(3)
		if t.arg(2) != 1 || spup > 63 {
			m.Logf("SPU %s: connect_event type %d port %d", s.name, t.arg(2), spup)
			t.ret(eINVAL)
			return
		}
		if s.spup[spup] != nil {
			t.ret(eISCONN)
			return
		}
		s.spup[spup] = q
		t.ret(0)
	})
	sysc(192, func(m *Machine, t *Thread) { // disconnect_event(id, et, spup)
		s := m.spuThreadByID(t.arg(0))
		if s == nil {
			t.ret(eSRCH)
			return
		}
		spup := t.arg(2)
		if spup > 63 {
			t.ret(eINVAL)
			return
		}
		if s.spup[spup] == nil {
			t.ret(eNOTCONN)
			return
		}
		s.spup[spup] = nil
		t.ret(0)
	})
	sysc(193, func(m *Machine, t *Thread) { // bind_queue(id, spuq, spuq_num)
		s := m.spuThreadByID(t.arg(0))
		q, ok := m.objects[t.arg(1)].(*eventQueue)
		if s == nil || !ok {
			t.ret(eSRCH)
			return
		}
		if _, dup := s.spuq[t.arg(2)]; dup {
			t.ret(eBUSY)
			return
		}
		s.spuq[t.arg(2)] = q
		t.ret(0)
	})
	sysc(194, func(m *Machine, t *Thread) { // unbind_queue(id, spuq_num)
		s := m.spuThreadByID(t.arg(0))
		if s == nil {
			t.ret(eSRCH)
			return
		}
		if _, ok := s.spuq[t.arg(1)]; !ok {
			t.ret(eSRCH)
			return
		}
		delete(s.spuq, t.arg(1))
		t.ret(0)
	})
	sysc(251, func(m *Machine, t *Thread) { // group_connect_event_all_threads(id, eq, req, spup*)
		g, ok := m.objects[t.arg(0)].(*spuGroup)
		q, ok2 := m.objects[t.arg(1)].(*eventQueue)
		if !ok || !ok2 {
			t.ret(eSRCH)
			return
		}
		req := t.arg64(2)
		for p := uint32(0); p < 64; p++ {
			if req>>p&1 == 0 {
				continue
			}
			free := true
			for _, s := range g.threads {
				if s != nil && s.spup[p] != nil {
					free = false
				}
			}
			if !free {
				continue
			}
			for _, s := range g.threads {
				if s != nil {
					s.spup[p] = q
				}
			}
			m.Mem.Write8(t.arg(3), uint8(p))
			t.ret(0)
			return
		}
		t.ret(eISCONN)
	})
	sysc(252, func(m *Machine, t *Thread) { // group_disconnect_event_all_threads(id, spup)
		g, ok := m.objects[t.arg(0)].(*spuGroup)
		if !ok {
			t.ret(eSRCH)
			return
		}
		p := t.arg(1) & 63
		for _, s := range g.threads {
			if s != nil {
				s.spup[p] = nil
			}
		}
		t.ret(0)
	})

	// sysPrxForUser: SPU images from ELFs in memory
	reg("sys_spu_image_import", func(m *Machine, t *Thread) { // (img*, src, type)
		img, src := t.arg(0), t.arg(1)
		if m.Mem.Read32(src) != 0x7f454c46 {
			t.ret(eNOEXEC)
			return
		}
		entry := m.Mem.Read32(src + 0x18)
		phoff := m.Mem.Read32(src + 0x1c)
		phnum := uint32(m.Mem.Read16(src + 0x2c))
		var segs []spuSeg
		for i := uint32(0); i < phnum; i++ {
			p := src + phoff + i*32
			typ, off, va := m.Mem.Read32(p), m.Mem.Read32(p+4), m.Mem.Read32(p+8)
			fs, ms := m.Mem.Read32(p+16), m.Mem.Read32(p+20)
			switch typ {
			case 1:
				if fs > 0 {
					segs = append(segs, spuSeg{1, va, fs, src + off})
				}
				if ms > fs {
					segs = append(segs, spuSeg{2, va + fs, ms - fs, 0})
				}
			case 4:
				segs = append(segs, spuSeg{4, 0, fs, src + off})
			}
		}
		arr := m.sysAlloc(uint32(len(segs))*24, 16)
		for i, sg := range segs {
			a := arr + uint32(i)*24
			m.Mem.Write32(a, sg.typ)
			m.Mem.Write32(a+4, sg.ls)
			m.Mem.Write32(a+8, sg.size)
			m.Mem.Write32(a+12, 0)
			m.Mem.Write32(a+16, sg.addr)
			m.Mem.Write32(a+20, 0)
		}
		m.Mem.Write32(img, 0)
		m.Mem.Write32(img+4, entry)
		m.Mem.Write32(img+8, arr)
		m.Mem.Write32(img+12, uint32(len(segs)))
		t.ret(0)
	})
	reg("sys_spu_image_close", func(m *Machine, t *Thread) { t.ret(0) })
}
