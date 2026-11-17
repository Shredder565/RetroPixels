package wiiu

// coreinit mutexes and conditions, in their ordinary and "fast" forms, and
// spin locks.
//
// A lock let go of while threads wait for it is handed straight to the
// first of them. Otherwise a thread that takes it again at once, on its own
// core or between another core's turns, could keep it from them forever.
//
// Written for RetroPixels from the SDK's object layouts and the behaviour
// programs expect, on RetroPixels' own thread kernel (kernel.go).

import "sort"

const (
	// OSMutex
	mutexTag   = 'm'<<24 | 'U'<<16 | 't'<<8 | 'X'
	mutexSize  = 0x2c
	mutexOwner = 0x1c
	mutexCount = 0x20 // how many times its owner holds it

	// OSCondition and OSFastCondition
	condTag     = 'c'<<24 | 'N'<<16 | 'd'<<8 | 'V'
	fastCondTag = 'f'<<24 | 'N'<<16 | 'd'<<8 | 'V'
	condSize    = 0x1c

	fastMutexTag = 'f'<<24 | 'M'<<16 | 't'<<8 | 'X'

	// OSSpinLock: owner, user data, times taken again, whether interrupts
	// were on
	spinOwner  = 0x0
	spinAgain  = 0x8
	spinIntsOn = 0xc
	spinSize   = 0x10

	// a thread queue's user data, set to the object it belongs to
	waitQueueOwner = 0x14
)

// lockKind is where a kind of mutex keeps its owner and count, and the key
// its waiters block on. Fast mutexes share the ordinary layout.
type lockKind struct {
	key     uint64
	tracked bool // RetroPixels lists these so a thread's exit frees them
}

var (
	osMutex   = lockKind{keyMutex, true}
	fastMutex = lockKind{keyFastMtx, false}
)

// take locks the mutex at a for t, or counts once more if t has it.
func (k lockKind) take(m *Machine, a uint32, t *Thread) bool {
	mem := m.Mem
	if k.tracked {
		m.mutexes[a] = true
	}
	switch owner := mem.Read32(a + mutexOwner); owner {
	case t.Addr:
		mem.Write32(a+mutexCount, mem.Read32(a+mutexCount)+1)
	case 0:
		mem.Write32(a+mutexOwner, t.Addr)
		mem.Write32(a+mutexCount, 1)
	default:
		return false
	}
	return true
}

// release frees the mutex at a and hands it to its first waiter, if any.
func (k lockKind) release(m *Machine, a uint32) {
	mem := m.Mem
	mem.Write32(a+mutexOwner, 0)
	mem.Write32(a+mutexCount, 0)
	w := m.wakeOne(k.key | uint64(a))
	if w == nil {
		return
	}
	mem.Write32(a+mutexOwner, w.Addr)
	mem.Write32(a+mutexCount, 1)
	w.Granted = true
}

// drop is one unlock by t; the last one releases the mutex.
func (k lockKind) drop(m *Machine, a uint32, t *Thread) {
	mem := m.Mem
	n := mem.Read32(a + mutexCount)
	if n == 0 || mem.Read32(a+mutexOwner) != t.Addr {
		return
	}
	if n == 1 {
		k.release(m, a)
		return
	}
	mem.Write32(a+mutexCount, n-1)
}

// lock is a blocking lock call. A waiter that was handed the mutex just
// returns.
func (k lockKind) lock(m *Machine, t *Thread, a uint32) {
	if !t.Granted && !k.take(m, a, t) {
		t.block(k.key|uint64(a), 0)
	}
}

// waitCond is a condition wait: let go of the mutex entirely, wait to be
// signalled, then take the mutex back as many times as it was held.
func (k lockKind) waitCond(m *Machine, t *Thread, condKey uint64, mx uint32) {
	if t.Phase == 0 {
		t.Saved = m.Mem.Read32(mx + mutexCount)
		k.release(m, mx)
		t.block(condKey, 0)
		t.Phase = 1
		return
	}
	t.Phase = 2
	if !t.Granted && !k.take(m, mx, t) {
		t.block(k.key|uint64(mx), 0)
		return
	}
	m.Mem.Write32(mx+mutexCount, max(t.Saved, 1))
}

// releaseMutex frees an ordinary mutex, for a thread that exits holding it.
func (m *Machine) releaseMutex(a uint32) { osMutex.release(m, a) }

// heldMutexes lists the ordinary mutexes t owns, lowest address first.
func (m *Machine) heldMutexes(t *Thread) []uint32 {
	var held []uint32
	for a := range m.mutexes {
		if m.Mem.Read32(a) == mutexTag && m.Mem.Read32(a+mutexOwner) == t.Addr {
			held = append(held, a)
		}
	}
	sort.Slice(held, func(i, j int) bool { return held[i] < held[j] })
	return held
}

// spinTake takes the spin lock at a for t: got says it has it, again that it
// already did. Holding one raises t to the top priority, so a thread
// spinning for it on t's core can't starve t.
func (m *Machine) spinTake(a uint32, t *Thread, uninterruptible bool) (got bool) {
	mem := m.Mem
	switch mem.Read32(a + spinOwner) {
	case t.Addr:
		mem.Write32(a+spinAgain, mem.Read32(a+spinAgain)+1)
		return true
	case 0:
		mem.Write32(a+spinOwner, t.Addr)
		t.Boost++
		m.updatePriority(t)
		if uninterruptible {
			mem.Write32(a+spinIntsOn, b2u(!t.IntsOff))
			t.IntsOff = true
		}
		return true
	}
	return false
}

func (m *Machine) spinGive(a uint32, t *Thread, uninterruptible bool) {
	mem := m.Mem
	if n := mem.Read32(a + spinAgain); n > 0 {
		mem.Write32(a+spinAgain, n-1)
		return
	}
	if mem.Read32(a+spinOwner) != t.Addr {
		return
	}
	if uninterruptible {
		t.IntsOff = mem.Read32(a+spinIntsOn) == 0
		mem.Write32(a+spinIntsOn, 1)
	}
	mem.Write32(a+spinOwner, 0)
	t.Boost = max(t.Boost-1, 0)
	m.updatePriority(t)
}

// spinAgainLater runs the current call again once threads of the same
// priority have had a turn.
func (m *Machine) spinAgainLater(t *Thread) {
	m.runSeq++
	t.LastRun = m.runSeq
	t.yield()
}

func init() {
	c := "coreinit"
	cancelled := func(m *Machine, cpu *CPU, t *Thread) bool {
		if t.Cancel {
			m.osExitThread(cpu, t, 0xffffffff)
		}
		return t.Cancel
	}
	header := func(m *Machine, a, size, tag, userData uint32) {
		m.Mem.Fill(a, 0, size)
		m.Mem.Write32(a, tag)
		m.Mem.Write32(a+4, userData)
	}

	initMutex := func(m *Machine, a, userData uint32) {
		header(m, a, mutexSize, mutexTag, userData)
		m.Mem.Write32(a+waitQueueOwner, a)
		m.mutexes[a] = true
	}
	reg(c, "OSInitMutex", func(m *Machine, cpu *CPU, t *Thread) { initMutex(m, arg(cpu, 0), 0) })
	reg(c, "OSInitMutexEx", func(m *Machine, cpu *CPU, t *Thread) { initMutex(m, arg(cpu, 0), arg(cpu, 1)) })
	reg(c, "OSLockMutex", func(m *Machine, cpu *CPU, t *Thread) {
		if t.Granted || cancelled(m, cpu, t) {
			return
		}
		osMutex.lock(m, t, arg(cpu, 0))
	})
	reg(c, "OSTryLockMutex", func(m *Machine, cpu *CPU, t *Thread) {
		if !cancelled(m, cpu, t) {
			retBool(cpu, osMutex.take(m, arg(cpu, 0), t))
		}
	})
	reg(c, "OSUnlockMutex", func(m *Machine, cpu *CPU, t *Thread) { osMutex.drop(m, arg(cpu, 0), t) })

	// a condition signal wakes every waiter
	initCond := func(m *Machine, a, tag, userData uint32) {
		header(m, a, condSize, tag, userData)
		m.Mem.Write32(a+waitQueueOwner, a)
	}
	reg(c, "OSInitCond", func(m *Machine, cpu *CPU, t *Thread) { initCond(m, arg(cpu, 0), condTag, 0) })
	reg(c, "OSInitCondEx", func(m *Machine, cpu *CPU, t *Thread) { initCond(m, arg(cpu, 0), condTag, arg(cpu, 1)) })
	reg(c, "OSWaitCond", func(m *Machine, cpu *CPU, t *Thread) {
		osMutex.waitCond(m, t, keyCond|uint64(arg(cpu, 0)), arg(cpu, 1))
	})
	reg(c, "OSSignalCond", func(m *Machine, cpu *CPU, t *Thread) { m.wake(keyCond | uint64(arg(cpu, 0))) })

	reg(c, "OSFastMutex_Init", func(m *Machine, cpu *CPU, t *Thread) {
		header(m, arg(cpu, 0), mutexSize, fastMutexTag, arg(cpu, 1))
	})
	reg(c, "OSFastMutex_Lock", func(m *Machine, cpu *CPU, t *Thread) { fastMutex.lock(m, t, arg(cpu, 0)) })
	reg(c, "OSFastMutex_TryLock", func(m *Machine, cpu *CPU, t *Thread) { retBool(cpu, fastMutex.take(m, arg(cpu, 0), t)) })
	reg(c, "OSFastMutex_Unlock", func(m *Machine, cpu *CPU, t *Thread) { fastMutex.drop(m, arg(cpu, 0), t) })
	reg(c, "OSFastCond_Init", func(m *Machine, cpu *CPU, t *Thread) { initCond(m, arg(cpu, 0), fastCondTag, arg(cpu, 1)) })
	reg(c, "OSFastCond_Wait", func(m *Machine, cpu *CPU, t *Thread) {
		fastMutex.waitCond(m, t, keyFastCnd|uint64(arg(cpu, 0)), arg(cpu, 1))
	})
	reg(c, "OSFastCond_Signal", func(m *Machine, cpu *CPU, t *Thread) { m.wake(keyFastCnd | uint64(arg(cpu, 0))) })

	// Spin locks. The uninterruptible kind also keeps other threads off the
	// core while held.
	reg(c, "OSInitSpinLock", func(m *Machine, cpu *CPU, t *Thread) {
		a := arg(cpu, 0)
		m.Mem.Fill(a, 0, spinSize)
		m.Mem.Write32(a+4, a)
		m.Mem.Write32(a+spinIntsOn, 1)
	})
	for _, kind := range []struct {
		acquire, try, timed, release string
		unint                        bool
	}{
		{"OSAcquireSpinLock", "OSTryAcquireSpinLock", "OSTryAcquireSpinLockWithTimeout", "OSReleaseSpinLock", false},
		{"OSUninterruptibleSpinLock_Acquire", "OSUninterruptibleSpinLock_TryAcquire",
			"OSUninterruptibleSpinLock_TryAcquireWithTimeout", "OSUninterruptibleSpinLock_Release", true},
	} {
		unint := kind.unint
		reg(c, kind.acquire, func(m *Machine, cpu *CPU, t *Thread) {
			if m.spinTake(arg(cpu, 0), t, unint) {
				ret(cpu, 1)
				return
			}
			m.spinAgainLater(t)
		})
		reg(c, kind.try, func(m *Machine, cpu *CPU, t *Thread) { retBool(cpu, m.spinTake(arg(cpu, 0), t, unint)) })
		reg(c, kind.timed, func(m *Machine, cpu *CPU, t *Thread) {
			if m.spinTake(arg(cpu, 0), t, unint) {
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
			m.spinAgainLater(t)
		})
		reg(c, kind.release, func(m *Machine, cpu *CPU, t *Thread) {
			m.spinGive(arg(cpu, 0), t, unint)
			ret(cpu, 1)
		})
	}
}
