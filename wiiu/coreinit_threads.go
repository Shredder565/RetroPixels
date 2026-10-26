// Original code for RetroPixels. Not derived from Cemu.

package wiiu

// coreinit threads: creating, starting, suspending, ending and joining
// them; priorities, core affinity, thread-specific values, thread queues
// and the per-thread interrupt switch.
//
// The scheduler state of a thread lives in its *Thread (kernel.go); the
// program's OSThread structure mirrors the parts programs look at, and
// syncThread refreshes that mirror whenever they change.

// OSThread offsets (the structure starts with its OSContext)
const (
	osThreadTag       = 0x320 // "tHrD"
	osThreadState     = 0x324 // u8
	osThreadAttr      = 0x325 // u8
	osThreadID        = 0x326 // u16
	osThreadSuspend   = 0x328
	osThreadPriority  = 0x32c // effective priority
	osThreadBasePrio  = 0x330
	osThreadExit      = 0x334
	osThreadStackHi   = 0x394 // top of the stack (it grows down)
	osThreadStackLo   = 0x398
	osThreadEntry     = 0x39c
	osThreadEHGlobals = 0x408 // C++ exception handling state
	osThreadSpecific  = 0x57c // 16 words for OSSet/GetThreadSpecific
	osThreadType      = 0x5bc
	osThreadName      = 0x5c0
	osThreadSize      = 0x6a0

	osContextAffinity = 0x304

	osThreadSpecifics = 16
)

// OSThreadState values
const (
	osStateNone     = 0
	osStateReady    = 1
	osStateRunning  = 2
	osStateWaiting  = 4
	osStateMoribund = 8
)

// thread types; each type's 0..31 priorities sit in their own band, so an
// I/O thread always outranks an application thread
const (
	threadDriver = 0
	threadIO     = 1
	threadApp    = 2
)

func typePriorityBase(typ uint32) int {
	switch typ {
	case threadApp:
		return 64
	case threadIO:
		return 32
	}
	return 0
}

const exitCancelled = 0xffffffff // a cancelled thread's exit value

// initThread makes a thread for the OSThread at addr, replacing any thread
// that used it before. With an entry point it starts suspended once (so
// OSResumeThread starts it); without, it is dormant until OSRunThread.
func (m *Machine) initThread(addr, entry, argc, argv, stackTop, stackSize uint32, prio int, attr, typ uint32) *Thread {
	if prev := m.threads[addr]; prev != nil {
		m.removeThread(prev)
	}
	delete(m.conts, addr)
	m.nextThreadID++

	t := &Thread{ID: m.nextThreadID, Addr: addr, Attr: attr, Type: typ, Core: -1, Exit: exitCancelled}
	t.BasePrio = prio + typePriorityBase(typ)
	t.Priority = t.BasePrio
	t.Affinity = attr & attrAffinityAny
	if t.Affinity == 0 {
		// no core given: the creator's core (core 1 when there's none)
		core := 1
		if r := m.running; r != nil && r.Core >= 0 {
			core = r.Core
		}
		t.Affinity = 1 << core
		t.Attr = attr&^(attrAffinityAny|0x10) | t.Affinity
	}
	if entry != 0 {
		t.State, t.Suspend = thSuspended, 1
	} else {
		t.State = thDormant
	}

	mem := m.Mem
	mem.Fill(addr, 0, osThreadSize)
	mem.Write32(addr+0, 0x4f53436f) // "OSCo"
	mem.Write32(addr+4, 0x6e747874) // "ntxt"
	mem.Write32(addr+osThreadTag, 0x74487244)
	mem.Write16(addr+osThreadID, uint16(t.ID))
	mem.Write32(addr+osThreadStackHi, stackTop)
	mem.Write32(addr+osThreadStackLo, stackTop-stackSize)
	mem.Write32(addr+osThreadEntry, entry)
	if stackSize != 0 {
		mem.Write32(stackTop-stackSize, 0xdeadbabe) // overflow canary
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

// startContext gives t the registers it starts with: r3/r4 = argc/argv, a
// fresh 8-byte-aligned stack frame, the start thunk as PC and OSExitThread
// as the return address, and the standard quantisation registers.
func (m *Machine) startContext(t *Thread, argc, argv uint32) {
	sp := m.Mem.Read32(t.Addr+osThreadStackHi) &^ 7
	m.Mem.Write32(sp-8, 0)
	m.Mem.Write32(sp-4, 0)
	t.Ctx = Context{}
	t.Ctx.GPR[1] = sp - 8
	t.Ctx.GPR[3] = argc
	t.Ctx.GPR[4] = argv
	t.Ctx.PC = m.thunk("coreinit", "__RPThreadStart")
	t.Ctx.LR = m.thunk("coreinit", "OSExitThread")
	for i := 2; i <= 5; i++ {
		t.Ctx.GQR[i] = uint32(i+2) * 0x10001 // load and store type i+2 (scale 0)
	}
}

// osStateOf is how the program sees t's state.
func (m *Machine) osStateOf(t *Thread) uint8 {
	switch t.State {
	case thReady, thSuspended:
		if m.running == t {
			return osStateRunning
		}
		return osStateReady
	case thWaiting:
		return osStateWaiting
	case thDone:
		return osStateMoribund
	}
	return osStateNone
}

// syncThread copies t's scheduler state into its OSThread.
func (m *Machine) syncThread(t *Thread) {
	mem, a := m.Mem, t.Addr
	mem.Write8(a+osThreadState, m.osStateOf(t))
	mem.Write8(a+osThreadAttr, uint8(t.Attr))
	mem.Write32(a+osThreadSuspend, uint32(t.Suspend))
	mem.Write32(a+osThreadPriority, uint32(t.Priority))
	mem.Write32(a+osThreadBasePrio, uint32(t.BasePrio))
	mem.Write32(a+osThreadExit, t.Exit)
	mem.Write32(a+osThreadType, t.Type)
	mem.Write32(a+osContextAffinity, t.Affinity)
}

// deactivate retires a thread that has been joined (or that ended while
// detached); its OSThread may then be reused.
func (m *Machine) deactivate(t *Thread) {
	m.removeThread(t)
	t.State = thDormant
	t.WaitKey, t.WakeAt = 0, 0
	m.syncThread(t)
	m.Mem.Write16(t.Addr+osThreadID, 0x8000)
}

// updatePriority recomputes the priority t runs at: its base priority, or
// the very top while it holds a spin lock.
func (m *Machine) updatePriority(t *Thread) {
	t.Priority = t.BasePrio
	if t.Boost > 0 {
		t.Priority = min(t.Priority, 0)
	}
	m.syncThread(t)
}

// queueDealloc schedules t's deallocator(thread, stack bottom), once, on
// the core's callback thread.
func (m *Machine) queueDealloc(core int, t *Thread) {
	fn := t.Dealloc
	if fn == 0 {
		return
	}
	t.Dealloc = 0
	m.queueCallback(core, fn, t.Addr, m.Mem.Read32(t.Addr+osThreadStackLo))
}

// cppExceptionPtr reads one of the function pointers the C++ runtime
// registers with coreinit (__cpp_exception_init_ptr, ..._cleanup_ptr).
func (m *Machine) cppExceptionPtr(name string) uint32 {
	a, ok := m.dataAddr["coreinit."+name]
	if !ok {
		return 0
	}
	return m.Mem.Read32(a)
}

// osExitThread is OSExitThread for the running thread t: first its cleanup
// callback, then the C++ runtime's exception cleanup, then endThread. Each
// guest call comes back here to do the next step.
func (m *Machine) osExitThread(cpu *CPU, t *Thread, v uint32) {
	again := func(m *Machine, cpu *CPU, t *Thread, _ uint32) { m.osExitThread(cpu, t, v) }
	if cb := t.Cleanup; cb != 0 {
		t.Cleanup = 0
		m.callGuest(cpu, t, cb, []uint32{t.Addr, m.Mem.Read32(t.Addr + osThreadStackLo)}, again)
		return
	}
	eh := t.Addr + osThreadEHGlobals
	if fn := m.cppExceptionPtr("__cpp_exception_cleanup_ptr"); fn != 0 && m.Mem.Read32(eh) != 0 {
		m.callGuest(cpu, t, fn, []uint32{eh}, func(m *Machine, cpu *CPU, t *Thread, r uint32) {
			m.Mem.Write32(eh, 0)
			again(m, cpu, t, r)
		})
		return
	}
	m.endThread(cpu.Core, t, v)
	t.yield()
}

// endThread finishes t with exit value v: it releases t's mutexes and wakes
// its joiners. A detached thread is retired at once (its deallocator runs);
// any other stays moribund until joined. The main thread ending ends the
// program.
func (m *Machine) endThread(core int, t *Thread, v uint32) {
	t.Exit = v
	t.WaitKey, t.WakeAt = 0, 0
	t.IntsOff = false
	for _, mx := range m.heldMutexes(t) {
		m.Mem.Write32(mx+mutexCount, 0)
		m.releaseMutex(mx)
	}
	m.wake(keyJoin | uint64(t.Addr))
	if t.Attr&attrDetached == 0 {
		t.State = thDone
		m.syncThread(t)
	} else {
		m.deactivate(t)
		m.queueDealloc(core, t)
	}
	for i, cur := range m.current {
		if cur == t {
			m.current[i] = nil
		}
	}
	if t == m.mainThread {
		m.exitProgram(v)
	}
}

// resumeBy lowers t's suspend count by n (not below 0) and returns the
// count it had; a thread whose count reaches 0 becomes ready.
func (m *Machine) resumeBy(t *Thread, n int) int {
	was := t.Suspend
	if was == 0 {
		return 0
	}
	t.Suspend = max(was-n, 0)
	if t.Suspend == 0 && t.State == thSuspended {
		t.State = thReady
		m.readied(t)
	}
	m.syncThread(t)
	return was
}

func init() {
	const c = "coreinit"

	regData(c, "__cpp_exception_init_ptr", 4, nil)
	regData(c, "__cpp_exception_cleanup_ptr", 4, nil)

	// OSCreateThread(thread, entry, argc, argv, stackTop, stackSize,
	// priority, attributes[, type on the stack])
	create := func(m *Machine, cpu *CPU, t *Thread, typ uint32) {
		addr, entry, argc, argv := arg(cpu, 0), arg(cpu, 1), arg(cpu, 2), arg(cpu, 3)
		stack, size := arg(cpu, 4), arg(cpu, 5)
		prio, attr := int(int32(arg(cpu, 6))), arg(cpu, 7)&0xff
		if addr == 0 || stack == 0 || size == 0 || prio < 0 || prio > 31 || attr >= 0x20 {
			ret(cpu, 0)
			return
		}
		prev := m.threadAt(addr)
		if prev != nil && prev != t && prev.State == thReady && prev.Suspend == 0 {
			// the OSThread is being re-created while its thread still
			// runs: give it half a millisecond to stop, then try again
			t.block(0, m.Time()+TimerHz/2000)
			return
		}
		if prev != nil && prev.State == thDone {
			m.deactivate(prev)
		}
		nt := m.initThread(addr, entry, argc, argv, stack, size, prio, attr, typ)
		nt.Ctx.GPR[2], nt.Ctx.GPR[13] = cpu.GPR[2], cpu.GPR[13]
		ret(cpu, 1)
	}
	stackedType := func(m *Machine, cpu *CPU) uint32 { return m.Mem.Read32(cpu.GPR[1] + 8) }
	reg(c, "OSCreateThread", func(m *Machine, cpu *CPU, t *Thread) { create(m, cpu, t, threadApp) })
	reg(c, "OSCreateThreadType", func(m *Machine, cpu *CPU, t *Thread) {
		switch typ := stackedType(m, cpu); typ {
		case threadApp, threadIO:
			create(m, cpu, t, typ)
		default:
			ret(cpu, 0)
		}
	})
	reg(c, "__OSCreateThreadType", func(m *Machine, cpu *CPU, t *Thread) { create(m, cpu, t, stackedType(m, cpu)&3) })

	// Every thread begins here: the C++ runtime initialises the thread's
	// exception state (if it registered a hook), then the entry point runs
	// with the original r3/r4.
	reg(c, "__RPThreadStart", func(m *Machine, cpu *CPU, t *Thread) {
		entry := m.Mem.Read32(t.Addr + osThreadEntry)
		argc, argv := cpu.GPR[3], cpu.GPR[4]
		enter := func(m *Machine, cpu *CPU, t *Thread, _ uint32) {
			cpu.GPR[3], cpu.GPR[4] = argc, argv
			cpu.PC = entry
			m.jumped = true
		}
		if hook := m.cppExceptionPtr("__cpp_exception_init_ptr"); hook != 0 {
			m.callGuest(cpu, t, hook, []uint32{t.Addr + osThreadEHGlobals}, enter)
			return
		}
		enter(m, cpu, t, 0)
	})

	// ---- suspend / resume / run ----
	reg(c, "OSResumeThread", func(m *Machine, cpu *CPU, t *Thread) {
		th := m.threadAt(arg(cpu, 0))
		if th == nil {
			ret(cpu, 0)
			return
		}
		ret(cpu, uint32(m.resumeBy(th, 1)))
	})
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
	// OSRunThread(thread, entry, argc, argv): restarts a dormant or ended
	// thread at entry
	reg(c, "OSRunThread", func(m *Machine, cpu *CPU, t *Thread) {
		th := m.threadAt(arg(cpu, 0))
		if th == nil || th == t || th.State != thDormant && th.State != thDone {
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

	// ---- ending and joining ----
	reg(c, "OSExitThread", func(m *Machine, cpu *CPU, t *Thread) { m.osExitThread(cpu, t, arg(cpu, 0)) })
	// OSJoinThread(thread, s32 *exitValue)
	reg(c, "OSJoinThread", func(m *Machine, cpu *CPU, t *Thread) {
		th := m.threadAt(arg(cpu, 0))
		if th == nil || th == t {
			ret(cpu, 0)
			return
		}
		ended := th.State == thDone
		if !ended {
			if th.Attr&attrDetached != 0 {
				ret(cpu, 0) // a detached thread can't be joined
			} else {
				t.block(keyJoin|uint64(th.Addr), 0)
			}
			return
		}
		if out := arg(cpu, 1); out != 0 {
			m.Mem.Write32(out, th.Exit)
		}
		ret(cpu, 1)
		m.deactivate(th)
		if th.Dealloc != 0 {
			// let the deallocator run before the joiner goes on
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
		if th.State != thDone {
			return
		}
		m.deactivate(th)
		m.wake(keyJoin | uint64(th.Addr))
		m.queueDealloc(cpu.Core, th)
	})
	cancelled := func(m *Machine, cpu *CPU, t *Thread) { m.osExitThread(cpu, t, exitCancelled) }
	reg(c, "OSCancelThread", func(m *Machine, cpu *CPU, t *Thread) {
		th := m.threadAt(arg(cpu, 0))
		if th == nil {
			return
		}
		th.Cancel = true
		if th == t {
			cancelled(m, cpu, t)
		}
	})
	reg(c, "OSTestThreadCancel", func(m *Machine, cpu *CPU, t *Thread) {
		if t != nil && t.Cancel {
			cancelled(m, cpu, t)
		}
	})
	reg(c, "OSSetThreadCancelState", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 1) })

	// ---- queries and attributes ----
	reg(c, "OSGetCurrentThread", func(m *Machine, cpu *CPU, t *Thread) {
		var a uint32
		if t != nil {
			a = t.Addr
		}
		ret(cpu, a)
	})
	reg(c, "OSGetDefaultThread", func(m *Machine, cpu *CPU, t *Thread) {
		if core := arg(cpu, 0); core < 3 {
			ret(cpu, m.defaultTh[core].Addr)
			return
		}
		ret(cpu, 0)
	})
	reg(c, "OSSetThreadName", func(m *Machine, cpu *CPU, t *Thread) {
		m.Mem.Write32(arg(cpu, 0)+osThreadName, arg(cpu, 1))
	})
	reg(c, "OSGetThreadName", func(m *Machine, cpu *CPU, t *Thread) {
		ret(cpu, m.Mem.Read32(arg(cpu, 0)+osThreadName))
	})
	reg(c, "OSSetThreadPriority", func(m *Machine, cpu *CPU, t *Thread) {
		th, p := m.threadAt(arg(cpu, 0)), int(int32(arg(cpu, 1)))
		if th == nil || p < 0 || p > 31 {
			ret(cpu, 0)
			return
		}
		th.BasePrio = typePriorityBase(th.Type) + p
		m.updatePriority(th)
		if th != t {
			m.readied(th)
		}
		ret(cpu, 1)
	})
	reg(c, "OSGetThreadPriority", func(m *Machine, cpu *CPU, t *Thread) {
		th := m.threadAt(arg(cpu, 0))
		if th == nil {
			ret(cpu, 0)
			return
		}
		ret(cpu, uint32(th.BasePrio-typePriorityBase(th.Type)))
	})
	reg(c, "OSSetThreadAffinity", func(m *Machine, cpu *CPU, t *Thread) {
		th := m.threadAt(arg(cpu, 0))
		if th == nil {
			ret(cpu, 0)
			return
		}
		cores := arg(cpu, 1) & attrAffinityAny
		th.Affinity = cores
		th.Attr = th.Attr&^attrAffinityAny | cores
		m.syncThread(th)
		ret(cpu, 1)
		switch {
		case th != t:
			m.readied(th)
		case cores>>cpu.Core&1 == 0:
			m.preempt(cpu) // the caller may no longer run on this core
		}
	})
	reg(c, "OSGetThreadAffinity", func(m *Machine, cpu *CPU, t *Thread) {
		var cores uint32
		if th := m.threadAt(arg(cpu, 0)); th != nil {
			cores = th.Affinity
		}
		ret(cpu, cores)
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
		if th != nil && th.State == thReady {
			for _, cur := range m.current {
				running = running || cur == th
			}
		}
		retBool(cpu, running)
	})
	reg(c, "OSSetThreadSpecific", func(m *Machine, cpu *CPU, t *Thread) {
		if i := arg(cpu, 0); t != nil && i < osThreadSpecifics {
			m.Mem.Write32(t.Addr+osThreadSpecific+4*i, arg(cpu, 1))
		}
	})
	reg(c, "OSGetThreadSpecific", func(m *Machine, cpu *CPU, t *Thread) {
		var v uint32
		if i := arg(cpu, 0); t != nil && i < osThreadSpecifics {
			v = m.Mem.Read32(t.Addr + osThreadSpecific + 4*i)
		}
		ret(cpu, v)
	})
	// the two callback setters return the previous callback
	setCallback := func(field func(th *Thread) *uint32) hleFunc {
		return func(m *Machine, cpu *CPU, t *Thread) {
			th := m.threadAt(arg(cpu, 0))
			if th == nil {
				ret(cpu, 0)
				return
			}
			p := field(th)
			ret(cpu, *p)
			*p = arg(cpu, 1)
		}
	}
	reg(c, "OSSetThreadCleanupCallback", setCallback(func(th *Thread) *uint32 { return &th.Cleanup }))
	reg(c, "OSSetThreadDeallocator", setCallback(func(th *Thread) *uint32 { return &th.Dealloc }))
	reg(c, "OSGetStackPointer", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, cpu.GPR[1]) })
	reg(c, "OSSetThreadStackUsage", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 1) })
	reg(c, "OSCheckThreadStackUsage", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0x1000) })
	reg(c, "OSCheckActiveThreads", func(m *Machine, cpu *CPU, t *Thread) {
		var n uint32
		for _, th := range m.threadList {
			if th.State != thDormant {
				n++
			}
		}
		ret(cpu, n)
	})
	// OSSleepTicks(u64 ticks)
	reg(c, "OSSleepTicks", func(m *Machine, cpu *CPU, t *Thread) {
		if t.Phase == 1 {
			return // slept
		}
		ticks := arg64(cpu, 0)
		if int64(ticks) <= 0 {
			m.preempt(cpu)
			return
		}
		t.Phase = 1
		t.block(0, m.Time()+ticks)
	})

	// ---- thread queues: { head, tail, parent, unused } ----
	reg(c, "OSInitThreadQueue", func(m *Machine, cpu *CPU, t *Thread) { m.Mem.Fill(arg(cpu, 0), 0, 0x10) })
	reg(c, "OSInitThreadQueueEx", func(m *Machine, cpu *CPU, t *Thread) {
		q := arg(cpu, 0)
		m.Mem.Fill(q, 0, 0x10)
		m.Mem.Write32(q+8, arg(cpu, 1))
	})
	reg(c, "OSSleepThread", func(m *Machine, cpu *CPU, t *Thread) {
		if !t.Woken {
			t.block(keyQueue|uint64(arg(cpu, 0)), 0)
		}
	})
	reg(c, "OSWakeupThread", func(m *Machine, cpu *CPU, t *Thread) { m.wake(keyQueue | uint64(arg(cpu, 0))) })

	// ---- interrupts: a thread with them off keeps its core (no other
	// thread, callbacks included, is switched in) ----
	switchInts := func(cpu *CPU, t *Thread, on bool) {
		if t == nil {
			ret(cpu, 0)
			return
		}
		wasOn := !t.IntsOff
		t.IntsOff = !on
		retBool(cpu, wasOn)
	}
	reg(c, "OSDisableInterrupts", func(m *Machine, cpu *CPU, t *Thread) { switchInts(cpu, t, false) })
	reg(c, "OSEnableInterrupts", func(m *Machine, cpu *CPU, t *Thread) { switchInts(cpu, t, true) })
	reg(c, "OSRestoreInterrupts", func(m *Machine, cpu *CPU, t *Thread) { switchInts(cpu, t, arg(cpu, 0) != 0) })
	reg(c, "OSIsInterruptEnabled", func(m *Machine, cpu *CPU, t *Thread) { retBool(cpu, t != nil && !t.IntsOff) })
}
