package wiiu

// coreinit: threads, mutexes, conditions, semaphores, events, message
// queues, spin locks, rendezvous and alarms.

import "math"

// fields of the program's synchronisation objects
const (
	mutexTag   = 0x6d557458 // "mUtX"
	mutexOwner = 0x1c
	mutexCount = 0x20

	condTag   = 0x634f6e44 // "cOnD"
	semTag    = 0x73506d48 // "sMpH"
	semCount  = 0x0c
	eventTag  = 0x65566e54 // "eVnT"
	eventVal  = 0x0c
	eventMode = 0x20
	mqTag     = 0x6d536751 // "mSgQ"
	mqMsgs    = 0x2c
	mqSize    = 0x30
	mqFirst   = 0x34
	mqUsed    = 0x38

	fmOwner = 0x0c // OSFastMutex (RetroPixels' own layout inside it)
	fmCount = 0x10
)

func nsToTicks(ns uint64) uint64 { return ns * (TimerHz / 1000000) / 1000 }

func init() {
	c := "coreinit"

	// threads
	create := func(m *Machine, cpu *CPU, t *Thread) {
		addr, entry, argc, argv := arg(cpu, 0), arg(cpu, 1), arg(cpu, 2), arg(cpu, 3)
		stack, size, prio, attr := arg(cpu, 4), arg(cpu, 5), int(int32(arg(cpu, 6))), arg(cpu, 7)&0xff
		if addr == 0 || stack == 0 || prio < 0 || prio > 31 {
			ret(cpu, 0)
			return
		}
		nt := m.newThread(addr, entry, argc, argv, stack, size, prio, attr)
		nt.Ctx.GPR[2], nt.Ctx.GPR[13] = cpu.GPR[2], cpu.GPR[13]
		ret(cpu, 1)
	}
	reg(c, "OSCreateThread", create)
	reg(c, "OSCreateThreadType", create)
	reg(c, "OSResumeThread", func(m *Machine, cpu *CPU, t *Thread) {
		th := m.threadAt(arg(cpu, 0))
		if th == nil {
			ret(cpu, 0)
			return
		}
		ret(cpu, uint32(th.Suspend))
		if th.Suspend > 0 {
			th.Suspend--
			if th.Suspend == 0 && th.State == thSuspended {
				th.State = thReady
			}
		}
		m.syncThread(th)
		if th != t && th.State == thReady && t != nil && th.Priority < t.Priority {
			m.preempt(cpu) // a higher-priority thread starts now
		}
	})
	reg(c, "OSSuspendThread", func(m *Machine, cpu *CPU, t *Thread) {
		th := m.threadAt(arg(cpu, 0))
		if th == nil {
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
	})
	reg(c, "OSContinueThread", func(m *Machine, cpu *CPU, t *Thread) {
		th := m.threadAt(arg(cpu, 0))
		if th == nil {
			ret(cpu, 0)
			return
		}
		th.Suspend = 0
		if th.State == thSuspended {
			th.State = thReady
		}
		m.syncThread(th)
		ret(cpu, 1)
	})
	reg(c, "OSRunThread", func(m *Machine, cpu *CPU, t *Thread) {
		th := m.threadAt(arg(cpu, 0))
		if th == nil || (th.State != thDormant && th.State != thDone) {
			ret(cpu, 0)
			return
		}
		stackHi := m.Mem.Read32(th.Addr + osThreadStackHi)
		th.Ctx = Context{}
		th.Ctx.PC = arg(cpu, 1)
		th.Ctx.GPR[3], th.Ctx.GPR[4] = arg(cpu, 2), arg(cpu, 3)
		th.Ctx.GPR[1] = (stackHi - 8) &^ 7
		th.Ctx.GPR[2], th.Ctx.GPR[13] = cpu.GPR[2], cpu.GPR[13]
		th.Ctx.LR = m.thunk("coreinit", "__RPThreadReturn")
		th.State = thReady
		th.Suspend = 0
		m.syncThread(th)
		ret(cpu, 1)
	})
	reg(c, "OSExitThread", func(m *Machine, cpu *CPU, t *Thread) {
		m.threadExit(cpu, t, arg(cpu, 0))
	})
	reg(c, "__RPThreadExit2", func(m *Machine, cpu *CPU, t *Thread) {
		m.threadExit(cpu, t, t.Exit)
	})
	reg(c, "OSJoinThread", func(m *Machine, cpu *CPU, t *Thread) {
		th := m.threadAt(arg(cpu, 0))
		if th == nil || th.Attr&attrDetached != 0 || th == t {
			ret(cpu, 0)
			return
		}
		if th.State != thDone {
			t.block(keyJoin|uint64(th.Addr), 0)
			return
		}
		if p := arg(cpu, 1); p != 0 {
			m.Mem.Write32(p, th.Exit)
		}
		ret(cpu, 1)
		if th.Dealloc == 0 {
			m.removeThread(th)
		}
	})
	reg(c, "OSDetachThread", func(m *Machine, cpu *CPU, t *Thread) {
		if th := m.threadAt(arg(cpu, 0)); th != nil {
			th.Attr |= attrDetached
			if th.State == thDone {
				m.removeThread(th)
			}
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
		if th := m.threadAt(arg(cpu, 0)); th != nil {
			th.Name = arg(cpu, 1)
			m.syncThread(th)
		}
	})
	reg(c, "OSGetThreadName", func(m *Machine, cpu *CPU, t *Thread) {
		if th := m.threadAt(arg(cpu, 0)); th != nil {
			ret(cpu, th.Name)
			return
		}
		ret(cpu, 0)
	})
	reg(c, "OSSetThreadPriority", func(m *Machine, cpu *CPU, t *Thread) {
		th := m.threadAt(arg(cpu, 0))
		p := int(int32(arg(cpu, 1)))
		if th == nil || p < 0 || p > 31 {
			ret(cpu, 0)
			return
		}
		th.Priority = p
		m.syncThread(th)
		ret(cpu, 1)
	})
	reg(c, "OSGetThreadPriority", func(m *Machine, cpu *CPU, t *Thread) {
		if th := m.threadAt(arg(cpu, 0)); th != nil {
			ret(cpu, uint32(th.Priority))
			return
		}
		ret(cpu, 0)
	})
	reg(c, "OSSetThreadAffinity", func(m *Machine, cpu *CPU, t *Thread) {
		th := m.threadAt(arg(cpu, 0))
		a := arg(cpu, 1) & attrAffinityAny
		if th == nil || a == 0 {
			ret(cpu, 0)
			return
		}
		th.Affinity = a
		th.Attr = th.Attr&^attrAffinityAny | a
		ret(cpu, 1)
		if th == t && a&(1<<cpu.Core) == 0 {
			m.preempt(cpu)
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
	reg(c, "OSSetThreadSpecific", func(m *Machine, cpu *CPU, t *Thread) {
		if i := arg(cpu, 0); i < 16 && t != nil {
			t.Specific[i] = arg(cpu, 1)
			m.Mem.Write32(t.Addr+osThreadSpecific+i*4, arg(cpu, 1))
		}
	})
	reg(c, "OSGetThreadSpecific", func(m *Machine, cpu *CPU, t *Thread) {
		if i := arg(cpu, 0); i < 16 && t != nil {
			ret(cpu, t.Specific[i])
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
		if th := m.threadAt(arg(cpu, 0)); th != nil {
			th.Cancel = true
		}
	})
	reg(c, "OSTestThreadCancel", func(m *Machine, cpu *CPU, t *Thread) {
		if t != nil && t.Cancel {
			m.threadExit(cpu, t, 0xffffffff)
		}
	})
	reg(c, "OSSetThreadCancelState", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 1) })
	reg(c, "OSSetThreadStackUsage", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 1) })
	reg(c, "OSCheckThreadStackUsage", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0x1000) })
	reg(c, "OSCheckActiveThreads", func(m *Machine, cpu *CPU, t *Thread) {
		n := 0
		for _, th := range m.threadList {
			if th.State != thDone && th.State != thDormant {
				n++
			}
		}
		ret(cpu, uint32(n))
	})
	reg(c, "OSSleepTicks", func(m *Machine, cpu *CPU, t *Thread) {
		if t.Phase == 1 {
			t.Phase = 0
			return
		}
		ticks := arg64(cpu, 0)
		if int64(ticks) <= 0 {
			m.preempt(cpu)
			return
		}
		t.Phase = 1
		t.block(0, m.Time()+ticks)
	})
	reg(c, "OSInitThreadQueue", func(m *Machine, cpu *CPU, t *Thread) { m.Mem.Fill(arg(cpu, 0), 0, 0x10) })
	reg(c, "OSInitThreadQueueEx", func(m *Machine, cpu *CPU, t *Thread) {
		m.Mem.Fill(arg(cpu, 0), 0, 0x10)
		m.Mem.Write32(arg(cpu, 0)+8, arg(cpu, 1))
	})
	reg(c, "OSSleepThread", func(m *Machine, cpu *CPU, t *Thread) {
		if t.Phase == 1 {
			t.Phase = 0
			return
		}
		t.Phase = 1
		t.block(keyQueue|uint64(arg(cpu, 0)), 0)
	})
	reg(c, "OSWakeupThread", func(m *Machine, cpu *CPU, t *Thread) { m.wake(keyQueue | uint64(arg(cpu, 0))) })

	// mutexes
	initMutex := func(m *Machine, cpu *CPU, t *Thread) {
		a := arg(cpu, 0)
		m.Mem.Fill(a, 0, 0x2c)
		m.Mem.Write32(a, mutexTag)
		if n := arg(cpu, 1); n != 0 {
			m.Mem.Write32(a+4, n)
		}
		m.mutexes[a] = true
	}
	reg(c, "OSInitMutex", func(m *Machine, cpu *CPU, t *Thread) { cpu.GPR[4] = 0; initMutex(m, cpu, t) })
	reg(c, "OSInitMutexEx", initMutex)
	reg(c, "OSLockMutex", func(m *Machine, cpu *CPU, t *Thread) {
		if !m.tryLock(arg(cpu, 0), t) {
			t.block(keyMutex|uint64(arg(cpu, 0)), 0)
		}
	})
	reg(c, "OSTryLockMutex", func(m *Machine, cpu *CPU, t *Thread) { retBool(cpu, m.tryLock(arg(cpu, 0), t)) })
	reg(c, "OSUnlockMutex", func(m *Machine, cpu *CPU, t *Thread) { m.unlock(arg(cpu, 0), t) })

	// conditions (OSSignalCond wakes every waiter)
	initCond := func(m *Machine, cpu *CPU, t *Thread) {
		a := arg(cpu, 0)
		m.Mem.Fill(a, 0, 0x1c)
		m.Mem.Write32(a, condTag)
		m.Mem.Write32(a+4, arg(cpu, 1))
	}
	reg(c, "OSInitCond", func(m *Machine, cpu *CPU, t *Thread) { cpu.GPR[4] = 0; initCond(m, cpu, t) })
	reg(c, "OSInitCondEx", initCond)
	reg(c, "OSWaitCond", func(m *Machine, cpu *CPU, t *Thread) {
		cond, mx := arg(cpu, 0), arg(cpu, 1)
		if t.Phase == 0 {
			// let go of the mutex and wait to be signalled
			t.Saved = m.Mem.Read32(mx + mutexCount)
			m.Mem.Write32(mx+mutexOwner, 0)
			m.Mem.Write32(mx+mutexCount, 0)
			m.wake(keyMutex | uint64(mx))
			t.Phase = 1
			t.block(keyCond|uint64(cond), 0)
			return
		}
		// signalled: take the mutex back
		if !m.tryLock(mx, t) {
			t.block(keyMutex|uint64(mx), 0)
			return
		}
		m.Mem.Write32(mx+mutexCount, max(t.Saved, 1))
		t.Phase = 0
	})
	reg(c, "OSSignalCond", func(m *Machine, cpu *CPU, t *Thread) { m.wake(keyCond | uint64(arg(cpu, 0))) })

	// semaphores
	initSem := func(m *Machine, cpu *CPU, t *Thread) {
		a := arg(cpu, 0)
		m.Mem.Fill(a, 0, 0x20)
		m.Mem.Write32(a, semTag)
		m.Mem.Write32(a+4, arg(cpu, 2))
		m.Mem.Write32(a+semCount, arg(cpu, 1))
	}
	reg(c, "OSInitSemaphore", func(m *Machine, cpu *CPU, t *Thread) { cpu.GPR[5] = 0; initSem(m, cpu, t) })
	reg(c, "OSInitSemaphoreEx", initSem)
	reg(c, "OSWaitSemaphore", func(m *Machine, cpu *CPU, t *Thread) {
		a := arg(cpu, 0)
		n := int32(m.Mem.Read32(a + semCount))
		if n <= 0 {
			t.block(keySem|uint64(a), 0)
			return
		}
		m.Mem.Write32(a+semCount, uint32(n-1))
		ret(cpu, uint32(n))
	})
	reg(c, "OSTryWaitSemaphore", func(m *Machine, cpu *CPU, t *Thread) {
		a := arg(cpu, 0)
		n := int32(m.Mem.Read32(a + semCount))
		if n > 0 {
			m.Mem.Write32(a+semCount, uint32(n-1))
		}
		ret(cpu, uint32(n))
	})
	reg(c, "OSSignalSemaphore", func(m *Machine, cpu *CPU, t *Thread) {
		a := arg(cpu, 0)
		n := m.Mem.Read32(a + semCount)
		m.Mem.Write32(a+semCount, n+1)
		m.wake(keySem | uint64(a))
		ret(cpu, n)
	})
	reg(c, "OSGetSemaphoreCount", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, m.Mem.Read32(arg(cpu, 0)+semCount)) })

	// events (mode 0 = manual reset, 1 = auto reset)
	initEvent := func(m *Machine, cpu *CPU, t *Thread) {
		a := arg(cpu, 0)
		m.Mem.Fill(a, 0, 0x24)
		m.Mem.Write32(a, eventTag)
		m.Mem.Write32(a+4, arg(cpu, 3))
		m.Mem.Write32(a+eventVal, b2u(arg(cpu, 1) != 0))
		m.Mem.Write32(a+eventMode, arg(cpu, 2))
	}
	reg(c, "OSInitEvent", func(m *Machine, cpu *CPU, t *Thread) { cpu.GPR[6] = 0; initEvent(m, cpu, t) })
	reg(c, "OSInitEventEx", initEvent)
	reg(c, "OSSignalEvent", func(m *Machine, cpu *CPU, t *Thread) {
		a := arg(cpu, 0)
		m.Mem.Write32(a+eventVal, 1)
		if m.Mem.Read32(a+eventMode) == 1 {
			if w := m.wakeOne(keyEvent | uint64(a)); w != nil {
				w.Phase = 7 // let through
				m.Mem.Write32(a+eventVal, 0)
			}
			return
		}
		m.wake(keyEvent | uint64(a))
	})
	reg(c, "OSSignalEventAll", func(m *Machine, cpu *CPU, t *Thread) {
		a := arg(cpu, 0)
		auto := m.Mem.Read32(a+eventMode) == 1
		woke := false
		for _, th := range m.threadList {
			if th.State == thWaiting && th.WaitKey == keyEvent|uint64(a) {
				th.State, th.WaitKey, th.WakeAt = thReady, 0, 0
				th.Phase = 7
				woke = true
			}
		}
		if !auto || !woke {
			m.Mem.Write32(a+eventVal, 1)
		}
	})
	reg(c, "OSResetEvent", func(m *Machine, cpu *CPU, t *Thread) { m.Mem.Write32(arg(cpu, 0)+eventVal, 0) })
	reg(c, "OSWaitEvent", func(m *Machine, cpu *CPU, t *Thread) {
		a := arg(cpu, 0)
		if t.Phase == 7 {
			t.Phase = 0
			return
		}
		if m.Mem.Read32(a+eventVal) != 0 {
			if m.Mem.Read32(a+eventMode) == 1 {
				m.Mem.Write32(a+eventVal, 0)
			}
			return
		}
		t.block(keyEvent|uint64(a), 0)
	})
	reg(c, "OSWaitEventWithTimeout", func(m *Machine, cpu *CPU, t *Thread) {
		a := arg(cpu, 0)
		if t.Phase == 7 {
			t.Phase = 0
			ret(cpu, 1)
			return
		}
		if m.Mem.Read32(a+eventVal) != 0 {
			if m.Mem.Read32(a+eventMode) == 1 {
				m.Mem.Write32(a+eventVal, 0)
			}
			t.Phase = 0
			ret(cpu, 1)
			return
		}
		if t.Phase == 1 && t.TimedOut {
			t.Phase = 0
			ret(cpu, 0)
			return
		}
		if t.Phase == 0 {
			t.Deadline = m.Time() + nsToTicks(arg64(cpu, 2))
			t.Phase = 1
		}
		t.block(keyEvent|uint64(a), t.Deadline)
	})

	// message queues
	initMQ := func(m *Machine, cpu *CPU, t *Thread) {
		a := arg(cpu, 0)
		m.Mem.Fill(a, 0, 0x3c)
		m.Mem.Write32(a, mqTag)
		m.Mem.Write32(a+4, arg(cpu, 3))
		m.Mem.Write32(a+mqMsgs, arg(cpu, 1))
		m.Mem.Write32(a+mqSize, arg(cpu, 2))
	}
	reg(c, "OSInitMessageQueue", func(m *Machine, cpu *CPU, t *Thread) { cpu.GPR[6] = 0; initMQ(m, cpu, t) })
	reg(c, "OSInitMessageQueueEx", initMQ)
	send := func(jam bool) hleFunc {
		return func(m *Machine, cpu *CPU, t *Thread) {
			q, msg, flags := arg(cpu, 0), arg(cpu, 1), arg(cpu, 2)
			mem := m.Mem
			size, used, first := mem.Read32(q+mqSize), mem.Read32(q+mqUsed), mem.Read32(q+mqFirst)
			if used >= size {
				if flags&1 != 0 {
					t.block(keyMsgSend|uint64(q), 0)
					return
				}
				ret(cpu, 0)
				return
			}
			var slot uint32
			if jam || flags&2 != 0 {
				first = (first + size - 1) % size
				mem.Write32(q+mqFirst, first)
				slot = first
			} else {
				slot = (first + used) % size
			}
			mem.Move(mem.Read32(q+mqMsgs)+slot*16, msg, 16)
			mem.Write32(q+mqUsed, used+1)
			m.wake(keyMsgRecv | uint64(q))
			ret(cpu, 1)
		}
	}
	reg(c, "OSSendMessage", send(false))
	reg(c, "OSJamMessage", send(true))
	reg(c, "OSReceiveMessage", func(m *Machine, cpu *CPU, t *Thread) {
		q, msg, flags := arg(cpu, 0), arg(cpu, 1), arg(cpu, 2)
		mem := m.Mem
		size, used, first := mem.Read32(q+mqSize), mem.Read32(q+mqUsed), mem.Read32(q+mqFirst)
		if used == 0 {
			if flags&1 != 0 {
				t.block(keyMsgRecv|uint64(q), 0)
				return
			}
			ret(cpu, 0)
			return
		}
		mem.Move(msg, mem.Read32(q+mqMsgs)+first*16, 16)
		mem.Write32(q+mqFirst, (first+1)%size)
		mem.Write32(q+mqUsed, used-1)
		m.wake(keyMsgSend | uint64(q))
		ret(cpu, 1)
	})
	reg(c, "OSPeekMessage", func(m *Machine, cpu *CPU, t *Thread) {
		q, msg := arg(cpu, 0), arg(cpu, 1)
		mem := m.Mem
		if mem.Read32(q+mqUsed) == 0 {
			ret(cpu, 0)
			return
		}
		mem.Move(msg, mem.Read32(q+mqMsgs)+mem.Read32(q+mqFirst)*16, 16)
		ret(cpu, 1)
	})
	sysQueue := func(name string) hleFunc {
		return func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, m.systemQueue(name)) }
	}
	reg(c, "OSGetSystemMessageQueue", sysQueue("system"))
	reg(c, "OSGetDefaultAppIOQueue", sysQueue("appio"))

	// fast mutexes and conditions
	reg(c, "OSFastMutex_Init", func(m *Machine, cpu *CPU, t *Thread) {
		a := arg(cpu, 0)
		m.Mem.Fill(a, 0, 0x2c)
		m.Mem.Write32(a, 0x664d7554) // "fMuT"
		m.Mem.Write32(a+4, arg(cpu, 1))
	})
	reg(c, "OSFastMutex_Lock", func(m *Machine, cpu *CPU, t *Thread) {
		if !m.fastLock(arg(cpu, 0), t) {
			t.block(keyFastMtx|uint64(arg(cpu, 0)), 0)
		}
	})
	reg(c, "OSFastMutex_TryLock", func(m *Machine, cpu *CPU, t *Thread) { retBool(cpu, m.fastLock(arg(cpu, 0), t)) })
	reg(c, "OSFastMutex_Unlock", func(m *Machine, cpu *CPU, t *Thread) {
		a := arg(cpu, 0)
		if m.Mem.Read32(a+fmOwner) != t.Addr {
			return
		}
		n := m.Mem.Read32(a+fmCount) - 1
		m.Mem.Write32(a+fmCount, n)
		if n == 0 {
			m.Mem.Write32(a+fmOwner, 0)
			m.wake(keyFastMtx | uint64(a))
		}
	})
	reg(c, "OSFastCond_Init", func(m *Machine, cpu *CPU, t *Thread) {
		a := arg(cpu, 0)
		m.Mem.Fill(a, 0, 0x1c)
		m.Mem.Write32(a, 0x664e6456) // "fCnD"
		m.Mem.Write32(a+4, arg(cpu, 1))
	})
	reg(c, "OSFastCond_Wait", func(m *Machine, cpu *CPU, t *Thread) {
		cond, mx := arg(cpu, 0), arg(cpu, 1)
		if t.Phase == 0 {
			t.Saved = m.Mem.Read32(mx + fmCount)
			m.Mem.Write32(mx+fmOwner, 0)
			m.Mem.Write32(mx+fmCount, 0)
			m.wake(keyFastMtx | uint64(mx))
			t.Phase = 1
			t.block(keyFastCnd|uint64(cond), 0)
			return
		}
		if !m.fastLock(mx, t) {
			t.block(keyFastMtx|uint64(mx), 0)
			return
		}
		m.Mem.Write32(mx+fmCount, max(t.Saved, 1))
		t.Phase = 0
	})
	reg(c, "OSFastCond_Signal", func(m *Machine, cpu *CPU, t *Thread) { m.wake(keyFastCnd | uint64(arg(cpu, 0))) })

	// spin locks: { owner, unknown, recursion, ... }
	spinTry := func(m *Machine, a uint32, t *Thread) bool {
		owner := m.Mem.Read32(a)
		switch owner {
		case 0:
			m.Mem.Write32(a, t.Addr)
			return true
		case t.Addr:
			m.Mem.Write32(a+8, m.Mem.Read32(a+8)+1)
			return true
		}
		return false
	}
	spinAcquire := func(m *Machine, cpu *CPU, t *Thread) {
		if spinTry(m, arg(cpu, 0), t) {
			ret(cpu, 1)
			return
		}
		m.preempt(cpu)
	}
	spinTryF := func(m *Machine, cpu *CPU, t *Thread) { retBool(cpu, spinTry(m, arg(cpu, 0), t)) }
	spinTimeout := func(m *Machine, cpu *CPU, t *Thread) {
		a := arg(cpu, 0)
		if spinTry(m, a, t) {
			t.Phase = 0
			ret(cpu, 1)
			return
		}
		if t.Phase == 0 {
			t.Deadline = m.Time() + nsToTicks(arg64(cpu, 2))
			t.Phase = 1
		}
		if m.Time() >= t.Deadline {
			t.Phase = 0
			ret(cpu, 0)
			return
		}
		m.preempt(cpu)
	}
	spinRelease := func(m *Machine, cpu *CPU, t *Thread) {
		a := arg(cpu, 0)
		if n := m.Mem.Read32(a + 8); n > 0 {
			m.Mem.Write32(a+8, n-1)
		} else {
			m.Mem.Write32(a, 0)
		}
		ret(cpu, 1)
	}
	reg(c, "OSInitSpinLock", func(m *Machine, cpu *CPU, t *Thread) { m.Mem.Fill(arg(cpu, 0), 0, 0x10) })
	reg(c, "OSAcquireSpinLock", spinAcquire)
	reg(c, "OSUninterruptibleSpinLock_Acquire", spinAcquire)
	reg(c, "OSTryAcquireSpinLock", spinTryF)
	reg(c, "OSUninterruptibleSpinLock_TryAcquire", spinTryF)
	reg(c, "OSTryAcquireSpinLockWithTimeout", spinTimeout)
	reg(c, "OSUninterruptibleSpinLock_TryAcquireWithTimeout", spinTimeout)
	reg(c, "OSReleaseSpinLock", spinRelease)
	reg(c, "OSUninterruptibleSpinLock_Release", spinRelease)

	// rendezvous: every core in the mask arrives before any goes on
	reg(c, "OSInitRendezvous", func(m *Machine, cpu *CPU, t *Thread) { m.Mem.Fill(arg(cpu, 0), 0, 0x10) })
	rendez := func(timeout bool) hleFunc {
		return func(m *Machine, cpu *CPU, t *Thread) {
			a, mask := arg(cpu, 0), arg(cpu, 1)
			m.Mem.Write32(a, m.Mem.Read32(a)|1<<cpu.Core)
			if m.Mem.Read32(a)&mask == mask {
				m.wake(keyRendez | uint64(a))
				t.Phase = 0
				ret(cpu, 1)
				return
			}
			if timeout {
				if t.Phase == 0 {
					t.Deadline = m.Time() + nsToTicks(arg64(cpu, 2))
					t.Phase = 1
				} else if t.TimedOut {
					t.Phase = 0
					ret(cpu, 0)
					return
				}
				t.block(keyRendez|uint64(a), t.Deadline)
				return
			}
			t.block(keyRendez|uint64(a), 0)
		}
	}
	reg(c, "OSWaitRendezvous", rendez(false))
	reg(c, "OSWaitRendezvousWithTimeout", rendez(true))

	// alarms
	reg(c, "OSCreateAlarm", func(m *Machine, cpu *CPU, t *Thread) { m.createAlarm(arg(cpu, 0), 0) })
	reg(c, "OSCreateAlarmEx", func(m *Machine, cpu *CPU, t *Thread) { m.createAlarm(arg(cpu, 0), arg(cpu, 1)) })
	reg(c, "OSSetAlarm", func(m *Machine, cpu *CPU, t *Thread) {
		m.setAlarm(cpu, arg(cpu, 0), m.Time()+arg64(cpu, 2), 0, arg(cpu, 4))
		ret(cpu, 1)
	})
	reg(c, "OSSetPeriodicAlarm", func(m *Machine, cpu *CPU, t *Thread) {
		start := arg64(cpu, 2) // an OSTime
		period := arg64(cpu, 4)
		now := m.Time()
		fire := now
		if s := start - m.bootTime; int64(s) > int64(now) {
			fire = s
		} else if period > 0 {
			// the next whole period after start
			if s < now {
				fire = now + period - (now-s)%period
			}
		}
		m.setAlarm(cpu, arg(cpu, 0), fire, period, arg(cpu, 6))
		ret(cpu, 1)
	})
	reg(c, "OSCancelAlarm", func(m *Machine, cpu *CPU, t *Thread) { retBool(cpu, m.cancelAlarm(arg(cpu, 0))) })
	reg(c, "OSCancelAlarms", func(m *Machine, cpu *CPU, t *Thread) {
		tag := arg(cpu, 0)
		for _, al := range m.alarms {
			if al.Active && m.Mem.Read32(al.Addr+0x10) == tag {
				m.cancelAlarm(al.Addr)
			}
		}
	})
	reg(c, "OSSetAlarmUserData", func(m *Machine, cpu *CPU, t *Thread) { m.Mem.Write32(arg(cpu, 0)+0x38, arg(cpu, 1)) })
	reg(c, "OSGetAlarmUserData", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, m.Mem.Read32(arg(cpu, 0)+0x38)) })
	reg(c, "OSSetAlarmTag", func(m *Machine, cpu *CPU, t *Thread) { m.Mem.Write32(arg(cpu, 0)+0x10, arg(cpu, 1)) })
	reg(c, "OSWaitAlarm", func(m *Machine, cpu *CPU, t *Thread) {
		a := arg(cpu, 0)
		if t.Phase == 1 {
			t.Phase = 0
			ret(cpu, b2u(m.Mem.Read32(a+0x3c) != 3))
			return
		}
		al := m.alarm(a)
		if al == nil || !al.Active {
			ret(cpu, 0)
			return
		}
		t.Phase = 1
		t.block(keyAlarm|uint64(a), 0)
	})
	reg(c, "__RPAlarmLoop", func(m *Machine, cpu *CPU, t *Thread) {
		q := m.alarmQueue[cpu.Core]
		if len(q) == 0 {
			t.block(keyAlarm|uint64(0xffff0000+uint32(cpu.Core)), 0)
			return
		}
		cb := q[0]
		m.alarmQueue[cpu.Core] = q[1:]
		copy(cpu.GPR[3:7], cb.Args[:])
		cpu.GPR[1] = t.Saved
		cpu.LR = m.thunk("coreinit", "__RPAlarmLoop")
		cpu.PC = cb.Fn
		m.jumped = true
	})
}

// Callback is a call the system makes into the program (an alarm firing,
// a file operation finishing), run by a hidden high-priority thread.
type Callback struct {
	Fn   uint32
	Args [4]uint32
}

// queueCallback has core's callback thread call fn(args...).
func (m *Machine) queueCallback(core int, fn uint32, args ...uint32) {
	if fn == 0 {
		return
	}
	m.alarmThread(core)
	cb := Callback{Fn: fn}
	copy(cb.Args[:], args)
	m.alarmQueue[core] = append(m.alarmQueue[core], cb)
	m.wake(keyAlarm | uint64(0xffff0000+uint32(core)))
}

// preempt lets other threads have the core; the call returns normally when
// this thread runs again.
func (m *Machine) preempt(cpu *CPU) {
	cpu.Stop()
	if t := m.current[cpu.Core]; t != nil {
		m.runSeq++
		t.LastRun = m.runSeq
	}
	m.current[cpu.Core] = nil
}

func (m *Machine) tryLock(a uint32, t *Thread) bool {
	mem := m.Mem
	m.mutexes[a] = true
	switch mem.Read32(a + mutexOwner) {
	case 0:
		mem.Write32(a+mutexOwner, t.Addr)
		mem.Write32(a+mutexCount, 1)
		return true
	case t.Addr:
		mem.Write32(a+mutexCount, mem.Read32(a+mutexCount)+1)
		return true
	}
	return false
}

func (m *Machine) unlock(a uint32, t *Thread) {
	mem := m.Mem
	if mem.Read32(a+mutexOwner) != t.Addr {
		return
	}
	n := mem.Read32(a+mutexCount) - 1
	mem.Write32(a+mutexCount, n)
	if n == 0 {
		mem.Write32(a+mutexOwner, 0)
		m.wake(keyMutex | uint64(a))
	}
}

func (m *Machine) fastLock(a uint32, t *Thread) bool {
	mem := m.Mem
	switch mem.Read32(a + fmOwner) {
	case 0:
		mem.Write32(a+fmOwner, t.Addr)
		mem.Write32(a+fmCount, 1)
		return true
	case t.Addr:
		mem.Write32(a+fmCount, mem.Read32(a+fmCount)+1)
		return true
	}
	return false
}

// threadExit runs the thread's cleanup and deallocator callbacks, then ends it.
func (m *Machine) threadExit(cpu *CPU, t *Thread, v uint32) {
	t.Exit = v
	stack := m.Mem.Read32(t.Addr + osThreadStackLo)
	if cb := t.Cleanup; cb != 0 {
		t.Cleanup = 0
		cpu.GPR[3], cpu.GPR[4] = t.Addr, stack
		cpu.LR = m.thunk("coreinit", "__RPThreadExit2")
		cpu.PC = cb
		m.jumped = true
		return
	}
	if cb := t.Dealloc; cb != 0 {
		t.Dealloc = 0
		m.exitThreadState(t)
		cpu.GPR[3], cpu.GPR[4] = t.Addr, stack
		cpu.LR = m.thunk("coreinit", "__RPThreadExit2")
		cpu.PC = cb
		m.jumped = true
		return
	}
	m.exitThread(t, v)
	t.yield()
}

// exitThreadState wakes joiners before the deallocator runs.
func (m *Machine) exitThreadState(t *Thread) {
	m.wake(keyJoin | uint64(t.Addr))
}

func (m *Machine) systemQueue(name string) uint32 {
	key := "queue:" + name
	if a, ok := m.perThread[key]; ok {
		return a
	}
	a := m.sysAlloc(0x3c, 16)
	msgs := m.sysAlloc(16*16, 16)
	m.Mem.Write32(a, mqTag)
	m.Mem.Write32(a+mqMsgs, msgs)
	m.Mem.Write32(a+mqSize, 16)
	if m.perThread == nil {
		m.perThread = map[string]uint32{}
	}
	m.perThread[key] = a
	return a
}

// Alarm is an OSAlarm that's been set.
type Alarm struct {
	Addr   uint32
	Fire   uint64
	Period uint64
	Core   int
	Active bool
}

func (m *Machine) alarm(a uint32) *Alarm {
	for _, al := range m.alarms {
		if al.Addr == a {
			return al
		}
	}
	return nil
}

func (m *Machine) createAlarm(a, name uint32) {
	m.Mem.Fill(a, 0, 0x58)
	m.Mem.Write32(a, 0x614c724d) // "aLrM"
	m.Mem.Write32(a+4, name)
	if al := m.alarm(a); al != nil {
		al.Active = false
	}
}

func (m *Machine) setAlarm(cpu *CPU, a uint32, fire, period uint64, cb uint32) {
	al := m.alarm(a)
	if al == nil {
		al = &Alarm{Addr: a}
		m.alarms = append(m.alarms, al)
	}
	al.Fire, al.Period, al.Core, al.Active = fire, period, cpu.Core, true
	m.Mem.Write32(a+0x0c, cb)
	m.Mem.Write64(a+0x18, fire+m.bootTime)
	m.Mem.Write64(a+0x28, period)
	m.Mem.Write32(a+0x3c, 1) // set
	m.alarmThread(cpu.Core)
}

func (m *Machine) cancelAlarm(a uint32) bool {
	al := m.alarm(a)
	if al == nil || !al.Active {
		return false
	}
	al.Active = false
	m.Mem.Write32(a+0x3c, 3) // canceled
	m.wake(keyAlarm | uint64(a))
	return true
}

// alarmThread makes the hidden thread that runs a core's alarm callbacks.
func (m *Machine) alarmThread(core int) {
	if m.alarmTh[core] != nil {
		return
	}
	addr := m.sysAlloc(osThreadSize, 64)
	stack := m.heaps.allocSystem(m, 0x8000, 64) + 0x8000
	t := m.newThread(addr, m.thunk("coreinit", "__RPAlarmLoop"), 0, 0, stack, 0x8000, 0, 1<<core)
	t.Priority = -1 // above every program thread, like an interrupt
	t.Saved = t.Ctx.GPR[1]
	t.State, t.Suspend = thReady, 0
	m.Mem.Write32(addr+osThreadName, m.cstrAlloc("{RetroPixels alarms}"))
	m.alarmTh[core] = t
	if m.alarmContext == 0 {
		m.alarmContext = m.sysAlloc(0x320, 32)
	}
}

func (m *Machine) runAlarms() {
	now := m.tb
	for _, al := range m.alarms {
		if !al.Active || al.Fire > now {
			continue
		}
		m.queueCallback(al.Core, m.Mem.Read32(al.Addr+0x0c), al.Addr, m.alarmContext)
		if al.Period > 0 {
			for al.Fire <= now {
				al.Fire += al.Period
			}
			m.Mem.Write64(al.Addr+0x18, al.Fire+m.bootTime)
		} else {
			al.Active = false
			m.Mem.Write32(al.Addr+0x3c, 2) // fired
			m.wake(keyAlarm | uint64(al.Addr))
		}
	}
}

func (m *Machine) nextAlarm() uint64 {
	var next uint64 = math.MaxUint64
	for _, al := range m.alarms {
		if al.Active && al.Fire < next {
			next = al.Fire
		}
	}
	if next == math.MaxUint64 {
		return 0
	}
	return next
}
