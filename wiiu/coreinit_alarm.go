// This Source Code Form is subject to the terms of the Mozilla Public License,
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at https://mozilla.org/MPL/2.0/.
//
// Based on Cemu (https://github.com/cemu-project/Cemu), src/Cafe/OS/libs/
// coreinit/coreinit_Alarm.cpp and coreinit_Alarm.h, rewritten in Go for
// RetroPixels.

package wiiu

// coreinit: alarms. An alarm calls its handler (alarm, context) on the
// callback thread of the core that set it when its time comes, again each
// period for a periodic one. One set with an alarm queue (OSSetAlarmEx) goes
// into the queue instead, for a thread to take with OSGetAlarmFromQueue.

import "math"

// OSAlarm fields
const (
	alarmTag      = 0x614c724d // "aLrM"
	alarmHandler  = 0x0c
	alarmGroup    = 0x10
	alarmNext     = 0x18 // the time it fires next
	alarmPrevLink = 0x20 // in its alarm queue
	alarmNextLink = 0x24
	alarmPeriod   = 0x28
	alarmStart    = 0x30
	alarmUserData = 0x38
	alarmState    = 0x3c // RetroPixels' own: 1 set, 2 fired, 3 cancelled
	alarmQueue    = 0x50
	alarmContext  = 0x54

	// OSAlarmQueue fields
	alarmQueueTag  = 0x614c6d51 // "aLmQ"
	alarmQueueHead = 0x1c
	alarmQueueTail = 0x20
)

// Alarm is an OSAlarm that's been set.
type Alarm struct {
	Addr   uint32
	Fire   uint64 // in time base ticks
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
	if al := m.alarm(a); al != nil {
		al.Active = false
	}
	m.Mem.Fill(a, 0, 0x58)
	m.Mem.Write32(a, alarmTag)
	m.Mem.Write32(a+4, name)
}

// inAlarmQueue: a fired alarm waiting in its alarm queue.
func (m *Machine) inAlarmQueue(a uint32) bool {
	q := m.Mem.Read32(a + alarmQueue)
	return q != 0 && (m.Mem.Read32(q+alarmQueueHead) == a || m.Mem.Read32(a+alarmPrevLink) != 0)
}

func (m *Machine) appendAlarmToQueue(a uint32) {
	mem := m.Mem
	q := mem.Read32(a + alarmQueue)
	tail := mem.Read32(q + alarmQueueTail)
	mem.Write32(a+alarmPrevLink, tail)
	mem.Write32(a+alarmNextLink, 0)
	if tail != 0 {
		mem.Write32(tail+alarmNextLink, a)
	} else {
		mem.Write32(q+alarmQueueHead, a)
	}
	mem.Write32(q+alarmQueueTail, a)
	m.wake(keyAlarmQ | uint64(q))
}

func (m *Machine) removeAlarmFromQueue(a uint32) {
	mem := m.Mem
	q := mem.Read32(a + alarmQueue)
	prev, next := mem.Read32(a+alarmPrevLink), mem.Read32(a+alarmNextLink)
	if next != 0 {
		mem.Write32(next+alarmPrevLink, prev)
	} else {
		mem.Write32(q+alarmQueueTail, prev)
	}
	if prev != 0 {
		mem.Write32(prev+alarmNextLink, next)
	} else {
		mem.Write32(q+alarmQueueHead, next)
	}
	mem.Write32(a+alarmPrevLink, 0)
	mem.Write32(a+alarmNextLink, 0)
}

// setAlarm sets the alarm at a to fire at start (an OSTime), and then every
// period if it's periodic: the first time is the next whole period after
// start when that's gone by already.
func (m *Machine) setAlarm(core int, a uint32, start, period uint64, periodic bool, handler, queue uint32) {
	if m.inAlarmQueue(a) {
		return // fired, and still waiting to be taken from its queue
	}
	mem := m.Mem
	next := start
	if periodic {
		if int64(period) <= 0 {
			return
		}
		if now := m.bootTime + m.Time(); start < now {
			next = start + ((now-start)/period+1)*period
		}
		mem.Write64(a+alarmStart, start)
	} else {
		period = 0
	}
	mem.Write64(a+alarmNext, next)
	mem.Write64(a+alarmPeriod, period)
	mem.Write32(a+alarmHandler, handler)
	mem.Write32(a+alarmQueue, queue)
	mem.Write32(a+alarmState, 1)
	al := m.alarm(a)
	if al == nil {
		al = &Alarm{Addr: a}
		m.alarms = append(m.alarms, al)
	}
	al.Fire = next - m.bootTime
	if next < m.bootTime {
		al.Fire = 0
	}
	al.Period, al.Core, al.Active = period, core, true
	m.alarmThread(core)
}

func (m *Machine) cancelAlarm(a uint32) bool {
	if al := m.alarm(a); al != nil && al.Active {
		al.Active = false
		m.Mem.Write32(a+alarmState, 3)
		m.wake(keyAlarm | uint64(a))
		return true
	}
	if m.inAlarmQueue(a) {
		m.removeAlarmFromQueue(a)
		return true
	}
	return false
}

// runAlarms fires the alarms that are due (a periodic one once for each
// period gone by).
func (m *Machine) runAlarms() {
	now := m.tb
	for _, al := range m.alarms {
		for al.Active && al.Fire <= now {
			a := al.Addr
			if m.Mem.Read32(a+alarmQueue) != 0 {
				// it waits in its queue; a periodic one is set again once
				// it's taken out
				al.Active = false
				m.Mem.Write32(a+alarmContext, m.alarmContext)
				m.appendAlarmToQueue(a)
				break
			}
			m.queueCallback(al.Core, m.Mem.Read32(a+alarmHandler), a, m.alarmContext)
			if al.Period == 0 {
				al.Active = false
				m.Mem.Write32(a+alarmState, 2)
				m.wake(keyAlarm | uint64(a))
				break
			}
			al.Fire += al.Period
			if al.Fire+100*al.Period < now {
				al.Fire = now + al.Period // far behind: don't call it for every period missed
			}
			m.Mem.Write64(a+alarmNext, al.Fire+m.bootTime)
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
	return max(next, 1)
}

func init() {
	c := "coreinit"

	reg(c, "OSCreateAlarm", func(m *Machine, cpu *CPU, t *Thread) { m.createAlarm(arg(cpu, 0), 0) })
	reg(c, "OSCreateAlarmEx", func(m *Machine, cpu *CPU, t *Thread) { m.createAlarm(arg(cpu, 0), arg(cpu, 1)) })
	// OSSetAlarm(alarm, delay, handler), OSSetAlarmEx(..., queue)
	set := func(m *Machine, cpu *CPU, queue uint32) {
		if delay := arg64(cpu, 2); int64(delay) > 0 {
			m.setAlarm(cpu.Core, arg(cpu, 0), m.bootTime+m.Time()+delay, 0, false, arg(cpu, 4), queue)
		}
		ret(cpu, 1)
	}
	reg(c, "OSSetAlarm", func(m *Machine, cpu *CPU, t *Thread) { set(m, cpu, 0) })
	reg(c, "OSSetAlarmEx", func(m *Machine, cpu *CPU, t *Thread) { set(m, cpu, arg(cpu, 5)) })
	// OSSetPeriodicAlarm(alarm, start, period, handler), OSSetPeriodicAlarmEx(..., queue)
	setPeriodic := func(m *Machine, cpu *CPU, queue uint32) {
		m.setAlarm(cpu.Core, arg(cpu, 0), arg64(cpu, 2), arg64(cpu, 4), true, arg(cpu, 6), queue)
		ret(cpu, 1)
	}
	reg(c, "OSSetPeriodicAlarm", func(m *Machine, cpu *CPU, t *Thread) { setPeriodic(m, cpu, 0) })
	reg(c, "OSSetPeriodicAlarmEx", func(m *Machine, cpu *CPU, t *Thread) { setPeriodic(m, cpu, arg(cpu, 7)) })
	reg(c, "OSCancelAlarm", func(m *Machine, cpu *CPU, t *Thread) { retBool(cpu, m.cancelAlarm(arg(cpu, 0))) })
	reg(c, "OSCancelAlarms", func(m *Machine, cpu *CPU, t *Thread) {
		group := arg(cpu, 0)
		if group == 0 || group == 0xffffffff {
			return
		}
		for _, al := range m.alarms {
			if al.Active && m.Mem.Read32(al.Addr+alarmGroup) == group {
				m.cancelAlarm(al.Addr)
			}
		}
	})
	reg(c, "OSSetAlarmUserData", func(m *Machine, cpu *CPU, t *Thread) { m.Mem.Write32(arg(cpu, 0)+alarmUserData, arg(cpu, 1)) })
	reg(c, "OSGetAlarmUserData", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, m.Mem.Read32(arg(cpu, 0)+alarmUserData)) })
	reg(c, "OSSetAlarmTag", func(m *Machine, cpu *CPU, t *Thread) {
		if group := arg(cpu, 1); group != 0xffffffff {
			m.Mem.Write32(arg(cpu, 0)+alarmGroup, group)
		}
	})
	reg(c, "OSWaitAlarm", func(m *Machine, cpu *CPU, t *Thread) {
		a := arg(cpu, 0)
		if t.Woken {
			ret(cpu, b2u(m.Mem.Read32(a+alarmState) != 3))
			return
		}
		al := m.alarm(a)
		if al == nil || !al.Active {
			ret(cpu, 0)
			return
		}
		t.block(keyAlarm|uint64(a), 0)
	})

	// alarm queues
	initQueue := func(m *Machine, q, name uint32) {
		m.Mem.Fill(q, 0, 0x24)
		m.Mem.Write32(q, alarmQueueTag)
		m.Mem.Write32(q+4, name)
		m.Mem.Write32(q+0x14, q)
	}
	reg(c, "OSInitAlarmQueue", func(m *Machine, cpu *CPU, t *Thread) { initQueue(m, arg(cpu, 0), 0) })
	reg(c, "OSInitAlarmQueueEx", func(m *Machine, cpu *CPU, t *Thread) { initQueue(m, arg(cpu, 0), arg(cpu, 1)) })
	// OSGetAlarmFromQueue(queue, alarmOut, contextOut, noWait)
	reg(c, "OSGetAlarmFromQueue", func(m *Machine, cpu *CPU, t *Thread) {
		q, alarmOut, ctxOut, noWait := arg(cpu, 0), arg(cpu, 1), arg(cpu, 2), arg(cpu, 3)
		mem := m.Mem
		a := mem.Read32(q + alarmQueueHead)
		if a == 0 {
			if noWait == 0 {
				t.block(keyAlarmQ|uint64(q), 0)
				return
			}
			mem.Write32(alarmOut, 0)
			mem.Write32(ctxOut, 0)
			ret(cpu, 0)
			return
		}
		m.removeAlarmFromQueue(a)
		if period := mem.Read64(a + alarmPeriod); period != 0 {
			m.setAlarm(cpu.Core, a, mem.Read64(a+alarmStart), period, true, mem.Read32(a+alarmHandler), q)
		}
		mem.Write32(alarmOut, a)
		mem.Write32(ctxOut, mem.Read32(a+alarmContext))
		ret(cpu, 1)
	})
}
