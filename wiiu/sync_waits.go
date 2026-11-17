package wiiu

// coreinit semaphores, events, rendezvous and message queues.
//
// Written for RetroPixels from the SDK's object layouts and the behaviour
// programs expect, on RetroPixels' own thread kernel (kernel.go).

const (
	// OSSemaphore
	semTag   = 's'<<24 | 'P'<<16 | 'h'<<8 | 'R'
	semSize  = 0x20
	semCount = 0x0c
	semQueue = 0x18

	// OSEvent
	eventTag   = 'e'<<24 | 'V'<<16 | 'n'<<8 | 'T'
	eventSize  = 0x24
	eventSet   = 0x0c
	eventQueue = 0x18
	eventMode  = 0x20 // eventManual or eventAuto
	eventAuto  = 1

	// OSMessageQueue; a message is 16 bytes
	mqTag    = 'm'<<24 | 'S'<<16 | 'g'<<8 | 'Q'
	mqBytes  = 0x3c
	mqMsgs   = 0x2c
	mqSize   = 0x30 // how many messages fit
	mqFirst  = 0x34
	mqUsed   = 0x38
	mqMsgLen = 16

	msgBlock = 1 // wait for room or for a message
	msgHigh  = 2 // send in front of the waiting messages
)

// nsToTicks converts nanoseconds to time base ticks: TimerHz is
// 31250 * 1989, so a tick is 32000/1989 ns.
func nsToTicks(ns uint64) uint64 { return TimerHz / 31250 * ns / 32000 }

// timeout is the time base value at which a wait of ns nanoseconds ends, or
// 0 for a wait too long ever to end.
func (m *Machine) timeout(ns uint64) uint64 {
	if ns >= 1<<52 {
		return 0
	}
	return m.Time() + max(nsToTicks(ns), 1)
}

// semTake takes one from the semaphore at a if it has any, returning the
// count it had.
func (m *Machine) semTake(a uint32) int32 {
	n := int32(m.Mem.Read32(a + semCount))
	if n > 0 {
		m.Mem.Write32(a+semCount, uint32(n-1))
	}
	return n
}

// eventPass lets a thread through a set event, unsetting an auto-reset one.
func (m *Machine) eventPass(a uint32) bool {
	mem := m.Mem
	if mem.Read32(a+eventSet) == 0 {
		return false
	}
	if mem.Read32(a+eventMode) == eventAuto {
		mem.Write32(a+eventSet, 0)
	}
	return true
}

// mqSlot is the address of message slot i of the queue at q.
func (m *Machine) mqSlot(q, i uint32) uint32 { return m.Mem.Read32(q+mqMsgs) + i*mqMsgLen }

func init() {
	c := "coreinit"

	initSem := func(m *Machine, a, count, userData uint32) {
		m.Mem.Fill(a, 0, semSize)
		m.Mem.Write32(a, semTag)
		m.Mem.Write32(a+4, userData)
		m.Mem.Write32(a+semCount, count)
		m.Mem.Write32(a+semQueue, a)
	}
	reg(c, "OSInitSemaphore", func(m *Machine, cpu *CPU, t *Thread) { initSem(m, arg(cpu, 0), arg(cpu, 1), 0) })
	reg(c, "OSInitSemaphoreEx", func(m *Machine, cpu *CPU, t *Thread) { initSem(m, arg(cpu, 0), arg(cpu, 1), arg(cpu, 2)) })
	reg(c, "OSWaitSemaphore", func(m *Machine, cpu *CPU, t *Thread) {
		if t.Granted { // a signaller passed its count straight to us
			ret(cpu, t.GrantVal)
			return
		}
		a := arg(cpu, 0)
		if n := m.semTake(a); n > 0 {
			ret(cpu, uint32(n))
			return
		}
		t.block(keySem|uint64(a), 0)
	})
	reg(c, "OSTryWaitSemaphore", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, uint32(m.semTake(arg(cpu, 0)))) })
	reg(c, "OSSignalSemaphore", func(m *Machine, cpu *CPU, t *Thread) {
		a := arg(cpu, 0)
		n := int32(m.Mem.Read32(a + semCount))
		ret(cpu, uint32(n))
		if n+1 > 0 {
			if w := m.wakeOne(keySem | uint64(a)); w != nil {
				w.Granted, w.GrantVal = true, uint32(n+1)
				return // the count goes to the waiter
			}
		}
		m.Mem.Write32(a+semCount, uint32(n+1))
	})
	reg(c, "OSGetSemaphoreCount", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, m.Mem.Read32(arg(cpu, 0)+semCount)) })

	// Events. A manual-reset event, once set, lets everyone through until
	// it's reset; an auto-reset one lets one waiter through per signal.
	initEvent := func(m *Machine, a, set, mode, userData uint32) {
		m.Mem.Fill(a, 0, eventSize)
		m.Mem.Write32(a, eventTag)
		m.Mem.Write32(a+4, userData)
		m.Mem.Write32(a+eventSet, b2u(set != 0))
		m.Mem.Write32(a+eventQueue, a)
		m.Mem.Write32(a+eventMode, mode)
	}
	reg(c, "OSInitEvent", func(m *Machine, cpu *CPU, t *Thread) { initEvent(m, arg(cpu, 0), arg(cpu, 1), arg(cpu, 2), 0) })
	reg(c, "OSInitEventEx", func(m *Machine, cpu *CPU, t *Thread) {
		initEvent(m, arg(cpu, 0), arg(cpu, 1), arg(cpu, 2), arg(cpu, 3))
	})
	signal := func(everyone bool) hleFunc {
		return func(m *Machine, cpu *CPU, t *Thread) {
			a := arg(cpu, 0)
			if m.Mem.Read32(a+eventSet) != 0 {
				return
			}
			key := keyEvent | uint64(a)
			if m.Mem.Read32(a+eventMode) == eventAuto {
				// waiters go through and leave it unset; with none
				// waiting, it stays set for the next
				if everyone && m.wake(key) > 0 || !everyone && m.wakeOne(key) != nil {
					return
				}
				m.Mem.Write32(a+eventSet, 1)
				return
			}
			m.Mem.Write32(a+eventSet, 1)
			m.wake(key)
		}
	}
	reg(c, "OSSignalEvent", signal(false))
	reg(c, "OSSignalEventAll", signal(true))
	reg(c, "OSResetEvent", func(m *Machine, cpu *CPU, t *Thread) { m.Mem.Write32(arg(cpu, 0)+eventSet, 0) })
	reg(c, "OSWaitEvent", func(m *Machine, cpu *CPU, t *Thread) {
		a := arg(cpu, 0)
		if !t.Woken && !m.eventPass(a) {
			t.block(keyEvent|uint64(a), 0)
		}
	})
	reg(c, "OSWaitEventWithTimeout", func(m *Machine, cpu *CPU, t *Thread) {
		a := arg(cpu, 0)
		if t.Woken || t.TimedOut {
			retBool(cpu, t.Woken)
			return
		}
		if m.eventPass(a) {
			ret(cpu, 1)
			return
		}
		ns := arg64(cpu, 2)
		if ns == 0 {
			ret(cpu, 0)
			return
		}
		// end a little early, for programs that time the wait themselves
		if ns < 1<<56-1 {
			ns = ns * 98 / 100
		}
		t.block(keyEvent|uint64(a), m.timeout(ns))
	})

	// Rendezvous: a flag for each core that has arrived, then user data.
	// The mask names the cores to wait for.
	reg(c, "OSInitRendezvous", func(m *Machine, cpu *CPU, t *Thread) {
		a := arg(cpu, 0)
		m.Mem.Fill(a, 0, 12)
		m.Mem.Write32(a+12, a)
	})
	rendezvous := func(timed bool) hleFunc {
		return func(m *Machine, cpu *CPU, t *Thread) {
			a, cores := arg(cpu, 0), arg(cpu, 1)
			if t.Phase == 0 {
				m.Mem.Write32(a+uint32(cpu.Core)*4, 1)
				m.wake(keyRendez | uint64(a))
				t.Deadline = 0
				if timed {
					t.Deadline = m.timeout(arg64(cpu, 2))
				}
				t.Phase = 1
			}
			all := true
			for core := uint32(0); core < 3; core++ {
				all = all && (cores>>core&1 == 0 || m.Mem.Read32(a+core*4) != 0)
			}
			switch {
			case all:
				ret(cpu, 1)
			case t.TimedOut:
				ret(cpu, 0)
			default:
				t.block(keyRendez|uint64(a), t.Deadline)
			}
		}
	}
	reg(c, "OSWaitRendezvous", rendezvous(false))
	reg(c, "OSWaitRendezvousWithTimeout", rendezvous(true))

	// message queues: a ring of 16-byte messages
	initMQ := func(m *Machine, q, msgs, size, userData uint32) {
		mem := m.Mem
		mem.Fill(q, 0, mqBytes)
		mem.Write32(q, mqTag)
		mem.Write32(q+4, userData)
		mem.Write32(q+0x14, q) // the senders' and receivers' thread queues
		mem.Write32(q+0x24, q)
		mem.Write32(q+mqMsgs, msgs)
		mem.Write32(q+mqSize, size)
	}
	reg(c, "OSInitMessageQueue", func(m *Machine, cpu *CPU, t *Thread) { initMQ(m, arg(cpu, 0), arg(cpu, 1), arg(cpu, 2), 0) })
	reg(c, "OSInitMessageQueueEx", func(m *Machine, cpu *CPU, t *Thread) {
		initMQ(m, arg(cpu, 0), arg(cpu, 1), arg(cpu, 2), arg(cpu, 3))
	})
	send := func(jam bool) hleFunc {
		return func(m *Machine, cpu *CPU, t *Thread) {
			mem := m.Mem
			q, msg, flags := arg(cpu, 0), arg(cpu, 1), arg(cpu, 2)
			size, used, first := mem.Read32(q+mqSize), mem.Read32(q+mqUsed), mem.Read32(q+mqFirst)
			if used >= size {
				if flags&msgBlock != 0 {
					t.block(keyMsgSend|uint64(q), 0)
				} else {
					ret(cpu, 0)
				}
				return
			}
			slot := (first + used) % size
			if jam || flags&msgHigh != 0 {
				slot = (first + size - 1) % size
				mem.Write32(q+mqFirst, slot)
			}
			mem.Move(m.mqSlot(q, slot), msg, mqMsgLen)
			mem.Write32(q+mqUsed, used+1)
			m.wakeOne(keyMsgRecv | uint64(q))
			ret(cpu, 1)
		}
	}
	reg(c, "OSSendMessage", send(false))
	reg(c, "OSJamMessage", send(true))
	reg(c, "OSReceiveMessage", func(m *Machine, cpu *CPU, t *Thread) {
		mem := m.Mem
		q, msg, flags := arg(cpu, 0), arg(cpu, 1), arg(cpu, 2)
		used, first := mem.Read32(q+mqUsed), mem.Read32(q+mqFirst)
		if used == 0 {
			if flags&msgBlock != 0 {
				t.block(keyMsgRecv|uint64(q), 0)
			} else {
				ret(cpu, 0)
			}
			return
		}
		mem.Move(msg, m.mqSlot(q, first), mqMsgLen)
		mem.Write32(q+mqFirst, (first+1)%mem.Read32(q+mqSize))
		mem.Write32(q+mqUsed, used-1)
		m.wakeOne(keyMsgSend | uint64(q))
		ret(cpu, 1)
	})
	reg(c, "OSPeekMessage", func(m *Machine, cpu *CPU, t *Thread) {
		q, msg := arg(cpu, 0), arg(cpu, 1)
		if m.Mem.Read32(q+mqUsed) == 0 {
			ret(cpu, 0)
			return
		}
		if msg != 0 {
			m.Mem.Move(msg, m.mqSlot(q, m.Mem.Read32(q+mqFirst)), mqMsgLen)
		}
		ret(cpu, 1)
	})
}
