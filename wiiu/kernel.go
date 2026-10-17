package wiiu

// Threads and the scheduler.
//
// Cafe OS runs programs on three cores; each OS thread has a priority (0 is
// the highest, 31 the lowest) and a set of cores it may run on. Each core
// runs its highest-priority ready thread, taking turns between threads of the
// same priority. RetroPixels runs the three cores in turn, a slice of
// instructions at a time, all on one host thread, so the emulation is the
// same every run (and save states are simple).
//
// Waiting works by running the library call again: a function that must wait
// (see hle.go) blocks the thread on a key (usually the address of the mutex,
// event, queue or thread waited for); waking the key makes its threads ready,
// and each one runs its call again to see whether it can go on now.

import (
	"sort"
)

const (
	thReady = iota
	thWaiting
	thSuspended // created but not started, or OSSuspendThread
	thDone
	thDormant // a default thread no one is running
)

// thread attributes
const (
	attrAffinity0   = 1
	attrAffinity1   = 2
	attrAffinity2   = 4
	attrAffinityAny = 7
	attrDetached    = 8
)

// OSThread fields RetroPixels keeps up to date in the program's structure
const (
	osThreadTag      = 0x320
	osThreadState    = 0x324
	osThreadAttr     = 0x325
	osThreadID       = 0x326
	osThreadSuspend  = 0x328
	osThreadPriority = 0x32c
	osThreadBasePrio = 0x330
	osThreadExit     = 0x334
	osThreadStackLo  = 0x394
	osThreadStackHi  = 0x398
	osThreadEntry    = 0x39c
	osThreadSpecific = 0x5a0 // 16 values
	osThreadName     = 0x5c0
	osThreadSize     = 0x6a0
)

// Thread is an OS thread.
type Thread struct {
	ID       uint32
	Addr     uint32 // the program's OSThread
	Ctx      Context
	State    int
	Suspend  int
	Priority int
	Affinity uint32
	Core     int // the core it last ran on (-1 = none)
	Attr     uint32
	Exit     uint32
	Name     uint32

	WaitKey  uint64 // what it's waiting for (0 = nothing)
	WakeAt   uint64 // time base value to wake at (0 = none)
	Phase    int    // where a multi-step wait got to
	Saved    uint32 // a value kept across a wait (a mutex count)
	TimedOut bool   // woken by WakeAt rather than its key
	Deadline uint64 // a timeout kept across a wait

	LastRun  uint64 // for taking turns
	Specific [16]uint32
	Cleanup  uint32 // cleanup callback
	Dealloc  uint32 // deallocator callback
	Cancel   bool
	TLS      map[uint32]uint32 // TLS block of each module

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
)

// block makes the current library call wait for key (and/or until wakeAt).
func (t *Thread) block(key uint64, wakeAt uint64) {
	t.State = thWaiting
	t.WaitKey = key
	t.WakeAt = wakeAt
	t.TimedOut = false
	t.blocked = true
}

// yield gives the core to another thread and runs the call again later.
func (t *Thread) yield() {
	t.blocked = true
}

func (m *Machine) threadAt(addr uint32) *Thread {
	return m.threads[addr]
}

// wake makes every thread waiting on key ready.
func (m *Machine) wake(key uint64) int {
	n := 0
	for _, t := range m.threadList {
		if t.State == thWaiting && t.WaitKey == key {
			t.State = thReady
			t.WaitKey = 0
			t.WakeAt = 0
			n++
		}
	}
	return n
}

// wakeOne wakes the highest-priority thread waiting on key.
func (m *Machine) wakeOne(key uint64) *Thread {
	var best *Thread
	for _, t := range m.threadList {
		if t.State == thWaiting && t.WaitKey == key {
			if best == nil || t.Priority < best.Priority {
				best = t
			}
		}
	}
	if best != nil {
		best.State = thReady
		best.WaitKey = 0
		best.WakeAt = 0
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

// newThread sets up a thread (suspended) for an OSThread at addr.
func (m *Machine) newThread(addr, entry, argc, argv, stackTop, stackSize uint32, prio int, attr uint32) *Thread {
	if old := m.threads[addr]; old != nil {
		m.removeThread(old)
	}
	m.nextThreadID++
	t := &Thread{ID: m.nextThreadID, Addr: addr, State: thSuspended, Suspend: 1, Priority: prio,
		Attr: attr, Core: -1}
	t.Affinity = attr & attrAffinityAny
	if t.Affinity == 0 {
		if cur := m.running; cur != nil && cur.Core >= 0 {
			t.Affinity = 1 << cur.Core
		} else {
			t.Affinity = attrAffinityAny
		}
	}
	t.Ctx.PC = entry
	t.Ctx.GPR[1] = (stackTop - 8) &^ 7
	m.Mem.Write32(t.Ctx.GPR[1], 0)
	m.Mem.Write32(t.Ctx.GPR[1]+4, 0)
	t.Ctx.GPR[3] = argc
	t.Ctx.GPR[4] = argv
	t.Ctx.LR = m.thunk("coreinit", "__RPThreadReturn")
	if main := m.mainModule(); main != nil {
		t.Ctx.GPR[2] = main.Info.SDA2Base
		t.Ctx.GPR[13] = main.Info.SDABase
	}
	m.threads[addr] = t
	m.threadList = append(m.threadList, t)
	sort.Slice(m.threadList, func(i, j int) bool { return m.threadList[i].ID < m.threadList[j].ID })

	mem := m.Mem
	mem.Fill(addr, 0, osThreadSize)
	mem.Write32(addr+osThreadTag, 0x74487244) // "tHrD"
	mem.Write16(addr+osThreadID, uint16(t.ID))
	mem.Write8(addr+osThreadAttr, uint8(attr))
	mem.Write32(addr+osThreadStackHi, stackTop)
	mem.Write32(addr+osThreadStackLo, stackTop-stackSize)
	mem.Write32(addr+osThreadEntry, entry)
	m.syncThread(t)
	return t
}

// syncThread writes the thread's state into the program's OSThread.
func (m *Machine) syncThread(t *Thread) {
	mem := m.Mem
	var st uint8
	switch t.State {
	case thReady:
		st = 1
		if m.running == t {
			st = 2
		}
	case thWaiting:
		st = 4
	case thSuspended, thDormant:
		st = 0
		if t.State == thSuspended && t.Ctx.PC != 0 {
			st = 1 // ready but suspended
		}
	case thDone:
		st = 8
	}
	mem.Write8(t.Addr+osThreadState, st)
	mem.Write32(t.Addr+osThreadSuspend, uint32(t.Suspend))
	mem.Write32(t.Addr+osThreadPriority, uint32(t.Priority))
	mem.Write32(t.Addr+osThreadBasePrio, uint32(t.Priority))
	mem.Write32(t.Addr+osThreadExit, t.Exit)
	mem.Write32(t.Addr+osThreadName, t.Name)
}

func (m *Machine) removeThread(t *Thread) {
	delete(m.threads, t.Addr)
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

// exitThread ends t with value v.
func (m *Machine) exitThread(t *Thread, v uint32) {
	t.State = thDone
	t.Exit = v
	t.WaitKey = 0
	m.syncThread(t)
	m.wake(keyJoin | uint64(t.Addr))
	// release mutexes it still holds (the OS does this too)
	for _, mx := range m.heldMutexes(t) {
		m.Mem.Write32(mx+0x1c, 0)
		m.Mem.Write32(mx+0x20, 0)
		m.wake(keyMutex | uint64(mx))
	}
	if t == m.mainThread {
		m.exitProgram(v)
	}
	if t.Attr&attrDetached != 0 && t.Dealloc == 0 {
		m.removeThread(t)
	}
}

// pick chooses the thread core c runs next.
func (m *Machine) pick(core int) *Thread {
	var best *Thread
	for _, t := range m.threadList {
		if t.State != thReady || t.Affinity&(1<<core) == 0 {
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
			t.State = thReady
			t.TimedOut = t.WaitKey != 0
			t.WaitKey = 0
			t.WakeAt = 0
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

// heldMutexes lists the mutexes t owns.
func (m *Machine) heldMutexes(t *Thread) []uint32 {
	var out []uint32
	for a := range m.mutexes {
		if m.Mem.Read32(a+0x1c) == t.Addr && m.Mem.Read32(a) == 0x6d557458 {
			out = append(out, a)
		}
	}
	sort.Slice(out, func(i, j int) bool { return out[i] < out[j] })
	return out
}
