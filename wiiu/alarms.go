package wiiu

// coreinit alarms. When an alarm's time comes its handler(alarm, context)
// runs on the callback thread of the core that set it, and a periodic alarm
// comes round again each period. An alarm set with an alarm queue
// (OSSetAlarmEx and OSSetPeriodicAlarmEx) is put in the queue instead, for a
// thread to collect with OSGetAlarmFromQueue.
//
// Written for RetroPixels from the SDK's OSAlarm and OSAlarmQueue layouts
// and the behaviour programs expect.

import "math"

const (
	// OSAlarm
	osAlarmTag      = 'a'<<24 | 'L'<<16 | 'r'<<8 | 'M'
	osAlarmSize     = 0x58
	osAlarmHandler  = 0x0c
	osAlarmGroup    = 0x10
	osAlarmFireAt   = 0x18 // OSTime of the next firing
	osAlarmQPrev    = 0x20 // neighbours while waiting in a queue
	osAlarmQNext    = 0x24
	osAlarmPeriod   = 0x28
	osAlarmStart    = 0x30
	osAlarmUserData = 0x38
	osAlarmState    = 0x3c // RetroPixels' own use: alarmSet, alarmDone or alarmCancelled
	osAlarmQueue    = 0x50
	osAlarmContext  = 0x54

	alarmSet       = 1
	alarmDone      = 2
	alarmCancelled = 3

	// OSAlarmQueue
	osAlarmQueueTag  = 'a'<<24 | 'L'<<16 | 'm'<<8 | 'Q'
	osAlarmQueueSize = 0x24
	osAlarmQueueSelf = 0x14
	osAlarmQueueHead = 0x1c
	osAlarmQueueTail = 0x20
)

// Alarm is an OSAlarm that has been set at least once.
type Alarm struct {
	Addr   uint32
	Fire   uint64 // time base ticks since boot
	Period uint64 // 0 for a one-shot
	Core   int
	Active bool
}

// alarm finds the record for the OSAlarm at a.
func (m *Machine) alarm(a uint32) *Alarm {
	for _, al := range m.alarms {
		if al.Addr == a {
			return al
		}
	}
	return nil
}

func (m *Machine) initAlarm(a, name uint32) {
	if al := m.alarm(a); al != nil {
		al.Active = false
	}
	m.Mem.Fill(a, 0, osAlarmSize)
	m.Mem.Write32(a, osAlarmTag)
	m.Mem.Write32(a+4, name)
}

// queuedAlarm reports whether the alarm at a fired and still waits in its
// queue.
func (m *Machine) queuedAlarm(a uint32) bool {
	q := m.Mem.Read32(a + osAlarmQueue)
	return q != 0 && (m.Mem.Read32(a+osAlarmQPrev) != 0 || m.Mem.Read32(q+osAlarmQueueHead) == a)
}

// enqueueAlarm puts the alarm at the back of its queue and wakes a thread
// waiting on the queue.
func (m *Machine) enqueueAlarm(a uint32) {
	mem := m.Mem
	q := mem.Read32(a + osAlarmQueue)
	last := mem.Read32(q + osAlarmQueueTail)
	mem.Write32(a+osAlarmQPrev, last)
	mem.Write32(a+osAlarmQNext, 0)
	if last == 0 {
		mem.Write32(q+osAlarmQueueHead, a)
	} else {
		mem.Write32(last+osAlarmQNext, a)
	}
	mem.Write32(q+osAlarmQueueTail, a)
	m.wake(keyAlarmQ | uint64(q))
}

func (m *Machine) dequeueAlarm(a uint32) {
	mem := m.Mem
	q := mem.Read32(a + osAlarmQueue)
	prev, next := mem.Read32(a+osAlarmQPrev), mem.Read32(a+osAlarmQNext)
	if prev == 0 {
		mem.Write32(q+osAlarmQueueHead, next)
	} else {
		mem.Write32(prev+osAlarmQNext, next)
	}
	if next == 0 {
		mem.Write32(q+osAlarmQueueTail, prev)
	} else {
		mem.Write32(next+osAlarmQPrev, prev)
	}
	mem.Write32(a+osAlarmQPrev, 0)
	mem.Write32(a+osAlarmQNext, 0)
}

// armAlarm sets the alarm at a for the OSTime at, then (with period non-zero)
// every period. A periodic alarm whose start has passed first fires at the
// next whole period from start.
func (m *Machine) armAlarm(core int, a uint32, at, period uint64, periodic bool, handler, queue uint32) {
	if m.queuedAlarm(a) {
		return // it fired and hasn't been collected yet
	}
	mem := m.Mem
	fireAt := at
	if !periodic {
		period = 0
	} else {
		if int64(period) <= 0 {
			return
		}
		if now := m.bootTime + m.Time(); at < now {
			fireAt = at + ((now-at)/period+1)*period
		}
		mem.Write64(a+osAlarmStart, at)
	}
	mem.Write64(a+osAlarmFireAt, fireAt)
	mem.Write64(a+osAlarmPeriod, period)
	mem.Write32(a+osAlarmHandler, handler)
	mem.Write32(a+osAlarmQueue, queue)
	mem.Write32(a+osAlarmState, alarmSet)

	al := m.alarm(a)
	if al == nil {
		al = &Alarm{Addr: a}
		m.alarms = append(m.alarms, al)
	}
	al.Fire = 0
	if fireAt >= m.bootTime {
		al.Fire = fireAt - m.bootTime
	}
	al.Period, al.Core, al.Active = period, core, true
	m.alarmThread(core)
}

// disarmAlarm is OSCancelAlarm: an alarm still to fire stops, and one waiting
// in its queue leaves it.
func (m *Machine) disarmAlarm(a uint32) bool {
	switch al := m.alarm(a); {
	case al != nil && al.Active:
		al.Active = false
		m.Mem.Write32(a+osAlarmState, alarmCancelled)
		m.wake(keyAlarm | uint64(a))
		return true
	case m.queuedAlarm(a):
		m.dequeueAlarm(a)
		return true
	}
	return false
}

// runAlarms fires every alarm that is due; a periodic alarm fires once for
// each period that has gone by, unless it has fallen far behind.
func (m *Machine) runAlarms() {
	now := m.tb
	for _, al := range m.alarms {
		for al.Active && al.Fire <= now {
			if !m.fireAlarm(al, now) {
				break
			}
		}
	}
}

// fireAlarm fires one alarm and reports whether it is due to come round
// again.
func (m *Machine) fireAlarm(al *Alarm, now uint64) bool {
	mem := m.Mem
	a := al.Addr
	if mem.Read32(a+osAlarmQueue) != 0 {
		// to the queue; collecting it sets a periodic alarm again
		al.Active = false
		mem.Write32(a+osAlarmContext, m.alarmContext)
		m.enqueueAlarm(a)
		return false
	}
	m.queueCallback(al.Core, mem.Read32(a+osAlarmHandler), a, m.alarmContext)
	if al.Period == 0 {
		al.Active = false
		mem.Write32(a+osAlarmState, alarmDone)
		m.wake(keyAlarm | uint64(a))
		return false
	}
	al.Fire += al.Period
	if al.Fire+100*al.Period < now {
		al.Fire = now + al.Period // too far behind to catch up period by period
	}
	mem.Write64(a+osAlarmFireAt, m.bootTime+al.Fire)
	return true
}

// nextAlarm is when the next alarm is due (at least 1), or 0 for none.
func (m *Machine) nextAlarm() uint64 {
	soonest := uint64(math.MaxUint64)
	for _, al := range m.alarms {
		if al.Active {
			soonest = min(soonest, al.Fire)
		}
	}
	if soonest == math.MaxUint64 {
		return 0
	}
	return max(soonest, 1)
}

func init() {
	c := "coreinit"

	reg(c, "OSCreateAlarm", func(m *Machine, cpu *CPU, t *Thread) { m.initAlarm(arg(cpu, 0), 0) })
	reg(c, "OSCreateAlarmEx", func(m *Machine, cpu *CPU, t *Thread) { m.initAlarm(arg(cpu, 0), arg(cpu, 1)) })

	// OSSetAlarm(alarm, delay, handler) and OSSetAlarmEx(..., queue): the
	// 64-bit delay takes r5 and r6
	oneShot := func(m *Machine, cpu *CPU, queue uint32) {
		if delay := arg64(cpu, 2); int64(delay) > 0 {
			m.armAlarm(cpu.Core, arg(cpu, 0), m.bootTime+m.Time()+delay, 0, false, arg(cpu, 4), queue)
		}
		ret(cpu, 1)
	}
	reg(c, "OSSetAlarm", func(m *Machine, cpu *CPU, t *Thread) { oneShot(m, cpu, 0) })
	reg(c, "OSSetAlarmEx", func(m *Machine, cpu *CPU, t *Thread) { oneShot(m, cpu, arg(cpu, 5)) })

	// OSSetPeriodicAlarm(alarm, start, period, handler) and the Ex form with
	// a queue
	periodic := func(m *Machine, cpu *CPU, queue uint32) {
		m.armAlarm(cpu.Core, arg(cpu, 0), arg64(cpu, 2), arg64(cpu, 4), true, arg(cpu, 6), queue)
		ret(cpu, 1)
	}
	reg(c, "OSSetPeriodicAlarm", func(m *Machine, cpu *CPU, t *Thread) { periodic(m, cpu, 0) })
	reg(c, "OSSetPeriodicAlarmEx", func(m *Machine, cpu *CPU, t *Thread) { periodic(m, cpu, arg(cpu, 7)) })

	reg(c, "OSCancelAlarm", func(m *Machine, cpu *CPU, t *Thread) { retBool(cpu, m.disarmAlarm(arg(cpu, 0))) })
	reg(c, "OSCancelAlarms", func(m *Machine, cpu *CPU, t *Thread) {
		group := arg(cpu, 0)
		if group == 0 || group == 0xffffffff {
			return
		}
		for _, al := range m.alarms {
			if al.Active && m.Mem.Read32(al.Addr+osAlarmGroup) == group {
				m.disarmAlarm(al.Addr)
			}
		}
	})
	reg(c, "OSSetAlarmTag", func(m *Machine, cpu *CPU, t *Thread) {
		if group := arg(cpu, 1); group != 0xffffffff {
			m.Mem.Write32(arg(cpu, 0)+osAlarmGroup, group)
		}
	})
	reg(c, "OSSetAlarmUserData", func(m *Machine, cpu *CPU, t *Thread) {
		m.Mem.Write32(arg(cpu, 0)+osAlarmUserData, arg(cpu, 1))
	})
	reg(c, "OSGetAlarmUserData", func(m *Machine, cpu *CPU, t *Thread) {
		ret(cpu, m.Mem.Read32(arg(cpu, 0)+osAlarmUserData))
	})

	// OSWaitAlarm blocks until the alarm fires (true) or is cancelled
	reg(c, "OSWaitAlarm", func(m *Machine, cpu *CPU, t *Thread) {
		a := arg(cpu, 0)
		if t.Woken {
			retBool(cpu, m.Mem.Read32(a+osAlarmState) != alarmCancelled)
			return
		}
		if al := m.alarm(a); al != nil && al.Active {
			t.block(keyAlarm|uint64(a), 0)
			return
		}
		ret(cpu, 0)
	})

	initQueue := func(m *Machine, q, name uint32) {
		m.Mem.Fill(q, 0, osAlarmQueueSize)
		m.Mem.Write32(q, osAlarmQueueTag)
		m.Mem.Write32(q+4, name)
		m.Mem.Write32(q+osAlarmQueueSelf, q)
	}
	reg(c, "OSInitAlarmQueue", func(m *Machine, cpu *CPU, t *Thread) { initQueue(m, arg(cpu, 0), 0) })
	reg(c, "OSInitAlarmQueueEx", func(m *Machine, cpu *CPU, t *Thread) { initQueue(m, arg(cpu, 0), arg(cpu, 1)) })

	// OSGetAlarmFromQueue(queue, alarm out, context out, don't wait)
	reg(c, "OSGetAlarmFromQueue", func(m *Machine, cpu *CPU, t *Thread) {
		mem := m.Mem
		q, alarmOut, ctxOut, noWait := arg(cpu, 0), arg(cpu, 1), arg(cpu, 2), arg(cpu, 3)
		a := mem.Read32(q + osAlarmQueueHead)
		if a == 0 && noWait == 0 {
			t.block(keyAlarmQ|uint64(q), 0)
			return
		}
		var ctx uint32
		if a != 0 {
			m.dequeueAlarm(a)
			if period := mem.Read64(a + osAlarmPeriod); period != 0 {
				m.armAlarm(cpu.Core, a, mem.Read64(a+osAlarmStart), period, true, mem.Read32(a+osAlarmHandler), q)
			}
			ctx = mem.Read32(a + osAlarmContext)
		}
		mem.Write32(alarmOut, a)
		mem.Write32(ctxOut, ctx)
		retBool(cpu, a != 0)
	})
}
