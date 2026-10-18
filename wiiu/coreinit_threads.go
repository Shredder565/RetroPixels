// This Source Code Form is subject to the terms of the Mozilla Public License,
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at https://mozilla.org/MPL/2.0/.
//
// Based on Cemu (https://github.com/cemu-project/Cemu), src/Cafe/OS/libs/
// coreinit/coreinit_Thread.cpp, coreinit_Thread.h, coreinit_ThreadQueue.cpp
// and coreinit_Scheduler.cpp, rewritten in Go for RetroPixels.

package wiiu

// coreinit: threads (creating, starting, suspending, ending and joining
// them, their priorities and cores), thread queues and the interrupt switch.

// OSThread fields RetroPixels keeps up to date in the program's structure
// (an OSContext comes first, at 0)
const (
	osThreadTag       = 0x320
	osThreadState     = 0x324
	osThreadAttr      = 0x325
	osThreadID        = 0x326
	osThreadSuspend   = 0x328
	osThreadPriority  = 0x32c // the priority it runs at
	osThreadBasePrio  = 0x330
	osThreadExit      = 0x334
	osThreadStackHi   = 0x394
	osThreadStackLo   = 0x398
	osThreadEntry     = 0x39c
	osThreadEHGlobals = 0x408 // the C++ runtime's exception state
	osThreadSpecific  = 0x57c // 16 values
	osThreadType      = 0x5bc
	osThreadName      = 0x5c0
	osThreadSize      = 0x6a0
)

// OSThread states as the program sees them
const (
	osStateNone     = 0
	osStateReady    = 1
	osStateRunning  = 2
	osStateWaiting  = 4
	osStateMoribund = 8
)

// thread types: each has its own range of priorities, I/O threads' above
// every app thread's
const (
	threadDriver = 0
	threadIO     = 1
	threadApp    = 2
)

func typePriorityBase(typ uint32) int {
	switch typ {
	case threadIO:
		return 32
	case threadApp:
		return 64
	}
	return 0
}

// initThread sets up a thread for the OSThread at addr. One with an entry
// point is ready but suspended once (OSResumeThread starts it); one without
// is dormant until OSRunThread.
func (m *Machine) initThread(addr, entry, argc, argv, stackTop, stackSize uint32, prio int, attr, typ uint32) *Thread {
	if old := m.threads[addr]; old != nil {
		m.removeThread(old)
	}
	delete(m.conts, addr)
	m.nextThreadID++
	t := &Thread{ID: m.nextThreadID, Addr: addr, Attr: attr, Type: typ, Core: -1, Exit: 0xffffffff}
	t.BasePrio = prio + typePriorityBase(typ)
	t.Priority = t.BasePrio
	t.Affinity = attr & attrAffinityAny
	if t.Affinity == 0 {
		// the creating thread's core
		core := 1
		if cur := m.running; cur != nil && cur.Core >= 0 {
			core = cur.Core
		}
		t.Affinity = 1 << core
		t.Attr = attr&^(attrAffinityAny|0x10) | t.Affinity
	}
	t.State = thDormant
	if entry != 0 {
		t.State, t.Suspend = thSuspended, 1
	}
	mem := m.Mem
	mem.Fill(addr, 0, osThreadSize)
	mem.Write32(addr, 0x4f53436f)             // "OSCo"
	mem.Write32(addr+4, 0x6e747874)           // "ntxt"
	mem.Write32(addr+osThreadTag, 0x74487244) // "tHrD"
	mem.Write16(addr+osThreadID, uint16(t.ID))
	mem.Write32(addr+osThreadStackHi, stackTop)
	mem.Write32(addr+osThreadStackLo, stackTop-stackSize)
	mem.Write32(addr+osThreadEntry, entry)
	if stackSize != 0 {
		mem.Write32(stackTop-stackSize, 0xdeadbabe)
	}
	m.startContext(t, argc, argv)
	if entry == 0 {
		t.Ctx.PC = 0
	}
	if main := m.mainModule(); main != nil {
		t.Ctx.GPR[2] = main.Info.SDA2Base
		t.Ctx.GPR[13] = main.Info.SDABase
	}
	m.threads[addr] = t
	if entry != 0 {
		m.activate(t)
	}
	m.syncThread(t)
	return t
}

// startContext sets the registers a thread starts its entry point with: it
// returns into OSExitThread.
func (m *Machine) startContext(t *Thread, argc, argv uint32) {
	top := m.Mem.Read32(t.Addr+osThreadStackHi) &^ 7
	m.Mem.Write32(top-4, 0)
	m.Mem.Write32(top-8, 0)
	t.Ctx = Context{}
	t.Ctx.GPR[1] = top - 8
	t.Ctx.GPR[3], t.Ctx.GPR[4] = argc, argv
	t.Ctx.PC = m.thunk("coreinit", "__RPThreadStart")
	t.Ctx.LR = m.thunk("coreinit", "OSExitThread")
	t.Ctx.GQR[2], t.Ctx.GQR[3], t.Ctx.GQR[4], t.Ctx.GQR[5] = 0x40004, 0x50005, 0x60006, 0x70007
}

// syncThread writes the thread's state into the program's OSThread.
func (m *Machine) syncThread(t *Thread) {
	mem := m.Mem
	var st uint8
	switch t.State {
	case thReady, thSuspended:
		st = osStateReady
		if m.running == t {
			st = osStateRunning
		}
	case thWaiting:
		st = osStateWaiting
	case thDone:
		st = osStateMoribund
	}
	mem.Write8(t.Addr+osThreadState, st)
	mem.Write8(t.Addr+osThreadAttr, uint8(t.Attr))
	mem.Write32(t.Addr+osThreadSuspend, uint32(t.Suspend))
	mem.Write32(t.Addr+osThreadPriority, uint32(t.Priority))
	mem.Write32(t.Addr+osThreadBasePrio, uint32(t.BasePrio))
	mem.Write32(t.Addr+osThreadExit, t.Exit)
	mem.Write32(t.Addr+osThreadType, t.Type)
	mem.Write32(t.Addr+0x304, t.Affinity) // the context's affinity
}

// deactivate is the end of a thread that's been joined, or that ended
// detached: its OSThread may be made into a new thread or run again.
func (m *Machine) deactivate(t *Thread) {
	m.removeThread(t)
	t.State = thDormant
	t.WaitKey, t.WakeAt = 0, 0
	m.syncThread(t)
	m.Mem.Write16(t.Addr+osThreadID, 0x8000)
}

// updatePriority works out the priority t runs at.
func (m *Machine) updatePriority(t *Thread) {
	t.Priority = t.BasePrio
	if t.Boost > 0 && t.Priority > 0 {
		t.Priority = 0
	}
	m.syncThread(t)
}

// queueDealloc has the thread's deallocator called, after it, on the
// higher-priority callback thread of the core.
func (m *Machine) queueDealloc(core int, t *Thread) {
	if fn := t.Dealloc; fn != 0 {
		t.Dealloc = 0
		m.queueCallback(core, fn, t.Addr, m.Mem.Read32(t.Addr+osThreadStackLo))
	}
}

// cppExceptionPtr reads one of the pointers the C++ runtime gives coreinit
// (__cpp_exception_init_ptr or __cpp_exception_cleanup_ptr).
func (m *Machine) cppExceptionPtr(name string) uint32 {
	if a, ok := m.dataAddr["coreinit."+name]; ok {
		return m.Mem.Read32(a)
	}
	return 0
}

// osExitThread ends the current thread t with v: its cleanup callback, the
// C++ runtime's cleanup, then the end (endThread).
func (m *Machine) osExitThread(cpu *CPU, t *Thread, v uint32) {
	if cb := t.Cleanup; cb != 0 {
		t.Cleanup = 0
		m.callGuest(cpu, t, cb, []uint32{t.Addr, m.Mem.Read32(t.Addr + osThreadStackLo)},
			func(m *Machine, cpu *CPU, t *Thread, _ uint32) { m.osExitThread(cpu, t, v) })
		return
	}
	eh := t.Addr + osThreadEHGlobals
	if fn := m.cppExceptionPtr("__cpp_exception_cleanup_ptr"); fn != 0 && m.Mem.Read32(eh) != 0 {
		m.callGuest(cpu, t, fn, []uint32{eh}, func(m *Machine, cpu *CPU, t *Thread, _ uint32) {
			m.Mem.Write32(eh, 0)
			m.osExitThread(cpu, t, v)
		})
		return
	}
	m.endThread(cpu.Core, t, v)
	t.yield()
}

// endThread: t has ended with value v. It lets go of its mutexes and wakes
// its joiners; a detached thread is gone now (its deallocator runs), any
// other waits to be joined.
func (m *Machine) endThread(core int, t *Thread, v uint32) {
	t.Exit = v
	t.WaitKey, t.WakeAt = 0, 0
	t.IntsOff = false
	for _, mx := range m.heldMutexes(t) {
		m.Mem.Write32(mx+mutexCount, 0)
		m.releaseMutex(mx)
	}
	m.wake(keyJoin | uint64(t.Addr))
	if t.Attr&attrDetached != 0 {
		m.deactivate(t)
		m.queueDealloc(core, t)
	} else {
		t.State = thDone
		m.syncThread(t)
	}
	for c := range m.current {
		if m.current[c] == t {
			m.current[c] = nil
		}
	}
	if t == m.mainThread {
		m.exitProgram(v)
	}
}

func init() {
	c := "coreinit"

	regData(c, "__cpp_exception_init_ptr", 4, nil)
	regData(c, "__cpp_exception_cleanup_ptr", 4, nil)

	create := func(m *Machine, cpu *CPU, t *Thread, typ uint32) {
		addr, entry, argc, argv := arg(cpu, 0), arg(cpu, 1), arg(cpu, 2), arg(cpu, 3)
		stack, size, prio, attr := arg(cpu, 4), arg(cpu, 5), int(int32(arg(cpu, 6))), arg(cpu, 7)&0xff
		if addr == 0 || prio < 0 || prio > 31 || attr >= 0x20 || stack == 0 || size == 0 {
			ret(cpu, 0)
			return
		}
		if old := m.threadAt(addr); old != nil && old != t && old.State == thReady && old.Suspend == 0 {
			// made again while it still runs: wait for it to stop first
			t.block(0, m.Time()+TimerHz/2000)
			return
		}
		if old := m.threadAt(addr); old != nil && old.State == thDone {
			m.deactivate(old)
		}
		nt := m.initThread(addr, entry, argc, argv, stack, size, prio, attr, typ)
		nt.Ctx.GPR[2], nt.Ctx.GPR[13] = cpu.GPR[2], cpu.GPR[13]
		ret(cpu, 1)
	}
	// the 9th argument, the type, is on the stack
	typeArg := func(m *Machine, cpu *CPU) uint32 { return m.Mem.Read32(cpu.GPR[1] + 8) }
	reg(c, "OSCreateThread", func(m *Machine, cpu *CPU, t *Thread) { create(m, cpu, t, threadApp) })
	reg(c, "OSCreateThreadType", func(m *Machine, cpu *CPU, t *Thread) {
		typ := typeArg(m, cpu)
		if typ != threadApp && typ != threadIO {
			ret(cpu, 0)
			return
		}
		create(m, cpu, t, typ)
	})
	reg(c, "__OSCreateThreadType", func(m *Machine, cpu *CPU, t *Thread) { create(m, cpu, t, typeArg(m, cpu)&3) })

	// a thread starts here: the C++ runtime sets up its exception state,
	// then the entry point runs
	reg(c, "__RPThreadStart", func(m *Machine, cpu *CPU, t *Thread) {
		entry := m.Mem.Read32(t.Addr + osThreadEntry)
		argc, argv := cpu.GPR[3], cpu.GPR[4]
		if fn := m.cppExceptionPtr("__cpp_exception_init_ptr"); fn != 0 {
			m.callGuest(cpu, t, fn, []uint32{t.Addr + osThreadEHGlobals}, func(m *Machine, cpu *CPU, t *Thread, _ uint32) {
				cpu.GPR[3], cpu.GPR[4] = argc, argv
				cpu.PC = entry
				m.jumped = true
			})
			return
		}
		cpu.PC = entry
		m.jumped = true
	})

	resume := func(m *Machine, th *Thread, n int) int {
		prev := th.Suspend
		if prev == 0 {
			return 0
		}
		th.Suspend = max(prev-n, 0)
		if th.Suspend == 0 && th.State == thSuspended {
			th.State = thReady
			m.readied(th)
		}
		m.syncThread(th)
		return prev
	}
	reg(c, "OSResumeThread", func(m *Machine, cpu *CPU, t *Thread) {
		th := m.threadAt(arg(cpu, 0))
		if th == nil {
			ret(cpu, 0)
			return
		}
		ret(cpu, uint32(resume(m, th, 1)))
	})
	reg(c, "OSContinueThread", func(m *Machine, cpu *CPU, t *Thread) {
		if th := m.threadAt(arg(cpu, 0)); th != nil {
			resume(m, th, th.Suspend)
		}
		ret(cpu, 1)
	})
	suspend := func(m *Machine, cpu *CPU, t *Thread) {
		th := m.threadAt(arg(cpu, 0))
		if th == nil || th.State == thDormant || th.State == thDone {
			ret(cpu, 0)
			return
		}
		ret(cpu, uint32(th.Suspend))
		th.Suspend++
		if th.State == thReady {
			th.State = thSuspended
		}
		m.syncThread(th)
		if th == t {
			m.preempt(cpu)
		}
	}
	reg(c, "OSSuspendThread", suspend)
	reg(c, "__OSSuspendThreadNolock", suspend)
	reg(c, "OSRunThread", func(m *Machine, cpu *CPU, t *Thread) {
		th := m.threadAt(arg(cpu, 0))
		if th == nil || th == t || (th.State != thDormant && th.State != thDone) {
			ret(cpu, 0)
			return
		}
		m.Mem.Write32(th.Addr+osThreadEntry, arg(cpu, 1))
		m.startContext(th, arg(cpu, 2), arg(cpu, 3))
		th.Ctx.GPR[2], th.Ctx.GPR[13] = cpu.GPR[2], cpu.GPR[13]
		th.Suspend = 0
		th.State = thReady
		th.WaitKey, th.WakeAt = 0, 0
		th.doneWaiting()
		m.activate(th)
		m.syncThread(th)
		m.readied(th)
		ret(cpu, 1)
	})
	reg(c, "OSExitThread", func(m *Machine, cpu *CPU, t *Thread) { m.osExitThread(cpu, t, arg(cpu, 0)) })
	reg(c, "OSJoinThread", func(m *Machine, cpu *CPU, t *Thread) {
		th := m.threadAt(arg(cpu, 0))
		if th == nil || th == t {
			ret(cpu, 0)
			return
		}
		if th.Attr&attrDetached == 0 && th.State != thDone {
			t.block(keyJoin|uint64(th.Addr), 0)
			return
		}
		if th.State != thDone {
			ret(cpu, 0) // detached, and not ended
			return
		}
		if p := arg(cpu, 1); p != 0 {
			m.Mem.Write32(p, th.Exit)
		}
		ret(cpu, 1)
		m.deactivate(th)
		if th.Dealloc != 0 {
			// the deallocator runs before the join returns
			m.queueDealloc(cpu.Core, th)
			m.preempt(cpu)
		}
	})
	reg(c, "OSDetachThread", func(m *Machine, cpu *CPU, t *Thread) {
		th := m.threadAt(arg(cpu, 0))
		if th == nil {
			return
		}
		th.Attr |= attrDetached
		m.syncThread(th)
		if th.State == thDone {
			m.deactivate(th)
			m.wake(keyJoin | uint64(th.Addr))
			m.queueDealloc(cpu.Core, th)
		}
	})
	reg(c, "OSGetCurrentThread", func(m *Machine, cpu *CPU, t *Thread) {
		if t == nil {
			ret(cpu, 0)
			return
		}
		ret(cpu, t.Addr)
	})
	reg(c, "OSGetDefaultThread", func(m *Machine, cpu *CPU, t *Thread) {
		core := arg(cpu, 0)
		if core > 2 {
			ret(cpu, 0)
			return
		}
		ret(cpu, m.defaultTh[core].Addr)
	})
	reg(c, "OSSetThreadName", func(m *Machine, cpu *CPU, t *Thread) {
		m.Mem.Write32(arg(cpu, 0)+osThreadName, arg(cpu, 1))
	})
	reg(c, "OSGetThreadName", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, m.Mem.Read32(arg(cpu, 0)+osThreadName)) })
	reg(c, "OSSetThreadPriority", func(m *Machine, cpu *CPU, t *Thread) {
		th := m.threadAt(arg(cpu, 0))
		p := int(int32(arg(cpu, 1)))
		if th == nil || p < 0 || p > 31 {
			ret(cpu, 0)
			return
		}
		th.BasePrio = p + typePriorityBase(th.Type)
		m.updatePriority(th)
		if th != t {
			m.readied(th)
		}
		ret(cpu, 1)
	})
	reg(c, "OSGetThreadPriority", func(m *Machine, cpu *CPU, t *Thread) {
		if th := m.threadAt(arg(cpu, 0)); th != nil {
			ret(cpu, uint32(th.BasePrio-typePriorityBase(th.Type)))
			return
		}
		ret(cpu, 0)
	})
	reg(c, "OSSetThreadAffinity", func(m *Machine, cpu *CPU, t *Thread) {
		th := m.threadAt(arg(cpu, 0))
		if th == nil {
			ret(cpu, 0)
			return
		}
		a := arg(cpu, 1) & attrAffinityAny
		th.Affinity = a
		th.Attr = th.Attr&^attrAffinityAny | a
		m.syncThread(th)
		ret(cpu, 1)
		if th == t && a&(1<<cpu.Core) == 0 {
			m.preempt(cpu) // it can't stay on this core
		} else if th != t {
			m.readied(th)
		}
	})
	reg(c, "OSGetThreadAffinity", func(m *Machine, cpu *CPU, t *Thread) {
		if th := m.threadAt(arg(cpu, 0)); th != nil {
			ret(cpu, th.Affinity)
			return
		}
		ret(cpu, 0)
	})
	reg(c, "OSYieldThread", func(m *Machine, cpu *CPU, t *Thread) { m.preempt(cpu) })
	reg(c, "OSIsThreadTerminated", func(m *Machine, cpu *CPU, t *Thread) {
		th := m.threadAt(arg(cpu, 0))
		retBool(cpu, th == nil || th.State == thDone || th.State == thDormant)
	})
	reg(c, "OSIsThreadSuspended", func(m *Machine, cpu *CPU, t *Thread) {
		th := m.threadAt(arg(cpu, 0))
		retBool(cpu, th != nil && th.Suspend > 0)
	})
	reg(c, "OSIsThreadRunning", func(m *Machine, cpu *CPU, t *Thread) {
		th := m.threadAt(arg(cpu, 0))
		running := false
		for _, cur := range m.current {
			if th != nil && cur == th && th.State == thReady {
				running = true
			}
		}
		retBool(cpu, running)
	})
	reg(c, "OSSetThreadSpecific", func(m *Machine, cpu *CPU, t *Thread) {
		if i := arg(cpu, 0); i < 16 && t != nil {
			m.Mem.Write32(t.Addr+osThreadSpecific+i*4, arg(cpu, 1))
		}
	})
	reg(c, "OSGetThreadSpecific", func(m *Machine, cpu *CPU, t *Thread) {
		if i := arg(cpu, 0); i < 16 && t != nil {
			ret(cpu, m.Mem.Read32(t.Addr+osThreadSpecific+i*4))
			return
		}
		ret(cpu, 0)
	})
	reg(c, "OSSetThreadCleanupCallback", func(m *Machine, cpu *CPU, t *Thread) {
		if th := m.threadAt(arg(cpu, 0)); th != nil {
			ret(cpu, th.Cleanup)
			th.Cleanup = arg(cpu, 1)
			return
		}
		ret(cpu, 0)
	})
	reg(c, "OSSetThreadDeallocator", func(m *Machine, cpu *CPU, t *Thread) {
		if th := m.threadAt(arg(cpu, 0)); th != nil {
			ret(cpu, th.Dealloc)
			th.Dealloc = arg(cpu, 1)
			return
		}
		ret(cpu, 0)
	})
	reg(c, "OSGetStackPointer", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, cpu.GPR[1]) })
	reg(c, "OSCancelThread", func(m *Machine, cpu *CPU, t *Thread) {
		th := m.threadAt(arg(cpu, 0))
		if th == nil {
			return
		}
		th.Cancel = true
		if th == t {
			m.osExitThread(cpu, t, 0xffffffff)
		}
	})
	reg(c, "OSTestThreadCancel", func(m *Machine, cpu *CPU, t *Thread) {
		if t != nil && t.Cancel {
			m.osExitThread(cpu, t, 0xffffffff)
		}
	})
	reg(c, "OSSetThreadCancelState", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 1) })
	reg(c, "OSSetThreadStackUsage", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 1) })
	reg(c, "OSCheckThreadStackUsage", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0x1000) })
	reg(c, "OSCheckActiveThreads", func(m *Machine, cpu *CPU, t *Thread) {
		n := 0
		for _, th := range m.threadList {
			if th.State != thDormant {
				n++
			}
		}
		ret(cpu, uint32(n))
	})
	reg(c, "OSSleepTicks", func(m *Machine, cpu *CPU, t *Thread) {
		if t.Phase == 1 {
			return
		}
		ticks := arg64(cpu, 0)
		if int64(ticks) <= 0 {
			m.preempt(cpu)
			return
		}
		t.block(0, m.Time()+ticks)
		t.Phase = 1
	})

	// thread queues
	reg(c, "OSInitThreadQueue", func(m *Machine, cpu *CPU, t *Thread) { m.Mem.Fill(arg(cpu, 0), 0, 0x10) })
	reg(c, "OSInitThreadQueueEx", func(m *Machine, cpu *CPU, t *Thread) {
		m.Mem.Fill(arg(cpu, 0), 0, 0x10)
		m.Mem.Write32(arg(cpu, 0)+8, arg(cpu, 1))
	})
	reg(c, "OSSleepThread", func(m *Machine, cpu *CPU, t *Thread) {
		if !t.Woken {
			t.block(keyQueue|uint64(arg(cpu, 0)), 0)
		}
	})
	reg(c, "OSWakeupThread", func(m *Machine, cpu *CPU, t *Thread) { m.wake(keyQueue | uint64(arg(cpu, 0))) })

	// interrupts: while a thread has them off, no other thread takes its core
	// (alarm and other callbacks included)
	setInts := func(cpu *CPU, t *Thread, on bool) {
		if t == nil {
			ret(cpu, 0)
			return
		}
		retBool(cpu, !t.IntsOff)
		t.IntsOff = !on
	}
	reg(c, "OSDisableInterrupts", func(m *Machine, cpu *CPU, t *Thread) { setInts(cpu, t, false) })
	reg(c, "OSEnableInterrupts", func(m *Machine, cpu *CPU, t *Thread) { setInts(cpu, t, true) })
	reg(c, "OSRestoreInterrupts", func(m *Machine, cpu *CPU, t *Thread) { setInts(cpu, t, arg(cpu, 0) != 0) })
	reg(c, "OSIsInterruptEnabled", func(m *Machine, cpu *CPU, t *Thread) { retBool(cpu, t != nil && !t.IntsOff) })
}
