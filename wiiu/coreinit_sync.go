// This Source Code Form is subject to the terms of the Mozilla Public License,
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at https://mozilla.org/MPL/2.0/.
//
// Based on Cemu (https://github.com/cemu-project/Cemu), src/Cafe/OS/libs/
// coreinit/coreinit_Synchronization.cpp, coreinit_Spinlock.cpp,
// coreinit_MessageQueue.cpp and coreinit_Thread.h, rewritten in Go for
// RetroPixels.

package wiiu

// coreinit: mutexes, conditions, semaphores, events, rendezvous, fast
// mutexes and conditions, spin locks and message queues.
//
// A mutex or a semaphore count let go of while threads wait for it goes
// straight to the first of them, so a thread that takes it again at once
// (on its own core, or between another core's turns) can't keep it from them.

import "sort"

// the program's synchronisation objects
const (
	mutexTag   = 0x6d557458 // "mUtX"
	mutexOwner = 0x1c
	mutexCount = 0x20

	condTag   = 0x634e6456 // "cNdV"
	semTag    = 0x73506852 // "sPhR"
	semCount  = 0x0c
	eventTag  = 0x65566e54 // "eVnT"
	eventVal  = 0x0c
	eventMode = 0x20       // 0 manual reset, 1 auto reset
	mqTag     = 0x6d536751 // "mSgQ"
	mqMsgs    = 0x2c
	mqSize    = 0x30
	mqFirst   = 0x34
	mqUsed    = 0x38

	fastMutexTag = 0x664d7458 // "fMtX"
	fastCondTag  = 0x664e6456 // "fNdV"
	fmOwner      = 0x1c
	fmCount      = 0x20

	// message flags
	msgBlock = 1
	msgHigh  = 2 // in front of the messages already there
)

// nsToTicks converts nanoseconds to time base ticks (TimerHz is
// 31250 * 1989).
func nsToTicks(ns uint64) uint64 { return TimerHz / 31250 * ns / 32000 }

// timeout is the time base value a wait of ns nanoseconds ends at (0 for
// one so long it never does).
func (m *Machine) timeout(ns uint64) uint64 {
	if ns >= 1<<52 {
		return 0
	}
	return m.Time() + max(nsToTicks(ns), 1)
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

// releaseMutex frees a mutex whose count has come to 0, handing it to the
// first thread waiting for it.
func (m *Machine) releaseMutex(a uint32) {
	mem := m.Mem
	mem.Write32(a+mutexOwner, 0)
	if w := m.wakeOne(keyMutex | uint64(a)); w != nil {
		mem.Write32(a+mutexOwner, w.Addr)
		mem.Write32(a+mutexCount, 1)
		w.Granted = true
	}
}

func (m *Machine) unlock(a uint32, t *Thread) {
	mem := m.Mem
	n := mem.Read32(a + mutexCount)
	if n == 0 || mem.Read32(a+mutexOwner) != t.Addr {
		return
	}
	mem.Write32(a+mutexCount, n-1)
	if n == 1 {
		m.releaseMutex(a)
	}
}

// heldMutexes lists the mutexes t owns.
func (m *Machine) heldMutexes(t *Thread) []uint32 {
	var out []uint32
	for a := range m.mutexes {
		if m.Mem.Read32(a+mutexOwner) == t.Addr && m.Mem.Read32(a) == mutexTag {
			out = append(out, a)
		}
	}
	sort.Slice(out, func(i, j int) bool { return out[i] < out[j] })
	return out
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

func (m *Machine) releaseFastMutex(a uint32) {
	mem := m.Mem
	mem.Write32(a+fmOwner, 0)
	mem.Write32(a+fmCount, 0)
	if w := m.wakeOne(keyFastMtx | uint64(a)); w != nil {
		mem.Write32(a+fmOwner, w.Addr)
		mem.Write32(a+fmCount, 1)
		w.Granted = true
	}
}

// boost raises a thread holding a spin lock to the top priority, so a
// thread spinning for the lock on its core can't keep it from running.
func (m *Machine) boost(t *Thread) {
	t.Boost++
	m.updatePriority(t)
}

func (m *Machine) deboost(t *Thread) {
	if t.Boost > 0 {
		t.Boost--
	}
	m.updatePriority(t)
}

// retry runs the current call again after the threads of the same
// priority have had a turn.
func (m *Machine) retry(t *Thread) {
	m.runSeq++
	t.LastRun = m.runSeq
	t.yield()
}

func init() {
	c := "coreinit"

	// mutexes
	initMutex := func(m *Machine, a, userData uint32) {
		m.Mem.Fill(a, 0, 0x2c)
		m.Mem.Write32(a, mutexTag)
		m.Mem.Write32(a+4, userData)
		m.Mem.Write32(a+0x14, a) // its thread queue's user data
		m.mutexes[a] = true
	}
	reg(c, "OSInitMutex", func(m *Machine, cpu *CPU, t *Thread) { initMutex(m, arg(cpu, 0), 0) })
	reg(c, "OSInitMutexEx", func(m *Machine, cpu *CPU, t *Thread) { initMutex(m, arg(cpu, 0), arg(cpu, 1)) })
	reg(c, "OSLockMutex", func(m *Machine, cpu *CPU, t *Thread) {
		if t.Granted {
			return // handed over by OSUnlockMutex
		}
		if t.Cancel {
			m.osExitThread(cpu, t, 0xffffffff)
			return
		}
		if !m.tryLock(arg(cpu, 0), t) {
			t.block(keyMutex|uint64(arg(cpu, 0)), 0)
		}
	})
	reg(c, "OSTryLockMutex", func(m *Machine, cpu *CPU, t *Thread) {
		if t.Cancel {
			m.osExitThread(cpu, t, 0xffffffff)
			return
		}
		retBool(cpu, m.tryLock(arg(cpu, 0), t))
	})
	reg(c, "OSUnlockMutex", func(m *Machine, cpu *CPU, t *Thread) { m.unlock(arg(cpu, 0), t) })

	// conditions: OSSignalCond wakes every waiter
	initCond := func(m *Machine, a, tag, userData uint32) {
		m.Mem.Fill(a, 0, 0x1c)
		m.Mem.Write32(a, tag)
		m.Mem.Write32(a+4, userData)
		m.Mem.Write32(a+0x14, a)
	}
	reg(c, "OSInitCond", func(m *Machine, cpu *CPU, t *Thread) { initCond(m, arg(cpu, 0), condTag, 0) })
	reg(c, "OSInitCondEx", func(m *Machine, cpu *CPU, t *Thread) { initCond(m, arg(cpu, 0), condTag, arg(cpu, 1)) })
	reg(c, "OSWaitCond", func(m *Machine, cpu *CPU, t *Thread) {
		cond, mx := arg(cpu, 0), arg(cpu, 1)
		switch t.Phase {
		case 0:
			// let go of the mutex and wait to be signalled
			t.Saved = m.Mem.Read32(mx + mutexCount)
			m.Mem.Write32(mx+mutexCount, 0)
			m.releaseMutex(mx)
			t.block(keyCond|uint64(cond), 0)
			t.Phase = 1
			return
		case 1:
			t.Phase = 2
		}
		// signalled: take the mutex back
		if !t.Granted && !m.tryLock(mx, t) {
			t.block(keyMutex|uint64(mx), 0)
			return
		}
		m.Mem.Write32(mx+mutexCount, max(t.Saved, 1))
	})
	reg(c, "OSSignalCond", func(m *Machine, cpu *CPU, t *Thread) { m.wake(keyCond | uint64(arg(cpu, 0))) })

	// semaphores
	initSem := func(m *Machine, a, count, userData uint32) {
		m.Mem.Fill(a, 0, 0x20)
		m.Mem.Write32(a, semTag)
		m.Mem.Write32(a+4, userData)
		m.Mem.Write32(a+semCount, count)
		m.Mem.Write32(a+0x18, a)
	}
	reg(c, "OSInitSemaphore", func(m *Machine, cpu *CPU, t *Thread) { initSem(m, arg(cpu, 0), arg(cpu, 1), 0) })
	reg(c, "OSInitSemaphoreEx", func(m *Machine, cpu *CPU, t *Thread) { initSem(m, arg(cpu, 0), arg(cpu, 1), arg(cpu, 2)) })
	reg(c, "OSWaitSemaphore", func(m *Machine, cpu *CPU, t *Thread) {
		if t.Granted {
			ret(cpu, t.GrantVal)
			return
		}
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
		n := int32(m.Mem.Read32(a + semCount))
		m.Mem.Write32(a+semCount, uint32(n+1))
		if n+1 > 0 {
			if w := m.wakeOne(keySem | uint64(a)); w != nil {
				w.Granted, w.GrantVal = true, uint32(n+1)
				m.Mem.Write32(a+semCount, uint32(n))
			}
		}
		ret(cpu, uint32(n))
	})
	reg(c, "OSGetSemaphoreCount", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, m.Mem.Read32(arg(cpu, 0)+semCount)) })

	// events: an auto-reset event lets one waiter through each time it's
	// signalled, a manual-reset one all of them until it's reset
	initEvent := func(m *Machine, a, value, mode, userData uint32) {
		m.Mem.Fill(a, 0, 0x24)
		m.Mem.Write32(a, eventTag)
		m.Mem.Write32(a+4, userData)
		m.Mem.Write32(a+eventVal, b2u(value != 0))
		m.Mem.Write32(a+0x18, a)
		m.Mem.Write32(a+eventMode, mode)
	}
	reg(c, "OSInitEvent", func(m *Machine, cpu *CPU, t *Thread) { initEvent(m, arg(cpu, 0), arg(cpu, 1), arg(cpu, 2), 0) })
	reg(c, "OSInitEventEx", func(m *Machine, cpu *CPU, t *Thread) {
		initEvent(m, arg(cpu, 0), arg(cpu, 1), arg(cpu, 2), arg(cpu, 3))
	})
	signal := func(all bool) hleFunc {
		return func(m *Machine, cpu *CPU, t *Thread) {
			a := arg(cpu, 0)
			if m.Mem.Read32(a+eventVal) != 0 {
				return
			}
			key := keyEvent | uint64(a)
			if m.Mem.Read32(a+eventMode) != 1 {
				m.Mem.Write32(a+eventVal, 1)
				m.wake(key)
				return
			}
			// auto reset: waiters go on and the event stays unsignalled;
			// with none, it's signalled for the next to wait
			if all && m.wake(key) > 0 || !all && m.wakeOne(key) != nil {
				return
			}
			m.Mem.Write32(a+eventVal, 1)
		}
	}
	reg(c, "OSSignalEvent", signal(false))
	reg(c, "OSSignalEventAll", signal(true))
	reg(c, "OSResetEvent", func(m *Machine, cpu *CPU, t *Thread) { m.Mem.Write32(arg(cpu, 0)+eventVal, 0) })
	// takeEvent: a signalled event lets the thread through (resetting an
	// auto-reset one)
	takeEvent := func(m *Machine, a uint32) bool {
		if m.Mem.Read32(a+eventVal) == 0 {
			return false
		}
		if m.Mem.Read32(a+eventMode) == 1 {
			m.Mem.Write32(a+eventVal, 0)
		}
		return true
	}
	reg(c, "OSWaitEvent", func(m *Machine, cpu *CPU, t *Thread) {
		a := arg(cpu, 0)
		if t.Woken || takeEvent(m, a) {
			return
		}
		t.block(keyEvent|uint64(a), 0)
	})
	reg(c, "OSWaitEventWithTimeout", func(m *Machine, cpu *CPU, t *Thread) {
		a := arg(cpu, 0)
		switch {
		case t.Woken:
			ret(cpu, 1)
			return
		case t.TimedOut:
			ret(cpu, 0)
			return
		case takeEvent(m, a):
			ret(cpu, 1)
			return
		}
		ns := arg64(cpu, 2)
		if ns == 0 {
			ret(cpu, 0)
			return
		}
		// a little early, for programs that time the wait themselves
		if ns < 0x00ffffffffffffff {
			ns = ns * 98 / 100
		}
		t.block(keyEvent|uint64(a), m.timeout(ns))
	})

	// rendezvous: { u32 arrived[3]; void *userData }
	reg(c, "OSInitRendezvous", func(m *Machine, cpu *CPU, t *Thread) {
		a := arg(cpu, 0)
		m.Mem.Fill(a, 0, 0x0c)
		m.Mem.Write32(a+0x0c, a)
	})
	rendez := func(timeout bool) hleFunc {
		return func(m *Machine, cpu *CPU, t *Thread) {
			a, mask := arg(cpu, 0), arg(cpu, 1)
			if t.Phase == 0 {
				// this core has arrived
				m.Mem.Write32(a+uint32(cpu.Core)*4, 1)
				m.wake(keyRendez | uint64(a))
				t.Deadline = 0
				if timeout {
					t.Deadline = m.timeout(arg64(cpu, 2))
				}
				t.Phase = 1
			}
			met := true
			for i := uint32(0); i < 3; i++ {
				if mask&(1<<i) != 0 && m.Mem.Read32(a+i*4) == 0 {
					met = false
				}
			}
			if met {
				ret(cpu, 1)
				return
			}
			if t.TimedOut {
				ret(cpu, 0)
				return
			}
			t.block(keyRendez|uint64(a), t.Deadline)
		}
	}
	reg(c, "OSWaitRendezvous", rendez(false))
	reg(c, "OSWaitRendezvousWithTimeout", rendez(true))

	// fast mutexes and conditions
	reg(c, "OSFastMutex_Init", func(m *Machine, cpu *CPU, t *Thread) {
		a := arg(cpu, 0)
		m.Mem.Fill(a, 0, 0x2c)
		m.Mem.Write32(a, fastMutexTag)
		m.Mem.Write32(a+4, arg(cpu, 1))
	})
	reg(c, "OSFastMutex_Lock", func(m *Machine, cpu *CPU, t *Thread) {
		if !t.Granted && !m.fastLock(arg(cpu, 0), t) {
			t.block(keyFastMtx|uint64(arg(cpu, 0)), 0)
		}
	})
	reg(c, "OSFastMutex_TryLock", func(m *Machine, cpu *CPU, t *Thread) { retBool(cpu, m.fastLock(arg(cpu, 0), t)) })
	reg(c, "OSFastMutex_Unlock", func(m *Machine, cpu *CPU, t *Thread) {
		a := arg(cpu, 0)
		n := m.Mem.Read32(a + fmCount)
		if m.Mem.Read32(a+fmOwner) != t.Addr || n == 0 {
			return
		}
		m.Mem.Write32(a+fmCount, n-1)
		if n == 1 {
			m.releaseFastMutex(a)
		}
	})
	reg(c, "OSFastCond_Init", func(m *Machine, cpu *CPU, t *Thread) { initCond(m, arg(cpu, 0), fastCondTag, arg(cpu, 1)) })
	reg(c, "OSFastCond_Wait", func(m *Machine, cpu *CPU, t *Thread) {
		cond, mx := arg(cpu, 0), arg(cpu, 1)
		switch t.Phase {
		case 0:
			t.Saved = m.Mem.Read32(mx + fmCount)
			m.releaseFastMutex(mx)
			t.block(keyFastCnd|uint64(cond), 0)
			t.Phase = 1
			return
		case 1:
			t.Phase = 2
		}
		if !t.Granted && !m.fastLock(mx, t) {
			t.block(keyFastMtx|uint64(mx), 0)
			return
		}
		m.Mem.Write32(mx+fmCount, max(t.Saved, 1))
	})
	reg(c, "OSFastCond_Signal", func(m *Machine, cpu *CPU, t *Thread) { m.wake(keyFastCnd | uint64(arg(cpu, 0))) })

	// spin locks: { owner, userData, count (recursion), interrupt mask }
	spinTry := func(m *Machine, a uint32, t *Thread) (got, again bool) {
		switch m.Mem.Read32(a) {
		case t.Addr:
			m.Mem.Write32(a+8, m.Mem.Read32(a+8)+1)
			return true, true
		case 0:
			m.Mem.Write32(a, t.Addr)
			m.boost(t)
			return true, false
		}
		return false, false
	}
	// acquire: uninterruptible locks turn the interrupts off while held
	acquire := func(m *Machine, cpu *CPU, t *Thread, a uint32, uninterruptible bool) bool {
		got, again := spinTry(m, a, t)
		if got && !again && uninterruptible {
			m.Mem.Write32(a+0x0c, b2u(!t.IntsOff))
			t.IntsOff = true
		}
		return got
	}
	spinAcquire := func(uninterruptible bool) hleFunc {
		return func(m *Machine, cpu *CPU, t *Thread) {
			if acquire(m, cpu, t, arg(cpu, 0), uninterruptible) {
				ret(cpu, 1)
				return
			}
			m.retry(t)
		}
	}
	spinTryOnce := func(uninterruptible bool) hleFunc {
		return func(m *Machine, cpu *CPU, t *Thread) {
			retBool(cpu, acquire(m, cpu, t, arg(cpu, 0), uninterruptible))
		}
	}
	spinTimeout := func(uninterruptible bool) hleFunc {
		return func(m *Machine, cpu *CPU, t *Thread) {
			if acquire(m, cpu, t, arg(cpu, 0), uninterruptible) {
				ret(cpu, 1)
				return
			}
			if t.Phase == 0 {
				t.Deadline = m.timeout(arg64(cpu, 2))
				t.Phase = 1
			}
			if t.Deadline != 0 && m.Time() >= t.Deadline {
				ret(cpu, 0)
				return
			}
			m.retry(t)
		}
	}
	spinRelease := func(uninterruptible bool) hleFunc {
		return func(m *Machine, cpu *CPU, t *Thread) {
			a := arg(cpu, 0)
			ret(cpu, 1)
			if n := m.Mem.Read32(a + 8); n > 0 {
				m.Mem.Write32(a+8, n-1)
				return
			}
			if m.Mem.Read32(a) != t.Addr {
				return
			}
			if uninterruptible {
				t.IntsOff = m.Mem.Read32(a+0x0c) == 0
				m.Mem.Write32(a+0x0c, 1)
			}
			m.Mem.Write32(a, 0)
			m.deboost(t)
		}
	}
	reg(c, "OSInitSpinLock", func(m *Machine, cpu *CPU, t *Thread) {
		a := arg(cpu, 0)
		m.Mem.Fill(a, 0, 0x10)
		m.Mem.Write32(a+4, a)
		m.Mem.Write32(a+0x0c, 1)
	})
	reg(c, "OSAcquireSpinLock", spinAcquire(false))
	reg(c, "OSUninterruptibleSpinLock_Acquire", spinAcquire(true))
	reg(c, "OSTryAcquireSpinLock", spinTryOnce(false))
	reg(c, "OSUninterruptibleSpinLock_TryAcquire", spinTryOnce(true))
	reg(c, "OSTryAcquireSpinLockWithTimeout", spinTimeout(false))
	reg(c, "OSUninterruptibleSpinLock_TryAcquireWithTimeout", spinTimeout(true))
	reg(c, "OSReleaseSpinLock", spinRelease(false))
	reg(c, "OSUninterruptibleSpinLock_Release", spinRelease(true))

	// message queues
	initMQ := func(m *Machine, a, msgs, size, userData uint32) {
		m.Mem.Fill(a, 0, 0x3c)
		m.Mem.Write32(a, mqTag)
		m.Mem.Write32(a+4, userData)
		m.Mem.Write32(a+0x14, a)
		m.Mem.Write32(a+0x24, a)
		m.Mem.Write32(a+mqMsgs, msgs)
		m.Mem.Write32(a+mqSize, size)
	}
	reg(c, "OSInitMessageQueue", func(m *Machine, cpu *CPU, t *Thread) { initMQ(m, arg(cpu, 0), arg(cpu, 1), arg(cpu, 2), 0) })
	reg(c, "OSInitMessageQueueEx", func(m *Machine, cpu *CPU, t *Thread) {
		initMQ(m, arg(cpu, 0), arg(cpu, 1), arg(cpu, 2), arg(cpu, 3))
	})
	send := func(jam bool) hleFunc {
		return func(m *Machine, cpu *CPU, t *Thread) {
			q, msg, flags := arg(cpu, 0), arg(cpu, 1), arg(cpu, 2)
			mem := m.Mem
			size, used, first := mem.Read32(q+mqSize), mem.Read32(q+mqUsed), mem.Read32(q+mqFirst)
			if used >= size {
				if flags&msgBlock != 0 {
					t.block(keyMsgSend|uint64(q), 0)
					return
				}
				ret(cpu, 0)
				return
			}
			var slot uint32
			if jam || flags&msgHigh != 0 {
				first = (first + size - 1) % size
				mem.Write32(q+mqFirst, first)
				slot = first
			} else {
				slot = (first + used) % size
			}
			mem.Move(mem.Read32(q+mqMsgs)+slot*16, msg, 16)
			mem.Write32(q+mqUsed, used+1)
			m.wakeOne(keyMsgRecv | uint64(q))
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
			if flags&msgBlock != 0 {
				t.block(keyMsgRecv|uint64(q), 0)
				return
			}
			ret(cpu, 0)
			return
		}
		mem.Move(msg, mem.Read32(q+mqMsgs)+first*16, 16)
		mem.Write32(q+mqFirst, (first+1)%size)
		mem.Write32(q+mqUsed, used-1)
		m.wakeOne(keyMsgSend | uint64(q))
		ret(cpu, 1)
	})
	reg(c, "OSPeekMessage", func(m *Machine, cpu *CPU, t *Thread) {
		q, msg := arg(cpu, 0), arg(cpu, 1)
		mem := m.Mem
		if mem.Read32(q+mqUsed) == 0 {
			ret(cpu, 0)
			return
		}
		if msg != 0 {
			mem.Move(msg, mem.Read32(q+mqMsgs)+mem.Read32(q+mqFirst)*16, 16)
		}
		ret(cpu, 1)
	})
}
