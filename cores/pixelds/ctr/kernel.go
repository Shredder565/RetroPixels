package ctr

import (
	"fmt"
	"sort"

	"retropixels/cores/pixelds/arm"
)

// The kernel, done at a high level: threads with priorities, handles, the
// synchronisation objects (events, mutexes, semaphores, timers, address
// arbiters), memory blocks, and IPC sessions to the system services in
// services.go. Threads switch when one blocks, exits or wakes another of
// higher priority, as the 3DS's kernel does on the application core.

// Result codes.
const (
	rcOK            = 0
	rcTimeout       = 0x09401bfe
	rcInvalidHandle = 0xd8e007f7
	rcOutOfMemory   = 0xd86007f3
	rcInvalidAddr   = 0xe0e01bf5
	rcNotFound      = 0xd88007fa
	rcInvalidCombo  = 0xe0e01bee
	rcNotImpl       = 0xf8c007f4
)

const (
	hCurProcess = 0xffff8001
	hCurThread  = 0xffff8000
)

type object interface{ name() string }

// waitable objects: signaled reports whether t would get it now; take is
// called when t gets it.
type waitable interface {
	object
	signaled(t *thread) bool
	take(t *thread)
}

const (
	thReady = iota
	thWaiting
	thDead
)

type thread struct {
	id     uint32
	prio   int32
	state  int
	ctx    arm.State
	tls    uint32
	tlsIdx int

	// waiting
	waitObjs []waitable
	waitAll  bool
	wakeAt   int64 // tick of the timeout, or -1
	arbAddr  uint32
	onArb    bool
	waitOut  bool // WaitSynchronizationN: report the index in r1
	sleeping bool
}

func (t *thread) name() string          { return fmt.Sprintf("thread %d", t.id) }
func (t *thread) signaled(*thread) bool { return t.state == thDead }
func (t *thread) take(*thread)          {}

type event struct {
	reset uint32 // 0 one-shot, 1 sticky, 2 pulse
	set   bool
	label string
}

func (e *event) name() string          { return "event " + e.label }
func (e *event) signaled(*thread) bool { return e.set }
func (e *event) take(*thread) {
	if e.reset == 0 {
		e.set = false
	}
}

type mutex struct {
	owner *thread
	count int
}

func (x *mutex) name() string { return "mutex" }
func (x *mutex) signaled(t *thread) bool {
	return x.owner == nil || x.owner == t
}
func (x *mutex) take(t *thread) { x.owner = t; x.count++ }

type semaphore struct{ count, max int32 }

func (s *semaphore) name() string          { return "semaphore" }
func (s *semaphore) signaled(*thread) bool { return s.count > 0 }
func (s *semaphore) take(*thread)          { s.count-- }

type timer struct {
	event
	at, interval int64 // ticks; at < 0: not running
}

func (t *timer) name() string { return "timer" }

type arbiter struct{}

func (*arbiter) name() string { return "address arbiter" }

type memBlock struct {
	pages []uint32 // physical pages
	size  uint32
	label string
}

func (b *memBlock) name() string { return "memory block " + b.label }

type process struct{}

func (*process) name() string          { return "process" }
func (*process) signaled(*thread) bool { return false }
func (*process) take(*thread)          {}

type resourceLimit struct{}

func (*resourceLimit) name() string { return "resource limit" }

// session is a client session to a service.
type session struct {
	svc service
}

func (s *session) name() string { return "session to " + s.svc.name() }

// port is a named port (srv:).
type port struct{ svc service }

func (p *port) name() string { return "port " + p.svc.name() }

// ---------------- the kernel ----------------

type kernel struct {
	m       *Machine
	cpu     *arm.CPU
	mem     *memory
	handles map[uint32]object
	next    uint32
	threads []*thread
	cur     *thread
	nextTID uint32
	tlsUsed [64]bool
	timers  []*timer
	// things the machine does at regular times (the DSP's audio frames)
	periodic []*periodicTask
	log      func(string, ...any)
}

func newKernel(m *Machine) *kernel {
	return &kernel{m: m, cpu: m.cpu, mem: m.mem, handles: map[uint32]object{}, next: 0x10000, nextTID: 1, log: m.logf}
}

func (k *kernel) add(o object) uint32 {
	k.next += 0x10
	k.handles[k.next] = o
	return k.next
}

func (k *kernel) get(h uint32) object {
	switch h {
	case hCurThread:
		return k.cur
	case hCurProcess:
		return &process{}
	}
	return k.handles[h]
}

func (k *kernel) now() int64 { return k.cpu.Cycles }

// newThread makes a ready thread.
func (k *kernel) newThread(entry, arg, sp uint32, prio int32) (*thread, error) {
	idx := -1
	for i, u := range k.tlsUsed {
		if !u {
			idx = i
			break
		}
	}
	if idx < 0 {
		return nil, fmt.Errorf("too many threads")
	}
	k.tlsUsed[idx] = true
	t := &thread{id: k.nextTID, prio: prio, tlsIdx: idx, tls: tlsBase + uint32(idx)*0x200, wakeAt: -1}
	k.nextTID++
	for i := uint32(0); i < 0x200; i += 4 {
		k.mem.Write32(t.tls+i, 0)
	}
	cpsr := uint32(arm.ModeUSR)
	if entry&1 != 0 {
		cpsr |= arm.FlagT
	}
	var st arm.State
	st.R[0] = arg
	st.R[13] = sp
	st.R[15] = entry &^ 1
	st.Next = entry &^ 1
	st.CPSR = cpsr
	st.FPSCR = 0x03c00000 // flush to zero, default NaN (the kernel's setting)
	st.FPEXC = 0x40000000
	t.ctx = st
	k.threads = append(k.threads, t)
	return t, nil
}

// save keeps the running thread's registers.
func (k *kernel) save() {
	if k.cur != nil {
		c := k.cpu.Cycles
		k.cur.ctx = k.cpu.SaveState()
		k.cur.ctx.Cycles = c
	}
}

// load makes t the running thread.
func (k *kernel) load(t *thread) {
	c := k.cpu.Cycles
	k.cpu.LoadState(t.ctx)
	k.cpu.Cycles = c
	k.cpu.Halted = false
	k.cur = t
}

// pick returns the ready thread to run: the highest priority (lowest
// number), the current one first among equals unless it yields.
func (k *kernel) pick(yield bool) *thread {
	var best *thread
	for _, t := range k.threads {
		if t.state != thReady {
			continue
		}
		if best == nil || t.prio < best.prio {
			best = t
		}
	}
	if best == nil {
		return nil
	}
	if k.cur != nil && k.cur.state == thReady && k.cur.prio == best.prio && !yield {
		return k.cur
	}
	if yield && k.cur != nil && k.cur.state == thReady && k.cur.prio == best.prio {
		// the next thread of the same priority after the current one
		var same []*thread
		for _, t := range k.threads {
			if t.state == thReady && t.prio == best.prio {
				same = append(same, t)
			}
		}
		for i, t := range same {
			if t == k.cur {
				return same[(i+1)%len(same)]
			}
		}
	}
	return best
}

// reschedule switches to the thread that should run now (nil: none can,
// the CPU idles until an event).
func (k *kernel) reschedule(yield bool) {
	t := k.pick(yield)
	if t == k.cur {
		return
	}
	k.save()
	if t == nil {
		k.cur = nil
		k.cpu.Halted = true
		return
	}
	k.load(t)
}

// ---------------- waiting ----------------

// tryWait: whether t's wait is satisfied now; if so it takes the objects
// and returns the index for WaitSynchronizationN.
func (k *kernel) tryWait(t *thread) (bool, int) {
	if t.waitAll {
		for _, o := range t.waitObjs {
			if !o.signaled(t) {
				return false, 0
			}
		}
		for _, o := range t.waitObjs {
			o.take(t)
		}
		return true, 0
	}
	for i, o := range t.waitObjs {
		if o.signaled(t) {
			o.take(t)
			return true, i
		}
	}
	return false, 0
}

// wake ends t's wait with a result.
func (k *kernel) wake(t *thread, result uint32, idx int) {
	t.state = thReady
	t.waitObjs = nil
	t.wakeAt = -1
	t.onArb = false
	t.sleeping = false
	t.ctx.R[0] = result
	if t.waitOut {
		t.ctx.R[1] = uint32(idx)
	}
	t.waitOut = false
}

// update wakes the threads whose waits are satisfied, in priority order,
// then switches to a higher priority thread if one woke up.
func (k *kernel) update() {
	ws := make([]*thread, 0, len(k.threads))
	for _, t := range k.threads {
		if t.state == thWaiting && len(t.waitObjs) > 0 {
			ws = append(ws, t)
		}
	}
	sort.SliceStable(ws, func(i, j int) bool { return ws[i].prio < ws[j].prio })
	for _, t := range ws {
		if ok, i := k.tryWait(t); ok {
			k.wake(t, rcOK, i)
		}
	}
	k.reschedule(false)
}

type periodicTask struct {
	at, interval int64
	fn           func()
}

// every runs fn every interval ticks from now on.
func (k *kernel) every(interval int64, fn func()) {
	k.periodic = append(k.periodic, &periodicTask{at: k.now() + interval, interval: interval, fn: fn})
}

// tick handles timeouts and timers at the current time.
func (k *kernel) tick() {
	now := k.now()
	for _, p := range k.periodic {
		for p.at <= now {
			p.at += p.interval
			p.fn()
		}
	}
	for _, tm := range k.timers {
		if tm.at >= 0 && tm.at <= now {
			tm.set = true
			if tm.interval > 0 {
				tm.at += tm.interval
				if tm.at <= now {
					tm.at = now + tm.interval
				}
			} else {
				tm.at = -1
			}
			if tm.reset == 2 { // pulse: wake waiters, then clear
				k.updateNoSwitch()
				tm.set = false
			}
		}
	}
	for _, t := range k.threads {
		if t.state == thWaiting && t.wakeAt >= 0 && t.wakeAt <= now {
			if t.sleeping {
				k.wake(t, rcOK, 0)
			} else {
				k.wake(t, rcTimeout, 0)
			}
		}
	}
	k.update()
}

func (k *kernel) updateNoSwitch() {
	for _, t := range k.threads {
		if t.state == thWaiting && len(t.waitObjs) > 0 {
			if ok, i := k.tryWait(t); ok {
				k.wake(t, rcOK, i)
			}
		}
	}
}

// nextEvent is the tick of the next timeout or timer.
func (k *kernel) nextEvent() int64 {
	n := int64(1) << 62
	for _, p := range k.periodic {
		n = min(n, p.at)
	}
	for _, tm := range k.timers {
		if tm.at >= 0 && tm.at < n {
			n = tm.at
		}
	}
	for _, t := range k.threads {
		if t.state == thWaiting && t.wakeAt >= 0 && t.wakeAt < n {
			n = t.wakeAt
		}
	}
	return n
}

// signal sets an event from a service and wakes its waiters.
func (k *kernel) signal(e *event) {
	e.set = true
	if e.reset == 2 {
		k.updateNoSwitch()
		e.set = false
	}
	k.update()
}

func nsToTicks(ns int64) int64 {
	if ns < 0 {
		return -1
	}
	if ns > 1<<62/cpuHz { // longer than the machine will run: never
		return 1 << 62
	}
	return ns * cpuHz / 1e9
}

// block puts the current thread to waiting and switches away.
func (k *kernel) block(timeout int64) {
	t := k.cur
	t.state = thWaiting
	if timeout >= 0 && nsToTicks(timeout) < 1<<62 {
		t.wakeAt = k.now() + nsToTicks(timeout)
	} else {
		t.wakeAt = -1
	}
	k.reschedule(false)
}
