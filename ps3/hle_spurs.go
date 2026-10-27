package ps3

// SPURS tasks and queues, enough for the PhysX of the game: it makes SPURS
// tasks (small SPU programs) for each physics step, and they hand their
// results back through SPURS queues that the PPU pops.
//
// The tasks run on our own little SPURS kernel: a pool of SPU threads
// (one per SPU given to cellSpursInitialize) takes tasks in the order
// they were made, loads each one's ELF and runs it to its exit. A task
// reaches the kernel through a table whose address is in its local store
// at 0x2FB0+8; the table's +196 entry is the syscall entry (exit, yield,
// wait signal...), +212 holds the task's id. Tasks made without a context
// save area can't block, which the task library sees from the +152 entry
// being 0.
//
// A queue (128 bytes, made by _cellSpursQueueInitialize) is laid out as
// the SPU side of the library expects it:
//
//	0x00 pop count, 0x04 push count (each counts modulo 2*depth; a
//	     negative value, its complement, marks an operation under way)
//	0x08 entry size  0x0C depth  0x10 buffer (u64)  0x18 event port
//	0x1C direction (0 SPU to SPU, 1 SPU to PPU, 2 PPU to SPU)
//	0x20/0x40 tasks waiting to pop, 0x30/0x50 tasks waiting to push
//	0x60 taskset (u64)

import (
	"bytes"
	"fmt"
	"os"
	"strings"
)

const (
	spursSyscallStop = 0x3E00 // stop code of the kernel's syscall entry
	spursKernelLS    = 0x2C00 // the syscall entry, then the table
	spursTableLS     = 0x2D00

	spursErrAgain = 0x80410901
	spursErrInval = 0x80410902
	spursErrSrch  = 0x80410905
	spursErrAlign = 0x80410910
	spursErrNull  = 0x80410911
)

var spursLog = os.Getenv("PS3_SPURSLOG") != ""

type spursTask struct {
	taskset, id uint32
	elf, entry  uint32
	segs        []spuSeg
	arg         [4]uint32
	trace       bool // log its transfers (PS3_SPURSTRACE=<ELF address>)
}

type spursState struct {
	g       *spuGroup
	running map[*spuThread]*spursTask
	ready   []*spursTask
	ids     map[uint32]*[128]bool // taskset -> task ids in use
	args    map[uint32]uint64     // taskset -> its argument (each task's r4)
	made    int
	last    map[*spuThread]uint32 // the ELF each SPU ran last
	img     []byte
}

func (m *Machine) spursInit(n int) *spursState {
	if m.spurs != nil {
		return m.spurs
	}
	m.spuInit()
	if n < 1 || n > 6 {
		n = 5
	}
	g := &spuGroup{name: "SPURS", num: n, threads: make([]*spuThread, n), state: spuGRunning, spurs: true}
	g.id = m.newObj(g)
	for i := range g.threads {
		g.threads[i] = m.newSPUThread(g, i, "SPURS"+string(rune('0'+i)))
	}
	m.spurs = &spursState{g: g, running: map[*spuThread]*spursTask{}, ids: map[uint32]*[128]bool{}, args: map[uint32]uint64{}, last: map[*spuThread]uint32{}, img: make([]byte, lsSize)}
	return m.spurs
}

// spursRun starts tasks on SPUs that have none, each on one that ran the
// same program last if it can.
func (m *Machine) spursRun() {
	sp := m.spurs
	for len(sp.ready) > 0 {
		var idle *spuThread
		for _, s := range sp.g.threads {
			if sp.running[s] == nil {
				if idle == nil || sp.last[s] == sp.ready[0].elf {
					idle = s
				}
			}
		}
		if idle == nil {
			return
		}
		tk := sp.ready[0]
		sp.ready = sp.ready[1:]
		m.spursLoad(idle, tk)
	}
}

func (m *Machine) spursLoad(s *spuThread, tk *spursTask) {
	m.spurs.running[s] = tk
	s.entry, s.segs, s.args = tk.entry, tk.segs, [4]uint64{}
	c := s.c
	// load the task, telling the recompiler only about the parts of local
	// store that changed: the same programs keep coming back, so their
	// code stays compiled
	img := m.spurs.img
	m.spuImage(tk.segs, img)
	const chunk = 512
	for a := 0; a < lsSize; a += chunk {
		if !bytes.Equal(c.LS[a:a+chunk], img[a:a+chunk]) {
			copy(c.LS[a:a+chunk], img[a:a+chunk])
			c.lsWrote(uint32(a), chunk)
		}
	}
	m.spuResetState(s)
	m.spurs.last[s] = tk.elf
	c.setLS32(spursKernelLS, spursSyscallStop) // stop
	c.setLS32(spursKernelLS+4, 0x35000000)     // bi $0
	c.setLS32(spursTableLS+196, spursKernelLS)
	c.setLS32(spursTableLS+212, tk.id)
	c.setLS32(0x2FB8, spursTableLS)
	c.GPR[3] = Vec{tk.arg[0], tk.arg[1], tk.arg[2], tk.arg[3]}
	a := m.spurs.args[tk.taskset]
	c.GPR[4] = Vec{uint32(a >> 32), uint32(a), 0, 0}
	c.stop = true
}

// spursStop handles the kernel's syscalls and a task's own exit.
func (m *Machine) spursStop(s *spuThread, code uint32) bool {
	sp := m.spurs
	tk := sp.running[s]
	n := s.c.GPR[3][0]
	if code == spursSyscallStop && n != 0 {
		// 1 yield, 2 wait signal, 3 poll, 4 receive workload flag: none
		// has anything to wait for here
		s.c.GPR[3] = Vec{}
		return true
	}
	if spursLog && tk != nil {
		m.Logf("SPURS task %d (ELF entry %x) ended: stop %x r3 %x, %d instructions", tk.id, tk.entry, code, n, s.c.Ran)
	}
	if code != spursSyscallStop || n != 0 {
		m.Logf("SPURS task stopped: stop %x, r3 %x, at %05x", code, n, s.c.PC-4)
	}
	if tk != nil {
		if ids := sp.ids[tk.taskset]; ids != nil {
			ids[tk.id] = false
		}
	}
	delete(sp.running, s)
	s.running = false
	s.c.stop = true
	m.spursRun()
	return true
}

func (m *Machine) spursQueuePop(q, buf uint32, peek bool) bool {
	a, b := m.Mem.Read32(q), m.Mem.Read32(q+4)
	if int32(a) < 0 {
		return false // an SPU is handing it over
	}
	if int32(b) < 0 {
		b = ^b // a push under way: its entry isn't in yet
	}
	size, depth, base := m.Mem.Read32(q+8), m.Mem.Read32(q+12), m.Mem.Read32(q+0x14)
	count := b - a
	if a > b {
		count += 2 * depth
	}
	if count == 0 || depth == 0 {
		return false
	}
	slot := a
	if slot >= depth {
		slot -= depth
	}
	if spursLog {
		m.Logf("SPURS pop %08X: %08X %08X %08X %08X", q, m.Mem.Read32(base+size*slot), m.Mem.Read32(base+size*slot+4), m.Mem.Read32(base+size*slot+8), m.Mem.Read32(base+size*slot+12))
	}
	if buf != 0 {
		e := make([]byte, size)
		m.Mem.ReadBytes(base+size*slot, e)
		m.Mem.WriteBytes(buf, e)
	}
	if !peek {
		a++
		if a >= 2*depth {
			a = 0
		}
		m.Mem.Write32(q, a)
	}
	return true
}

func init() {
	reg("cellSpursInitialize", func(m *Machine, t *Thread) { // (spurs, nSpu, prio, ppuPrio, exitIfNoWork)
		m.spursInit(int(t.argS(1)))
		t.ret(0)
	})
	// (attr, revision, sdkVersion, argTaskset, priority*, maxContention):
	// the attribute is only read by cellSpursCreateTasksetWithAttribute,
	// so it keeps the argument where that looks for it
	reg("_cellSpursTasksetAttributeInitialize", func(m *Machine, t *Thread) {
		attr := t.arg(0)
		if attr == 0 {
			t.ret(spursErrNull)
			return
		}
		m.Mem.Write64(attr, t.arg64(3))
		t.ret(0)
	})
	reg("cellSpursTasksetAttributeSetName", func(m *Machine, t *Thread) { t.ret(0) })
	// (spurs, taskset, attr)
	reg("cellSpursCreateTasksetWithAttribute", func(m *Machine, t *Thread) {
		ts, attr := t.arg(1), t.arg(2)
		if ts == 0 || attr == 0 {
			t.ret(spursErrNull)
			return
		}
		m.spursInit(5).args[ts] = m.Mem.Read64(attr)
		t.ret(0)
	})
	// (spurs, taskset, argTaskset, priority*, maxContention)
	reg("cellSpursCreateTaskset", func(m *Machine, t *Thread) {
		if t.arg(1) == 0 {
			t.ret(spursErrNull)
			return
		}
		m.spursInit(5).args[t.arg(1)] = t.arg64(2)
		t.ret(0)
	})
	// (spurs, queue, port*, isDynamic): events the tasks send on the port
	// go to the queue
	reg("cellSpursAttachLv2EventQueue", func(m *Machine, t *Thread) {
		q, ok := m.objects[t.arg(1)].(*eventQueue)
		portp := t.arg(2)
		if !ok {
			t.ret(spursErrSrch)
			return
		}
		if portp == 0 {
			t.ret(spursErrNull)
			return
		}
		sp := m.spursInit(5)
		s0 := sp.g.threads[0]
		port := uint32(m.Mem.Read8(portp))
		if t.arg(3) != 0 {
			port = 64
			for p := uint32(0x10); p < 64; p++ {
				if s0.spup[p] == nil {
					port = p
					break
				}
			}
		}
		if port >= 64 || s0.spup[port] != nil {
			t.ret(spursErrInval)
			return
		}
		for _, s := range sp.g.threads {
			s.spup[port] = q
		}
		m.Mem.Write8(portp, uint8(port))
		m.Logf("SPURS port %d: event queue %x", port, t.arg(1))
		t.ret(0)
	})
	// (taskset, id*, elf, context, size, lsPattern*, arg*)
	reg("cellSpursCreateTask", func(m *Machine, t *Thread) {
		ts, idp, elf, argp := t.arg(0), t.arg(1), t.arg(2), t.arg(6)
		if ts == 0 || elf == 0 {
			t.ret(spursErrNull)
			return
		}
		if m.Mem.Read32(elf) != 0x7f454c46 {
			t.ret(spursErrInval)
			return
		}
		sp := m.spursInit(5)
		ids := sp.ids[ts]
		if ids == nil {
			ids = new([128]bool)
			sp.ids[ts] = ids
		}
		id := -1
		for i, used := range ids {
			if !used {
				id = i
				break
			}
		}
		if id < 0 {
			t.ret(0x80410904) // no space
			return
		}
		ids[id] = true
		tk := &spursTask{taskset: ts, id: uint32(id), elf: elf}
		if v := os.Getenv("PS3_SPURSTRACE"); v != "" && strings.Contains(v, fmt.Sprintf("%x", elf)) {
			tk.trace = true
		}
		tk.entry, tk.segs = m.spuELFSegs(elf)
		if argp != 0 {
			for i := range tk.arg {
				tk.arg[i] = m.Mem.Read32(argp + uint32(i)*4)
			}
		}
		if idp != 0 {
			m.Mem.Write32(idp, uint32(id))
		}
		sp.made++
		if spursLog {
			m.Logf("SPURS task %d of taskset %08X: ELF %08X, entry %05X, argument %08X %08X %08X %08X", id, ts, elf, tk.entry, tk.arg[0], tk.arg[1], tk.arg[2], tk.arg[3])
		}
		if strings.Contains(os.Getenv("PS3_SPURSSKIP"), fmt.Sprintf("%x", elf)) {
			// debugging: don't run it, post an empty result
			q := tk.arg[0]
			a, b := m.Mem.Read32(q), m.Mem.Read32(q+4)
			_ = a
			size, depth, base := m.Mem.Read32(q+8), m.Mem.Read32(q+12), m.Mem.Read32(q+0x14)
			slot := b
			if slot >= depth {
				slot -= depth
			}
			m.Mem.Fill(base+size*slot, 0, size)
			b++
			if b >= 2*depth {
				b = 0
			}
			m.Mem.Write32(q+4, b)
			ids[id] = false
			t.ret(0)
			return
		}
		sp.ready = append(sp.ready, tk)
		m.spursRun()
		t.ret(0)
	})
	// (?, taskset, queue, buffer, size, depth, direction)
	reg("_cellSpursQueueInitialize", func(m *Machine, t *Thread) {
		ts, q, buf, size, depth, dir := t.arg(1), t.arg(2), t.arg(3), t.arg(4), t.arg(5), t.arg(6)
		if q == 0 || buf == 0 {
			t.ret(spursErrNull)
			return
		}
		if q&127 != 0 || buf&15 != 0 {
			t.ret(spursErrAlign)
			return
		}
		if size == 0 || size&15 != 0 || depth == 0 || depth >= 0x8000 || dir > 2 {
			t.ret(spursErrInval)
			return
		}
		m.Mem.Fill(q, 0, 128)
		m.Mem.Write32(q+8, size)
		m.Mem.Write32(q+12, depth)
		m.Mem.Write32(q+0x14, buf)
		m.Mem.Write32(q+0x18, 0xFFFFFFFF)
		m.Mem.Write32(q+0x1C, dir)
		m.Mem.Write32(q+0x64, ts)
		t.ret(0)
	})
	// (queue, buffer, isPeek, isBlocking)
	reg("cellSpursQueuePopBody", func(m *Machine, t *Thread) {
		q, buf, peek, wait := t.arg(0), t.arg(1), t.arg(2) != 0, t.arg(3) != 0
		if q == 0 || buf == 0 {
			t.ret(spursErrNull)
			return
		}
		if m.spursQueuePop(q, buf, peek) {
			t.ret(0)
			return
		}
		if !wait {
			t.ret(spursErrAgain)
			return
		}
		m.block(t, "SPURS queue", func() bool {
			if !m.spursQueuePop(q, buf, peek) {
				return false
			}
			t.ret(0)
			return true
		})
	})
}
