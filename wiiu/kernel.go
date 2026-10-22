package wiiu

// Threads and the scheduler.
//
// Cafe OS runs programs on three cores; each OS thread has a priority (lower
// runs first) and a set of cores it may run on. Each core runs its
// highest-priority ready thread, taking turns between threads of the same
// priority. RetroPixels runs the three cores in turn, a slice of
// instructions at a time, all on one host thread, so the emulation is the
// same every run (and save states are simple).
//
// Waiting works by running the library call again: a function that must wait
// (see hle.go) blocks the thread on a key (usually the address of the mutex,
// event, queue or thread waited for); waking the key makes its threads ready,
// and each one runs its call again. The call sees from Woken that it was
// woken (an event or a condition doesn't wait again), from Granted that what
// it waited for was handed to it (a mutex or a semaphore count), or checks
// again whether it can go on now.

import (
	"sort"
	"sync/atomic"
)

const (
	thReady = iota
	thWaiting
	thSuspended // ready, but its suspend count isn't 0 (a new thread starts so)
	thDone      // ended but not joined yet
	thDormant   // not running anything: a default thread, or a joined one
)

// thread attributes
const (
	attrAffinity0   = 1
	attrAffinity1   = 2
	attrAffinity2   = 4
	attrAffinityAny = 7
	attrDetached    = 8
)

// Thread is an OS thread.
type Thread struct {
	ID       uint32
	Addr     uint32 // the program's OSThread
	Ctx      Context
	State    int
	Suspend  int
	Priority int    // the priority it runs at: BasePrio, or 0 while it holds a spin lock
	BasePrio int    // the priority set for it plus its type's base (see coreinit_threads.go)
	Type     uint32 // driver, I/O or app thread
	Boost    int    // spin locks it holds
	Affinity uint32
	Core     int // the core it last ran on (-1 = none)
	Attr     uint32
	Exit     uint32
	IntsOff  bool // interrupts are off: no other thread takes its core

	WaitKey  uint64 // what it's waiting for (0 = nothing)
	WakeAt   uint64 // time base value to wake at (0 = none)
	WaitSeq  uint64 // when it started waiting, so waiters go first come, first served
	Phase    int    // where a multi-step wait got to
	Saved    uint32 // a value kept across a wait (a mutex count)
	TimedOut bool   // woken by WakeAt rather than its key
	Woken    bool   // woken by its key
	Granted  bool   // handed what it waited for
	GrantVal uint32 // the call's result, when Granted
	Deadline uint64 // a timeout kept across a wait

	LastRun uint64 // for taking turns
	Cleanup uint32 // cleanup callback
	Dealloc uint32 // deallocator callback
	Cancel  bool
	TLS     map[uint32]uint32 // TLS block of each module

	blocked bool // the current library call has to wait
}

// wait keys: kind in the top byte, address below
const (
	keyMutex   = 1 << 56
	keyCond    = 2 << 56
	keySem     = 3 << 56
	keyEvent   = 4 << 56
	keyJoin    = 5 << 56
	keyMsgRecv = 6 << 56
	keyMsgSend = 7 << 56
	keyQueue   = 8 << 56
	keyVsync   = 9 << 56
	keyAlarm   = 10 << 56
	keyFlip    = 11 << 56
	keyRendez  = 12 << 56
	keyFastMtx = 13 << 56
	keyFastCnd = 14 << 56
	keyGPU     = 15 << 56
	keyAlarmQ  = 16 << 56
)

var waitSeq atomic.Uint64

// block makes the current library call wait for key (and/or until wakeAt).
func (t *Thread) block(key uint64, wakeAt uint64) {
	t.State = thWaiting
	t.WaitKey = key
	t.WakeAt = wakeAt
	t.WaitSeq = waitSeq.Add(1)
	t.TimedOut, t.Woken, t.Granted = false, false, false
	t.blocked = true
}

// yield gives the core to another thread and runs the call again later.
func (t *Thread) yield() {
	t.blocked = true
}

// doneWaiting forgets how the thread's last wait ended, once the call that
// waited has gone on.
func (t *Thread) doneWaiting() {
	t.Phase = 0
	t.TimedOut, t.Woken, t.Granted = false, false, false
}

func (m *Machine) threadAt(addr uint32) *Thread {
	return m.threads[addr]
}

// ready ends t's wait.
func (m *Machine) ready(t *Thread) {
	t.State = thReady
	if t.Suspend > 0 {
		t.State = thSuspended
	}
	t.WaitKey = 0
	t.WakeAt = 0
	m.readied(t)
}

// readied notes that t can run now: if it should take the core from the
// thread that's running, that happens when the current library call returns.
func (m *Machine) readied(t *Thread) {
	r := m.running
	if r == nil || r == t || t.State != thReady || t.Priority < 0 {
		return
	}
	if r.Core >= 0 && t.Affinity&(1<<r.Core) != 0 && t.Priority < r.Priority {
		m.resched = true
	}
}

// wake makes every thread waiting on key ready.
func (m *Machine) wake(key uint64) int {
	n := 0
	for _, t := range m.threadList {
		if t.State == thWaiting && t.WaitKey == key {
			m.ready(t)
			t.Woken = true
			n++
		}
	}
	return n
}

// wakeOne wakes the thread waiting on key that's first in line: the
// highest-priority one, the first to wait of those.
func (m *Machine) wakeOne(key uint64) *Thread {
	var best *Thread
	for _, t := range m.threadList {
		if t.State == thWaiting && t.WaitKey == key {
			if best == nil || t.Priority < best.Priority || t.Priority == best.Priority && t.WaitSeq < best.WaitSeq {
				best = t
			}
		}
	}
	if best != nil {
		m.ready(best)
		best.Woken = true
	}
	return best
}

func (m *Machine) waiting(key uint64) bool {
	for _, t := range m.threadList {
		if t.State == thWaiting && t.WaitKey == key {
			return true
		}
	}
	return false
}

// activate puts t in the list of threads the scheduler looks at.
func (m *Machine) activate(t *Thread) {
	m.threads[t.Addr] = t
	for _, x := range m.threadList {
		if x == t {
			return
		}
	}
	m.threadList = append(m.threadList, t)
	sort.Slice(m.threadList, func(i, j int) bool { return m.threadList[i].ID < m.threadList[j].ID })
}

// removeThread takes t out of the scheduler's list (it stays known by its
// address until another thread is made there).
func (m *Machine) removeThread(t *Thread) {
	for i, x := range m.threadList {
		if x == t {
			m.threadList = append(m.threadList[:i], m.threadList[i+1:]...)
			break
		}
	}
	for c := range m.current {
		if m.current[c] == t {
			m.current[c] = nil
		}
	}
}

// runnable: t may run on core now.
func (t *Thread) runnable(core int) bool {
	return t.State == thReady && t.Suspend == 0 && t.Affinity&(1<<core) != 0
}

// pick chooses the thread core c runs next.
func (m *Machine) pick(core int) *Thread {
	var best *Thread
	for _, t := range m.threadList {
		if !t.runnable(core) {
			continue
		}
		busy := false
		for o := range m.current {
			if o != core && m.current[o] == t {
				busy = true
			}
		}
		if busy {
			continue
		}
		if best == nil || t.Priority < best.Priority ||
			(t.Priority == best.Priority && t.LastRun < best.LastRun) {
			best = t
		}
	}
	return best
}

// wakeTimers readies threads whose sleep or timeout is over.
func (m *Machine) wakeTimers() {
	for _, t := range m.threadList {
		if t.State == thWaiting && t.WakeAt != 0 && m.tb >= t.WakeAt {
			timedOut := t.WaitKey != 0
			m.ready(t)
			t.TimedOut = timedOut
		}
	}
	m.runAlarms()
	m.audioTick()
}

// nextTimer is the earliest time a sleeping thread or alarm wakes (0 = none).
func (m *Machine) nextTimer() uint64 {
	var next uint64
	for _, t := range m.threadList {
		if t.State == thWaiting && t.WakeAt != 0 && (next == 0 || t.WakeAt < next) {
			next = t.WakeAt
		}
	}
	if a := m.nextAlarm(); a != 0 && (next == 0 || a < next) {
		next = a
	}
	if a := m.nextAudio(); a != 0 && (next == 0 || a < next) {
		next = a
	}
	return next
}
