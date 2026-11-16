package wiiu

// coreinit threads: making, starting, suspending, ending and joining them,
// their priorities, cores and per-thread values, thread queues, and turning
// interrupts off and on. The scheduling itself is RetroPixels' own kernel
// (kernel.go); this keeps the program's OSThread structures in step with it.
//
// Written for RetroPixels from the SDK's OSThread layout and the behaviour
// programs expect.

// OSThread, after the OSContext at its start
const (
	osThreadTag       = 0x320
	osThreadState     = 0x324 // u8
	osThreadAttr      = 0x325 // u8
	osThreadID        = 0x326 // u16
	osThreadSuspend   = 0x328
	osThreadPriority  = 0x32c // what it runs at, boosts included
	osThreadBasePrio  = 0x330
	osThreadExit      = 0x334
	osThreadStackHi   = 0x394
	osThreadStackLo   = 0x398
	osThreadEntry     = 0x39c
	osThreadEHGlobals = 0x408 // the C++ runtime's exception state
	osThreadSpecific  = 0x57c // 16 words
	osThreadType      = 0x5bc
	osThreadName      = 0x5c0
	osThreadSize      = 0x6a0

	osContextAffinity = 0x304

	// the states the program sees
	osStateNone     = 0
	osStateReady    = 1
	osStateRunning  = 2
	osStateWaiting  = 4
	osStateMoribund = 8

	// Thread types. Each has its own band of priorities (0-31 within it),
	// I/O threads' above every app thread's.
	threadDriver = 0
	threadIO     = 1
	threadApp    = 2

	threadNoExit = 0xffffffff // the exit value of a cancelled thread
)

func typePriorityBase(typ uint32) int {
	if typ == threadIO || typ == threadApp {
		return 32 * int(typ)
	}
	return 0
}

// initThread makes the thread for the OSThread at addr. With an entry point
// it is ready but suspended once, for OSResumeThread to start; without one
// it is dormant until OSRunThread.
func (m *Machine) initThread(addr, entry, argc, argv, stackTop, stackSize uint32, prio int, attr, typ uint32) *Thread {
	if old := m.threads[addr]; old != nil {
		m.removeThread(old)
	}
	delete(m.conts, addr)
	m.nextThreadID++
	t := &Thread{ID: m.nextThreadID, Addr: addr, Attr: attr, Type: typ, Core: -1, Exit: threadNoExit}
	t.BasePrio = prio + typePriorityBase(typ)
	t.Priority = t.BasePrio
	if t.Affinity = attr & attrAffinityAny; t.Affinity == 0 {
		// no core given: the core of the thread making it
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
	mem.WriteBytes(addr, []byte("OSContxt"))
	mem.Write32(addr+osThreadTag, 't'<<24|'H'<<16|'r'<<8|'D')
	mem.Write16(addr+osThreadID, uint16(t.ID))
	mem.Write32(addr+osThreadStackHi, stackTop)
	mem.Write32(addr+osThreadStackLo, stackTop-stackSize)
	mem.Write32(addr+osThreadEntry, entry)
	if stackSize != 0 {
		mem.Write32(stackTop-stackSize, 0xdeadbabe) // the stack's end marker
	}

	m.startContext(t, argc, argv)
	if entry == 0 {
		t.Ctx.PC = 0
	}
	if main := m.mainModule(); main != nil {
		t.Ctx.GPR[2], t.Ctx.GPR[13] = main.Info.SDA2Base, main.Info.SDABase
	}
	m.threads[addr] = t
	if entry != 0 {
		m.activate(t)
	}
	m.syncThread(t)
	return t
}

// startContext gives a thread the registers it starts with: (argc, argv)
// for __RPThreadStart, returning into OSExitThread, on an empty stack frame.
func (m *Machine) startContext(t *Thread, argc, argv uint32) {
	top := m.Mem.Read32(t.Addr+osThreadStackHi) &^ 7
	m.Mem.Write32(top-8, 0)
	m.Mem.Write32(top-4, 0)
	t.Ctx = Context{PC: m.thunk("coreinit", "__RPThreadStart"), LR: m.thunk("coreinit", "OSExitThread")}
	t.Ctx.GPR[1] = top - 8
	t.Ctx.GPR[3], t.Ctx.GPR[4] = argc, argv
	for i := 2; i <= 5; i++ {
		t.Ctx.GQR[i] = uint32(i+2) * 0x10001 // the SDK's paired-single quantizers
	}
}

// syncThread writes t's state into its OSThread.
func (m *Machine) syncThread(t *Thread) {
	var state uint8 = osStateNone
	switch t.State {
	case thReady, thSuspended:
		state = osStateReady
		if m.running == t {
			state = osStateRunning
		}
	case thWaiting:
		state = osStateWaiting
	case thDone:
		state = osStateMoribund
	}
	mem, a := m.Mem, t.Addr
	mem.Write8(a+osThreadState, state)
	mem.Write8(a+osThreadAttr, uint8(t.Attr))
	mem.Write32(a+osThreadSuspend, uint32(t.Suspend))
	mem.Write32(a+osThreadPriority, uint32(t.Priority))
	mem.Write32(a+osThreadBasePrio, uint32(t.BasePrio))
	mem.Write32(a+osThreadExit, t.Exit)
	mem.Write32(a+osThreadType, t.Type)
	mem.Write32(a+osContextAffinity, t.Affinity)
}

// retire is the end of a thread that was joined or ended detached: its
// OSThread can be made into a new thread or run again.
func (m *Machine) retire(t *Thread) {
	m.removeThread(t)
	t.State = thDormant
	t.WaitKey, t.WakeAt = 0, 0
	m.syncThread(t)
	m.Mem.Write16(t.Addr+osThreadID, 0x8000)
}

// updatePriority works out what t runs at: holding a spin lock lifts it to
// the top.
func (m *Machine) updatePriority(t *Thread) {
	t.Priority = t.BasePrio
	if t.Boost > 0 {
		t.Priority = min(t.Priority, 0)
	}
	m.syncThread(t)
}

// callDeallocator has t's deallocator(thread, stack) called on the core's
// callback thread, which runs above it.
func (m *Machine) callDeallocator(core int, t *Thread) {
	if fn := t.Dealloc; fn != 0 {
		t.Dealloc = 0
		m.queueCallback(core, fn, t.Addr, m.Mem.Read32(t.Addr+osThreadStackLo))
	}
}

// cppRuntimeHook is one of the functions the C++ runtime hands coreinit
// through __cpp_exception_init_ptr and __cpp_exception_cleanup_ptr.
func (m *Machine) cppRuntimeHook(name string) uint32 {
	if a, ok := m.dataAddr["coreinit."+name]; ok {
		return m.Mem.Read32(a)
	}
	return 0
}

// osExitThread ends the running thread t with v, after its cleanup callback
// and the C++ runtime's cleanup have run.
func (m *Machine) osExitThread(cpu *CPU, t *Thread, v uint32) {
	again := func(m *Machine, cpu *CPU, t *Thread, _ uint32) { m.osExitThread(cpu, t, v) }
	if cb := t.Cleanup; cb != 0 {
		t.Cleanup = 0
		m.callGuest(cpu, t, cb, []uint32{t.Addr, m.Mem.Read32(t.Addr + osThreadStackLo)}, again)
		return
	}
	eh := t.Addr + osThreadEHGlobals
	if fn := m.cppRuntimeHook("__cpp_exception_cleanup_ptr"); fn != 0 && m.Mem.Read32(eh) != 0 {
		m.callGuest(cpu, t, fn, []uint32{eh}, func(m *Machine, cpu *CPU, t *Thread, r uint32) {
			m.Mem.Write32(eh, 0)
			again(m, cpu, t, r)
		})
		return
	}
	m.finishThread(cpu.Core, t, v)
	t.yield()
}

// finishThread records that t ended with v. Its mutexes are freed and its
// joiners woken; a detached thread is retired at once (and its deallocator
// called), any other waits to be joined. The main thread ending ends the
// program.
func (m *Machine) finishThread(core int, t *Thread, v uint32) {
	t.Exit = v
	t.WaitKey, t.WakeAt = 0, 0
	t.IntsOff = false
	for _, mx := range m.heldMutexes(t) {
		m.releaseMutex(mx)
	}
	m.wake(keyJoin | uint64(t.Addr))
	if t.Attr&attrDetached == 0 {
		t.State = thDone
		m.syncThread(t)
	} else {
		m.retire(t)
		m.callDeallocator(core, t)
	}
	for c, cur := range m.current {
		if cur == t {
			m.current[c] = nil
		}
	}
	if t == m.mainThread {
		m.exitProgram(v)
	}
}

// resumeBy takes up to n suspensions off th, readying it when none are left;
// it returns the count there was.
func (m *Machine) resumeBy(th *Thread, n int) int {
	was := th.Suspend
	if was == 0 {
		return 0
	}
	th.Suspend = max(was-n, 0)
	if th.Suspend == 0 && th.State == thSuspended {
		th.State = thReady
		m.readied(th)
	}
	m.syncThread(th)
	return was
}

// withThread wraps a call whose first argument is a thread: for no thread
// it returns 0.
func withThread(fn func(m *Machine, cpu *CPU, t, th *Thread)) hleFunc {
	return func(m *Machine, cpu *CPU, t *Thread) {
		if th := m.threadAt(arg(cpu, 0)); th != nil {
			fn(m, cpu, t, th)
			return
		}
		ret(cpu, 0)
	}
}

func init() {
	c := "coreinit"
	regData(c, "__cpp_exception_init_ptr", 4, nil)
	regData(c, "__cpp_exception_cleanup_ptr", 4, nil)

	// OSCreateThread(thread, entry, argc, argv, stack top, stack size,
	// priority, attributes)
	create := func(m *Machine, cpu *CPU, t *Thread, typ uint32) {
		addr, entry, argc, argv := arg(cpu, 0), arg(cpu, 1), arg(cpu, 2), arg(cpu, 3)
		stack, size, prio, attr := arg(cpu, 4), arg(cpu, 5), int(int32(arg(cpu, 6))), arg(cpu, 7)&0xff
		if addr == 0 || stack == 0 || size == 0 || prio < 0 || prio > 31 || attr >= 0x20 {
			ret(cpu, 0)
			return
		}
		old := m.threadAt(addr)
		if old != nil && old != t && old.State == thReady && old.Suspend == 0 {
			// made again while it still runs: try again once it has stopped
			t.block(0, m.Time()+TimerHz/2000)
			return
		}
		if old != nil && old.State == thDone {
			m.retire(old)
		}
		nt := m.initThread(addr, entry, argc, argv, stack, size, prio, attr, typ)
		nt.Ctx.GPR[2], nt.Ctx.GPR[13] = cpu.GPR[2], cpu.GPR[13]
		ret(cpu, 1)
	}
	stackType := func(m *Machine, cpu *CPU) uint32 { return m.Mem.Read32(cpu.GPR[1] + 8) } // the 9th argument
	reg(c, "OSCreateThread", func(m *Machine, cpu *CPU, t *Thread) { create(m, cpu, t, threadApp) })
	reg(c, "OSCreateThreadType", func(m *Machine, cpu *CPU, t *Thread) {
		if typ := stackType(m, cpu); typ == threadApp || typ == threadIO {
			create(m, cpu, t, typ)
			return
		}
		ret(cpu, 0)
	})
	reg(c, "__OSCreateThreadType", func(m *Machine, cpu *CPU, t *Thread) { create(m, cpu, t, stackType(m, cpu)&3) })

	// every thread starts here: the C++ runtime sets up its exception state
	// and then the entry point runs
	reg(c, "__RPThreadStart", func(m *Machine, cpu *CPU, t *Thread) {
		entry := m.Mem.Read32(t.Addr + osThreadEntry)
		fn := m.cppRuntimeHook("__cpp_exception_init_ptr")
		if fn == 0 {
			m.jumpTo(cpu, entry)
			return
		}
		argc, argv := cpu.GPR[3], cpu.GPR[4]
		m.callGuest(cpu, t, fn, []uint32{t.Addr + osThreadEHGlobals}, func(m *Machine, cpu *CPU, t *Thread, _ uint32) {
			cpu.GPR[3], cpu.GPR[4] = argc, argv
			m.jumpTo(cpu, entry)
		})
	})

	reg(c, "OSResumeThread", withThread(func(m *Machine, cpu *CPU, t, th *Thread) {
		ret(cpu, uint32(m.resumeBy(th, 1)))
	}))
	reg(c, "OSContinueThread", func(m *Machine, cpu *CPU, t *Thread) {
		if th := m.threadAt(arg(cpu, 0)); th != nil {
			m.resumeBy(th, th.Suspend)
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

	// OSRunThread(thread, entry, argc, argv) starts a dormant or ended
	// thread again
	reg(c, "OSRunThread", withThread(func(m *Machine, cpu *CPU, t, th *Thread) {
		if th == t || th.State != thDormant && th.State != thDone {
			ret(cpu, 0)
			return
		}
		m.Mem.Write32(th.Addr+osThreadEntry, arg(cpu, 1))
		m.startContext(th, arg(cpu, 2), arg(cpu, 3))
		th.Ctx.GPR[2], th.Ctx.GPR[13] = cpu.GPR[2], cpu.GPR[13]
		th.Suspend, th.State = 0, thReady
		th.WaitKey, th.WakeAt = 0, 0
		th.doneWaiting()
		m.activate(th)
		m.syncThread(th)
		m.readied(th)
		ret(cpu, 1)
	}))
	reg(c, "OSExitThread", func(m *Machine, cpu *CPU, t *Thread) { m.osExitThread(cpu, t, arg(cpu, 0)) })

	// OSJoinThread(thread, exit value out) waits for a joinable thread to
	// end, then retires it
	reg(c, "OSJoinThread", withThread(func(m *Machine, cpu *CPU, t, th *Thread) {
		switch {
		case th == t:
			ret(cpu, 0)
			return
		case th.State != thDone && th.Attr&attrDetached == 0:
			t.block(keyJoin|uint64(th.Addr), 0)
			return
		case th.State != thDone:
			ret(cpu, 0) // detached and still going
			return
		}
		if out := arg(cpu, 1); out != 0 {
			m.Mem.Write32(out, th.Exit)
		}
		ret(cpu, 1)
		m.retire(th)
		if th.Dealloc != 0 {
			m.callDeallocator(cpu.Core, th)
			m.preempt(cpu) // it runs before the join returns
		}
	}))
	reg(c, "OSDetachThread", func(m *Machine, cpu *CPU, t *Thread) {
		th := m.threadAt(arg(cpu, 0))
		if th == nil {
			return
		}
		th.Attr |= attrDetached
		m.syncThread(th)
		if th.State == thDone {
			m.retire(th)
			m.wake(keyJoin | uint64(th.Addr))
			m.callDeallocator(cpu.Core, th)
		}
	})
	reg(c, "OSCancelThread", func(m *Machine, cpu *CPU, t *Thread) {
		if th := m.threadAt(arg(cpu, 0)); th != nil {
			th.Cancel = true
			if th == t {
				m.osExitThread(cpu, t, threadNoExit)
			}
		}
	})
	reg(c, "OSTestThreadCancel", func(m *Machine, cpu *CPU, t *Thread) {
		if t != nil && t.Cancel {
			m.osExitThread(cpu, t, threadNoExit)
		}
	})

	// about threads
	reg(c, "OSGetCurrentThread", func(m *Machine, cpu *CPU, t *Thread) {
		var a uint32
		if t != nil {
			a = t.Addr
		}
		ret(cpu, a)
	})
	reg(c, "OSGetDefaultThread", func(m *Machine, cpu *CPU, t *Thread) {
		var a uint32
		if core := arg(cpu, 0); core <= 2 {
			a = m.defaultTh[core].Addr
		}
		ret(cpu, a)
	})
	reg(c, "OSSetThreadName", func(m *Machine, cpu *CPU, t *Thread) { m.Mem.Write32(arg(cpu, 0)+osThreadName, arg(cpu, 1)) })
	reg(c, "OSGetThreadName", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, m.Mem.Read32(arg(cpu, 0)+osThreadName)) })
	reg(c, "OSSetThreadPriority", withThread(func(m *Machine, cpu *CPU, t, th *Thread) {
		p := int(int32(arg(cpu, 1)))
		if p < 0 || p > 31 {
			ret(cpu, 0)
			return
		}
		th.BasePrio = typePriorityBase(th.Type) + p
		m.updatePriority(th)
		if th != t {
			m.readied(th)
		}
		ret(cpu, 1)
	}))
	reg(c, "OSGetThreadPriority", withThread(func(m *Machine, cpu *CPU, t, th *Thread) {
		ret(cpu, uint32(th.BasePrio-typePriorityBase(th.Type)))
	}))
	reg(c, "OSSetThreadAffinity", withThread(func(m *Machine, cpu *CPU, t, th *Thread) {
		cores := arg(cpu, 1) & attrAffinityAny
		th.Affinity = cores
		th.Attr = th.Attr&^attrAffinityAny | cores
		m.syncThread(th)
		ret(cpu, 1)
		switch {
		case th != t:
			m.readied(th)
		case cores&(1<<cpu.Core) == 0:
			m.preempt(cpu) // it may no longer run here
		}
	}))
	reg(c, "OSGetThreadAffinity", withThread(func(m *Machine, cpu *CPU, t, th *Thread) { ret(cpu, th.Affinity) }))
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
		on := false
		if th != nil && th.State == thReady {
			for _, cur := range m.current {
				on = on || cur == th
			}
		}
		retBool(cpu, on)
	})
	reg(c, "OSCheckActiveThreads", func(m *Machine, cpu *CPU, t *Thread) {
		var n uint32
		for _, th := range m.threadList {
			if th.State != thDormant {
				n++
			}
		}
		ret(cpu, n)
	})
	reg(c, "OSGetStackPointer", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, cpu.GPR[1]) })
	reg(c, "OSSetThreadCancelState", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 1) })
	reg(c, "OSSetThreadStackUsage", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 1) })
	reg(c, "OSCheckThreadStackUsage", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0x1000) })

	// per-thread values and callbacks
	reg(c, "OSSetThreadSpecific", func(m *Machine, cpu *CPU, t *Thread) {
		if i := arg(cpu, 0); i < 16 && t != nil {
			m.Mem.Write32(t.Addr+osThreadSpecific+i*4, arg(cpu, 1))
		}
	})
	reg(c, "OSGetThreadSpecific", func(m *Machine, cpu *CPU, t *Thread) {
		var v uint32
		if i := arg(cpu, 0); i < 16 && t != nil {
			v = m.Mem.Read32(t.Addr + osThreadSpecific + i*4)
		}
		ret(cpu, v)
	})
	reg(c, "OSSetThreadCleanupCallback", withThread(func(m *Machine, cpu *CPU, t, th *Thread) {
		old := th.Cleanup
		th.Cleanup = arg(cpu, 1)
		ret(cpu, old)
	}))
	reg(c, "OSSetThreadDeallocator", withThread(func(m *Machine, cpu *CPU, t, th *Thread) {
		old := th.Dealloc
		th.Dealloc = arg(cpu, 1)
		ret(cpu, old)
	}))

	// giving up the core
	reg(c, "OSYieldThread", func(m *Machine, cpu *CPU, t *Thread) { m.preempt(cpu) })
	reg(c, "OSSleepTicks", func(m *Machine, cpu *CPU, t *Thread) {
		if t.Phase == 1 {
			return // slept
		}
		ticks := arg64(cpu, 0)
		if int64(ticks) <= 0 {
			m.preempt(cpu)
			return
		}
		t.block(0, m.Time()+ticks)
		t.Phase = 1
	})

	// thread queues: OSSleepThread waits on one, OSWakeupThread wakes all
	// its threads
	initQueue := func(m *Machine, q, userData uint32) {
		m.Mem.Fill(q, 0, 0x10)
		m.Mem.Write32(q+8, userData)
	}
	reg(c, "OSInitThreadQueue", func(m *Machine, cpu *CPU, t *Thread) { initQueue(m, arg(cpu, 0), 0) })
	reg(c, "OSInitThreadQueueEx", func(m *Machine, cpu *CPU, t *Thread) { initQueue(m, arg(cpu, 0), arg(cpu, 1)) })
	reg(c, "OSSleepThread", func(m *Machine, cpu *CPU, t *Thread) {
		if !t.Woken {
			t.block(keyQueue|uint64(arg(cpu, 0)), 0)
		}
	})
	reg(c, "OSWakeupThread", func(m *Machine, cpu *CPU, t *Thread) { m.wake(keyQueue | uint64(arg(cpu, 0))) })

	// Interrupts. While a thread has them off no other thread, callbacks
	// included, takes its core. Each call returns whether they were on.
	interrupts := func(on func(cpu *CPU) bool) hleFunc {
		return func(m *Machine, cpu *CPU, t *Thread) {
			if t == nil {
				ret(cpu, 0)
				return
			}
			wasOn := !t.IntsOff
			t.IntsOff = !on(cpu)
			retBool(cpu, wasOn)
		}
	}
	reg(c, "OSDisableInterrupts", interrupts(func(*CPU) bool { return false }))
	reg(c, "OSEnableInterrupts", interrupts(func(*CPU) bool { return true }))
	reg(c, "OSRestoreInterrupts", interrupts(func(cpu *CPU) bool { return arg(cpu, 0) != 0 }))
	reg(c, "OSIsInterruptEnabled", func(m *Machine, cpu *CPU, t *Thread) { retBool(cpu, t != nil && !t.IntsOff) })
}
