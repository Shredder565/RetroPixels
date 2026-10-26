// Original code for RetroPixels. Not derived from Cemu.

package wiiu

// coreinit synchronisation: mutexes, condition variables, semaphores,
// events, rendezvous, fast mutexes and conditions, spin locks and message
// queues.
//
// Blocking follows the kernel's model (kernel.go): a call that has to wait
// calls t.block and returns; it runs again once the thread is readied, and
// can tell from t.Woken, t.Granted or t.TimedOut why. When a mutex or a
// semaphore unit is released while threads wait for it, it is handed to the
// first waiter directly (t.Granted), so a thread that releases and grabs it
// again straight away can't starve the waiters.

import "slices"

// object layouts and tags
const (
	mutexTag   = 0x6d557458 // "mUtX"
	mutexOwner = 0x1c       // owning OSThread
	mutexCount = 0x20       // recursion depth

	condTag   = 0x634e6456 // "cNdV"
	semTag    = 0x73506852 // "sPhR"
	semCount  = 0x0c
	eventTag  = 0x65566e54 // "eVnT"
	eventVal  = 0x0c
	eventMode = 0x20 // 0 manual reset, 1 auto reset

	mqTag   = 0x6d536751 // "mSgQ"
	mqMsgs  = 0x2c       // the ring of 16-byte messages
	mqSize  = 0x30       // its capacity
	mqFirst = 0x34       // index of the oldest message
	mqUsed  = 0x38       // messages in it

	fastMutexTag = 0x664d7458 // "fMtX"
	fastCondTag  = 0x664e6456 // "fNdV"
	fmOwner      = 0x1c
	fmCount      = 0x20

	// OSSendMessage flags
	msgBlock = 1
	msgHigh  = 2 // goes in front of the queued messages

	eventAutoReset = 1
	msgBytes       = 16
)

// nsToTicks converts nanoseconds to time base ticks. TimerHz is 31250*1989,
// so ticks = ns * 1989 / 32000 exactly.
func nsToTicks(ns uint64) uint64 { return ns * (TimerHz / 31250) / 32000 }

// timeout is the time base value at which a wait of ns nanoseconds ends, or
// 0 (no deadline) for waits too long to matter.
func (m *Machine) timeout(ns uint64) uint64 {
	if ns >= 1<<52 {
		return 0
	}
	return m.Time() + max(nsToTicks(ns), 1)
}

// ---- recursive locks (OSMutex and OSFastMutex share the owner/count
// layout) ----

// acquireLock takes the lock at a for t if it's free or t's already.
func (m *Machine) acquireLock(a uint32, t *Thread) bool {
	mem := m.Mem
	switch mem.Read32(a + mutexOwner) {
	case 0:
		mem.Write32(a+mutexOwner, t.Addr)
		mem.Write32(a+mutexCount, 1)
	case t.Addr:
		mem.Write32(a+mutexCount, mem.Read32(a+mutexCount)+1)
	default:
		return false
	}
	return true
}

// passLock gives up a lock: the first thread waiting on key gets it.
func (m *Machine) passLock(a uint32, key uint64) {
	mem := m.Mem
	mem.Write32(a+mutexOwner, 0)
	next := m.wakeOne(key)
	if next == nil {
		return
	}
	mem.Write32(a+mutexOwner, next.Addr)
	mem.Write32(a+mutexCount, 1)
	next.Granted = true
}

// dropLock undoes one level of t's hold on the lock; at the last level the
// lock is passed on.
func (m *Machine) dropLock(a uint32, t *Thread, key uint64, reset func()) {
	mem := m.Mem
	depth := mem.Read32(a + mutexCount)
	if depth == 0 || mem.Read32(a+mutexOwner) != t.Addr {
		return
	}
	mem.Write32(a+mutexCount, depth-1)
	if depth == 1 {
		if reset != nil {
			reset()
		}
		m.passLock(a, key)
	}
}

func mutexKey(a uint32) uint64     { return keyMutex | uint64(a) }
func fastMutexKey(a uint32) uint64 { return keyFastMtx | uint64(a) }

// tryLock is OSTryLockMutex's test; the mutex is remembered so a thread
// that exits can be made to let go of it.
func (m *Machine) tryLock(a uint32, t *Thread) bool {
	m.mutexes[a] = true
	return m.acquireLock(a, t)
}

// releaseMutex frees an OSMutex whose count is already 0.
func (m *Machine) releaseMutex(a uint32) { m.passLock(a, mutexKey(a)) }

func (m *Machine) unlock(a uint32, t *Thread) { m.dropLock(a, t, mutexKey(a), nil) }

// heldMutexes lists, in address order, the OSMutexes t owns.
func (m *Machine) heldMutexes(t *Thread) []uint32 {
	var held []uint32
	for a := range m.mutexes {
		if m.Mem.Read32(a) == mutexTag && m.Mem.Read32(a+mutexOwner) == t.Addr {
			held = append(held, a)
		}
	}
	slices.Sort(held)
	return held
}

func (m *Machine) fastLock(a uint32, t *Thread) bool { return m.acquireLock(a, t) }

func (m *Machine) releaseFastMutex(a uint32) {
	m.Mem.Write32(a+fmCount, 0)
	m.passLock(a, fastMutexKey(a))
}

// ---- spin lock helpers ----

// boost lifts a thread holding a spin lock to the highest priority while
// it holds it, so a waiter spinning on the same core can't starve it.
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

// retry yields and runs the current call again after the other threads of
// the same priority have had their turn.
func (m *Machine) retry(t *Thread) {
	m.runSeq++
	t.LastRun = m.runSeq
	t.yield()
}

// spin lock layout: { owner, self, recursion, interrupts were on }
const (
	slOwner  = 0x0
	slSelf   = 0x4
	slDepth  = 0x8
	slIntsOn = 0xc
	slBytes  = 0x10
)

// spinTake tries to take the spin lock at a. Uninterruptible locks turn
// t's interrupts off on first acquisition, remembering whether they were on.
func (m *Machine) spinTake(a uint32, t *Thread, uninterruptible bool) bool {
	mem := m.Mem
	switch mem.Read32(a + slOwner) {
	case t.Addr:
		mem.Write32(a+slDepth, mem.Read32(a+slDepth)+1)
		return true
	case 0:
		mem.Write32(a+slOwner, t.Addr)
		m.boost(t)
		if uninterruptible {
			mem.Write32(a+slIntsOn, b2u(!t.IntsOff))
			t.IntsOff = true
		}
		return true
	}
	return false
}

func (m *Machine) spinGive(a uint32, t *Thread, uninterruptible bool) {
	mem := m.Mem
	if d := mem.Read32(a + slDepth); d > 0 {
		mem.Write32(a+slDepth, d-1)
		return
	}
	if mem.Read32(a+slOwner) != t.Addr {
		return
	}
	if uninterruptible {
		t.IntsOff = mem.Read32(a+slIntsOn) == 0
		mem.Write32(a+slIntsOn, 1)
	}
	mem.Write32(a+slOwner, 0)
	m.deboost(t)
}

// ---- message queues ----

func (m *Machine) mqSlot(q, i uint32) uint32 { return m.Mem.Read32(q+mqMsgs) + i*msgBytes }

func init() {
	const c = "coreinit"
	mem := func(m *Machine) *Memory { return m.Mem }

	// header writes the common start of a sync object: tag, user data, and
	// a pointer back to itself at selfAt (its thread queue's parent).
	header := func(m *Machine, a, size, tag, user, selfAt uint32) {
		m.Mem.Fill(a, 0, size)
		m.Mem.Write32(a, tag)
		m.Mem.Write32(a+4, user)
		if selfAt != 0 {
			m.Mem.Write32(a+selfAt, a)
		}
	}

	// ---- OSMutex ----
	newMutex := func(m *Machine, a, user uint32) {
		header(m, a, 0x2c, mutexTag, user, 0x14)
		m.mutexes[a] = true
	}
	reg(c, "OSInitMutex", func(m *Machine, cpu *CPU, t *Thread) { newMutex(m, arg(cpu, 0), 0) })
	reg(c, "OSInitMutexEx", func(m *Machine, cpu *CPU, t *Thread) { newMutex(m, arg(cpu, 0), arg(cpu, 1)) })
	reg(c, "OSLockMutex", func(m *Machine, cpu *CPU, t *Thread) {
		a := arg(cpu, 0)
		switch {
		case t.Granted: // the previous owner handed it over
		case t.Cancel:
			m.osExitThread(cpu, t, 0xffffffff)
		case !m.tryLock(a, t):
			t.block(mutexKey(a), 0)
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

	// ---- condition variables (OSCond and OSFastCond) ----
	// Waiting releases the lock completely, sleeps until signalled (a
	// signal wakes every waiter), then takes the lock back at its old depth.
	newCond := func(m *Machine, a, tag, user uint32) { header(m, a, 0x1c, tag, user, 0x14) }
	type lockOps struct {
		condKey, lockKey func(uint32) uint64
		take             func(m *Machine, a uint32, t *Thread) bool
		release          func(m *Machine, a uint32)
		depth            uint32
	}
	condWait := func(ops lockOps) hleFunc {
		return func(m *Machine, cpu *CPU, t *Thread) {
			cond, lock := arg(cpu, 0), arg(cpu, 1)
			if t.Phase == 0 {
				t.Saved = m.Mem.Read32(lock + ops.depth)
				ops.release(m, lock)
				t.Phase = 1
				t.block(ops.condKey(cond), 0)
				return
			}
			// signalled (phase 1), or back after waiting for the lock (2)
			t.Phase = 2
			if !t.Granted && !ops.take(m, lock, t) {
				t.block(ops.lockKey(lock), 0)
				return
			}
			m.Mem.Write32(lock+ops.depth, max(t.Saved, 1))
		}
	}
	reg(c, "OSInitCond", func(m *Machine, cpu *CPU, t *Thread) { newCond(m, arg(cpu, 0), condTag, 0) })
	reg(c, "OSInitCondEx", func(m *Machine, cpu *CPU, t *Thread) { newCond(m, arg(cpu, 0), condTag, arg(cpu, 1)) })
	reg(c, "OSWaitCond", condWait(lockOps{
		condKey: func(a uint32) uint64 { return keyCond | uint64(a) },
		lockKey: mutexKey,
		take:    func(m *Machine, a uint32, t *Thread) bool { return m.tryLock(a, t) },
		release: func(m *Machine, a uint32) {
			m.Mem.Write32(a+mutexCount, 0)
			m.releaseMutex(a)
		},
		depth: mutexCount,
	}))
	reg(c, "OSSignalCond", func(m *Machine, cpu *CPU, t *Thread) { m.wake(keyCond | uint64(arg(cpu, 0))) })

	// ---- OSSemaphore ----
	newSem := func(m *Machine, a, count, user uint32) {
		header(m, a, 0x20, semTag, user, 0x18)
		m.Mem.Write32(a+semCount, count)
	}
	reg(c, "OSInitSemaphore", func(m *Machine, cpu *CPU, t *Thread) { newSem(m, arg(cpu, 0), arg(cpu, 1), 0) })
	reg(c, "OSInitSemaphoreEx", func(m *Machine, cpu *CPU, t *Thread) { newSem(m, arg(cpu, 0), arg(cpu, 1), arg(cpu, 2)) })
	// the wait calls return the count from before they took a unit
	reg(c, "OSWaitSemaphore", func(m *Machine, cpu *CPU, t *Thread) {
		if t.Granted {
			ret(cpu, t.GrantVal)
			return
		}
		a := arg(cpu, 0)
		n := int32(mem(m).Read32(a + semCount))
		if n <= 0 {
			t.block(keySem|uint64(a), 0)
			return
		}
		mem(m).Write32(a+semCount, uint32(n-1))
		ret(cpu, uint32(n))
	})
	reg(c, "OSTryWaitSemaphore", func(m *Machine, cpu *CPU, t *Thread) {
		a := arg(cpu, 0)
		n := int32(mem(m).Read32(a + semCount))
		if n > 0 {
			mem(m).Write32(a+semCount, uint32(n-1))
		}
		ret(cpu, uint32(n))
	})
	// OSSignalSemaphore adds a unit, handing it straight to the first
	// waiter if there is one; it returns the count from before
	reg(c, "OSSignalSemaphore", func(m *Machine, cpu *CPU, t *Thread) {
		a := arg(cpu, 0)
		before := int32(mem(m).Read32(a + semCount))
		after := before + 1
		mem(m).Write32(a+semCount, uint32(after))
		if after > 0 {
			if w := m.wakeOne(keySem | uint64(a)); w != nil {
				w.Granted, w.GrantVal = true, uint32(after)
				mem(m).Write32(a+semCount, uint32(before))
			}
		}
		ret(cpu, uint32(before))
	})
	reg(c, "OSGetSemaphoreCount", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, mem(m).Read32(arg(cpu, 0)+semCount)) })

	// ---- OSEvent ----
	// A manual-reset event, once signalled, lets everyone through until
	// reset. An auto-reset event lets one waiter (or, with SignalAll, every
	// current waiter) through per signal; with nobody waiting it stays
	// signalled for the next thread that waits, which resets it.
	newEvent := func(m *Machine, a, value, mode, user uint32) {
		header(m, a, 0x24, eventTag, user, 0x18)
		m.Mem.Write32(a+eventVal, b2u(value != 0))
		m.Mem.Write32(a+eventMode, mode)
	}
	reg(c, "OSInitEvent", func(m *Machine, cpu *CPU, t *Thread) {
		newEvent(m, arg(cpu, 0), arg(cpu, 1), arg(cpu, 2), 0)
	})
	reg(c, "OSInitEventEx", func(m *Machine, cpu *CPU, t *Thread) {
		newEvent(m, arg(cpu, 0), arg(cpu, 1), arg(cpu, 2), arg(cpu, 3))
	})
	signalEvent := func(all bool) hleFunc {
		return func(m *Machine, cpu *CPU, t *Thread) {
			a := arg(cpu, 0)
			if mem(m).Read32(a+eventVal) != 0 {
				return
			}
			key := keyEvent | uint64(a)
			if mem(m).Read32(a+eventMode) != eventAutoReset {
				mem(m).Write32(a+eventVal, 1)
				m.wake(key)
				return
			}
			released := false
			if all {
				released = m.wake(key) > 0
			} else {
				released = m.wakeOne(key) != nil
			}
			if !released {
				mem(m).Write32(a+eventVal, 1)
			}
		}
	}
	reg(c, "OSSignalEvent", signalEvent(false))
	reg(c, "OSSignalEventAll", signalEvent(true))
	reg(c, "OSResetEvent", func(m *Machine, cpu *CPU, t *Thread) { mem(m).Write32(arg(cpu, 0)+eventVal, 0) })
	// passEvent lets a thread through a signalled event
	passEvent := func(m *Machine, a uint32) bool {
		if mem(m).Read32(a+eventVal) == 0 {
			return false
		}
		if mem(m).Read32(a+eventMode) == eventAutoReset {
			mem(m).Write32(a+eventVal, 0)
		}
		return true
	}
	reg(c, "OSWaitEvent", func(m *Machine, cpu *CPU, t *Thread) {
		a := arg(cpu, 0)
		if !t.Woken && !passEvent(m, a) {
			t.block(keyEvent|uint64(a), 0)
		}
	})
	// OSWaitEventWithTimeout(event, u64 ns): 1 signalled, 0 timed out
	reg(c, "OSWaitEventWithTimeout", func(m *Machine, cpu *CPU, t *Thread) {
		a := arg(cpu, 0)
		if t.Woken {
			ret(cpu, 1)
			return
		}
		if t.TimedOut {
			ret(cpu, 0)
			return
		}
		if passEvent(m, a) {
			ret(cpu, 1)
			return
		}
		ns := arg64(cpu, 2)
		if ns == 0 {
			ret(cpu, 0)
			return
		}
		// end slightly early: programs that measure the wait themselves
		// expect not to have overslept
		if ns < 0x00ffffffffffffff {
			ns = ns * 98 / 100
		}
		t.block(keyEvent|uint64(a), m.timeout(ns))
	})

	// ---- OSRendezvous: { u32 arrived[3], self } ----
	reg(c, "OSInitRendezvous", func(m *Machine, cpu *CPU, t *Thread) {
		a := arg(cpu, 0)
		mem(m).Fill(a, 0, 0x0c)
		mem(m).Write32(a+0x0c, a)
	})
	// OSWaitRendezvous(rv, coreMask) and ...WithTimeout(rv, mask, u64 ns):
	// marks this core arrived and waits for every core in the mask
	rendezvous := func(timed bool) hleFunc {
		return func(m *Machine, cpu *CPU, t *Thread) {
			a, cores := arg(cpu, 0), arg(cpu, 1)
			key := keyRendez | uint64(a)
			if t.Phase == 0 {
				mem(m).Write32(a+uint32(cpu.Core)*4, 1)
				m.wake(key)
				t.Deadline = 0
				if timed {
					t.Deadline = m.timeout(arg64(cpu, 2))
				}
				t.Phase = 1
			}
			all := true
			for core := uint32(0); core < 3; core++ {
				if cores>>core&1 != 0 && mem(m).Read32(a+core*4) == 0 {
					all = false
					break
				}
			}
			switch {
			case all:
				ret(cpu, 1)
			case t.TimedOut:
				ret(cpu, 0)
			default:
				t.block(key, t.Deadline)
			}
		}
	}
	reg(c, "OSWaitRendezvous", rendezvous(false))
	reg(c, "OSWaitRendezvousWithTimeout", rendezvous(true))

	// ---- OSFastMutex / OSFastCond ----
	reg(c, "OSFastMutex_Init", func(m *Machine, cpu *CPU, t *Thread) {
		header(m, arg(cpu, 0), 0x2c, fastMutexTag, arg(cpu, 1), 0)
	})
	reg(c, "OSFastMutex_Lock", func(m *Machine, cpu *CPU, t *Thread) {
		a := arg(cpu, 0)
		if !t.Granted && !m.fastLock(a, t) {
			t.block(fastMutexKey(a), 0)
		}
	})
	reg(c, "OSFastMutex_TryLock", func(m *Machine, cpu *CPU, t *Thread) { retBool(cpu, m.fastLock(arg(cpu, 0), t)) })
	reg(c, "OSFastMutex_Unlock", func(m *Machine, cpu *CPU, t *Thread) {
		a := arg(cpu, 0)
		m.dropLock(a, t, fastMutexKey(a), func() { mem(m).Write32(a+fmCount, 0) })
	})
	reg(c, "OSFastCond_Init", func(m *Machine, cpu *CPU, t *Thread) { newCond(m, arg(cpu, 0), fastCondTag, arg(cpu, 1)) })
	reg(c, "OSFastCond_Wait", condWait(lockOps{
		condKey: func(a uint32) uint64 { return keyFastCnd | uint64(a) },
		lockKey: fastMutexKey,
		take:    func(m *Machine, a uint32, t *Thread) bool { return m.fastLock(a, t) },
		release: func(m *Machine, a uint32) { m.releaseFastMutex(a) },
		depth:   fmCount,
	}))
	reg(c, "OSFastCond_Signal", func(m *Machine, cpu *CPU, t *Thread) { m.wake(keyFastCnd | uint64(arg(cpu, 0))) })

	// ---- spin locks ----
	reg(c, "OSInitSpinLock", func(m *Machine, cpu *CPU, t *Thread) {
		a := arg(cpu, 0)
		mem(m).Fill(a, 0, slBytes)
		mem(m).Write32(a+slSelf, a)
		mem(m).Write32(a+slIntsOn, 1)
	})
	for _, kind := range []struct {
		uninterruptible                   bool
		acquire, try, tryTimeout, release string
	}{
		{false, "OSAcquireSpinLock", "OSTryAcquireSpinLock", "OSTryAcquireSpinLockWithTimeout", "OSReleaseSpinLock"},
		{true, "OSUninterruptibleSpinLock_Acquire", "OSUninterruptibleSpinLock_TryAcquire",
			"OSUninterruptibleSpinLock_TryAcquireWithTimeout", "OSUninterruptibleSpinLock_Release"},
	} {
		u := kind.uninterruptible
		// spin until it's ours (as a yield and retry)
		reg(c, kind.acquire, func(m *Machine, cpu *CPU, t *Thread) {
			if m.spinTake(arg(cpu, 0), t, u) {
				ret(cpu, 1)
				return
			}
			m.retry(t)
		})
		reg(c, kind.try, func(m *Machine, cpu *CPU, t *Thread) {
			retBool(cpu, m.spinTake(arg(cpu, 0), t, u))
		})
		// (lock, u64 ns)
		reg(c, kind.tryTimeout, func(m *Machine, cpu *CPU, t *Thread) {
			if m.spinTake(arg(cpu, 0), t, u) {
				ret(cpu, 1)
				return
			}
			if t.Phase == 0 {
				t.Phase = 1
				t.Deadline = m.timeout(arg64(cpu, 2))
			}
			if t.Deadline != 0 && m.Time() >= t.Deadline {
				ret(cpu, 0)
				return
			}
			m.retry(t)
		})
		reg(c, kind.release, func(m *Machine, cpu *CPU, t *Thread) {
			m.spinGive(arg(cpu, 0), t, u)
			ret(cpu, 1)
		})
	}

	// ---- OSMessageQueue ----
	newQueue := func(m *Machine, q, msgs, capacity, user uint32) {
		header(m, q, 0x3c, mqTag, user, 0x14)
		mem(m).Write32(q+0x24, q) // the second thread queue (senders)
		mem(m).Write32(q+mqMsgs, msgs)
		mem(m).Write32(q+mqSize, capacity)
	}
	reg(c, "OSInitMessageQueue", func(m *Machine, cpu *CPU, t *Thread) {
		newQueue(m, arg(cpu, 0), arg(cpu, 1), arg(cpu, 2), 0)
	})
	reg(c, "OSInitMessageQueueEx", func(m *Machine, cpu *CPU, t *Thread) {
		newQueue(m, arg(cpu, 0), arg(cpu, 1), arg(cpu, 2), arg(cpu, 3))
	})
	// OSSendMessage / OSJamMessage(queue, msg, flags): 1 sent, 0 full
	post := func(toFront bool) hleFunc {
		return func(m *Machine, cpu *CPU, t *Thread) {
			q, msg, flags := arg(cpu, 0), arg(cpu, 1), arg(cpu, 2)
			mm := mem(m)
			capacity, count := mm.Read32(q+mqSize), mm.Read32(q+mqUsed)
			if count >= capacity {
				if flags&msgBlock != 0 {
					t.block(keyMsgSend|uint64(q), 0)
				} else {
					ret(cpu, 0)
				}
				return
			}
			head := mm.Read32(q + mqFirst)
			var slot uint32
			if toFront || flags&msgHigh != 0 {
				head = (head + capacity - 1) % capacity
				mm.Write32(q+mqFirst, head)
				slot = head
			} else {
				slot = (head + count) % capacity
			}
			mm.Move(m.mqSlot(q, slot), msg, msgBytes)
			mm.Write32(q+mqUsed, count+1)
			m.wakeOne(keyMsgRecv | uint64(q))
			ret(cpu, 1)
		}
	}
	reg(c, "OSSendMessage", post(false))
	reg(c, "OSJamMessage", post(true))
	// OSReceiveMessage(queue, msg, flags): 1 received, 0 empty
	reg(c, "OSReceiveMessage", func(m *Machine, cpu *CPU, t *Thread) {
		q, msg, flags := arg(cpu, 0), arg(cpu, 1), arg(cpu, 2)
		mm := mem(m)
		count := mm.Read32(q + mqUsed)
		if count == 0 {
			if flags&msgBlock != 0 {
				t.block(keyMsgRecv|uint64(q), 0)
			} else {
				ret(cpu, 0)
			}
			return
		}
		head := mm.Read32(q + mqFirst)
		mm.Move(msg, m.mqSlot(q, head), msgBytes)
		mm.Write32(q+mqFirst, (head+1)%mm.Read32(q+mqSize))
		mm.Write32(q+mqUsed, count-1)
		m.wakeOne(keyMsgSend | uint64(q))
		ret(cpu, 1)
	})
	// OSPeekMessage(queue, msg): the oldest message without removing it
	reg(c, "OSPeekMessage", func(m *Machine, cpu *CPU, t *Thread) {
		q, msg := arg(cpu, 0), arg(cpu, 1)
		if mem(m).Read32(q+mqUsed) == 0 {
			ret(cpu, 0)
			return
		}
		if msg != 0 {
			mem(m).Move(msg, m.mqSlot(q, mem(m).Read32(q+mqFirst)), msgBytes)
		}
		ret(cpu, 1)
	})
}
