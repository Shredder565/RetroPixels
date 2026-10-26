// Original code for RetroPixels. Not derived from Cemu.

package wiiu

// coreinit alarms. When an alarm's time comes its handler(alarm, context)
// runs on the callback thread of the core that set it; a periodic alarm
// comes back every period. An alarm set with an alarm queue (the Ex calls)
// is instead appended to that queue for a thread to collect with
// OSGetAlarmFromQueue, and a periodic one is armed again only once it has
// been collected.
//
// The emulator keeps one Alarm record per OSAlarm it has armed; the times in
// those records are in time base ticks since boot, while the OSAlarm in
// memory holds OSTime values (ticks since 2000).

import "math"

// OSAlarm layout (0x58 bytes)
const (
	oaTag      = 0x00
	oaName     = 0x04
	oaHandler  = 0x0c
	oaGroup    = 0x10
	oaNextFire = 0x18 // u64 OSTime
	oaQPrev    = 0x20 // links within its alarm queue
	oaQNext    = 0x24
	oaPeriod   = 0x28 // u64
	oaStart    = 0x30 // u64
	oaUserData = 0x38
	oaState    = 0x3c // RetroPixels' use: see alarmArmed etc.
	oaQueue    = 0x50
	oaContext  = 0x54
	oaBytes    = 0x58

	oaMagic = 0x614c724d // "aLrM"

	alarmArmed     = 1
	alarmFired     = 2
	alarmCancelled = 3
)

// OSAlarmQueue layout (0x24 bytes)
const (
	aqTag   = 0x00
	aqName  = 0x04
	aqSelf  = 0x14
	aqFirst = 0x1c
	aqLast  = 0x20
	aqBytes = 0x24

	aqMagic = 0x614c6d51 // "aLmQ"
)

// Alarm is the emulator's record of an armed OSAlarm.
type Alarm struct {
	addr   uint32
	due    uint64 // time base ticks since boot
	period uint64 // 0 for a one-shot alarm
	core   int
	armed  bool
}

// alarmRecord finds the record for the OSAlarm at addr.
func (m *Machine) alarmRecord(addr uint32) *Alarm {
	for _, a := range m.alarms {
		if a.addr == addr {
			return a
		}
	}
	return nil
}

// alarmWaitKey is what threads in OSWaitAlarm wait on.
func alarmWaitKey(addr uint32) uint64 { return keyAlarm | uint64(addr) }

// osAlarmInit is OSCreateAlarm(Ex). Re-creating an armed alarm disarms it.
func (m *Machine) osAlarmInit(addr, name uint32) {
	if a := m.alarmRecord(addr); a != nil {
		a.armed = false
	}
	m.Mem.Fill(addr, 0, oaBytes)
	m.Mem.Write32(addr+oaTag, oaMagic)
	m.Mem.Write32(addr+oaName, name)
}

// ---- alarm queues ----

// queuedAlarm reports whether the alarm has fired into its queue and has
// not been collected yet.
func (m *Machine) queuedAlarm(addr uint32) bool {
	mem := m.Mem
	q := mem.Read32(addr + oaQueue)
	if q == 0 {
		return false
	}
	return mem.Read32(q+aqFirst) == addr || mem.Read32(addr+oaQPrev) != 0
}

func (m *Machine) alarmQueuePush(addr uint32) {
	mem := m.Mem
	q := mem.Read32(addr + oaQueue)
	last := mem.Read32(q + aqLast)
	mem.Write32(addr+oaQPrev, last)
	mem.Write32(addr+oaQNext, 0)
	if last == 0 {
		mem.Write32(q+aqFirst, addr)
	} else {
		mem.Write32(last+oaQNext, addr)
	}
	mem.Write32(q+aqLast, addr)
	m.wake(keyAlarmQ | uint64(q))
}

func (m *Machine) alarmQueueDrop(addr uint32) {
	mem := m.Mem
	q := mem.Read32(addr + oaQueue)
	prev, next := mem.Read32(addr+oaQPrev), mem.Read32(addr+oaQNext)
	if prev == 0 {
		mem.Write32(q+aqFirst, next)
	} else {
		mem.Write32(prev+oaQNext, next)
	}
	if next == 0 {
		mem.Write32(q+aqLast, prev)
	} else {
		mem.Write32(next+oaQPrev, prev)
	}
	mem.Write32(addr+oaQPrev, 0)
	mem.Write32(addr+oaQNext, 0)
}

// ---- arming, firing, cancelling ----

// armAlarm arms the OSAlarm at addr. first is an OSTime. A periodic alarm
// whose first time has already passed starts at the next multiple of the
// period after it. An alarm still sitting uncollected in its queue can't be
// re-armed.
func (m *Machine) armAlarm(core int, addr uint32, first, period uint64, periodic bool, handler, queue uint32) {
	if m.queuedAlarm(addr) {
		return
	}
	mem := m.Mem
	when := first
	if !periodic {
		period = 0
	} else {
		if int64(period) <= 0 {
			return
		}
		if now := m.bootTime + m.Time(); first < now {
			missed := (now-first)/period + 1
			when = first + missed*period
		}
		mem.Write64(addr+oaStart, first)
	}
	mem.Write64(addr+oaNextFire, when)
	mem.Write64(addr+oaPeriod, period)
	mem.Write32(addr+oaHandler, handler)
	mem.Write32(addr+oaQueue, queue)
	mem.Write32(addr+oaState, alarmArmed)

	a := m.alarmRecord(addr)
	if a == nil {
		a = &Alarm{addr: addr}
		m.alarms = append(m.alarms, a)
	}
	a.due = 0
	if when > m.bootTime {
		a.due = when - m.bootTime
	}
	a.period, a.core, a.armed = period, core, true
	m.alarmThread(core)
}

// disarmAlarm is OSCancelAlarm: it reports whether there was anything to
// cancel (an armed alarm, or one waiting in its queue).
func (m *Machine) disarmAlarm(addr uint32) bool {
	if a := m.alarmRecord(addr); a != nil && a.armed {
		a.armed = false
		m.Mem.Write32(addr+oaState, alarmCancelled)
		m.wake(alarmWaitKey(addr))
		return true
	}
	if m.queuedAlarm(addr) {
		m.alarmQueueDrop(addr)
		return true
	}
	return false
}

// fire handles one alarm whose time has come. It reports whether the alarm
// stays armed (and may be due again already).
func (m *Machine) fire(a *Alarm, now uint64) bool {
	mem := m.Mem
	addr := a.addr
	if mem.Read32(addr+oaQueue) != 0 {
		a.armed = false
		mem.Write32(addr+oaContext, m.alarmContext)
		m.alarmQueuePush(addr)
		return false
	}
	m.queueCallback(a.core, mem.Read32(addr+oaHandler), addr, m.alarmContext)
	if a.period == 0 {
		a.armed = false
		mem.Write32(addr+oaState, alarmFired)
		m.wake(alarmWaitKey(addr))
		return false
	}
	a.due += a.period
	// hopelessly behind (the program was stalled): skip ahead rather than
	// firing once for every missed period
	if a.due+100*a.period < now {
		a.due = now + a.period
	}
	mem.Write64(addr+oaNextFire, m.bootTime+a.due)
	return true
}

// runAlarms fires every alarm that is due by the current time base.
func (m *Machine) runAlarms() {
	now := m.tb
	for _, a := range m.alarms {
		for a.armed && a.due <= now {
			if !m.fire(a, now) {
				break
			}
		}
	}
}

// nextAlarm is when the next armed alarm is due (at least 1), or 0 if none
// is armed.
func (m *Machine) nextAlarm() uint64 {
	soonest := uint64(math.MaxUint64)
	for _, a := range m.alarms {
		if a.armed {
			soonest = min(soonest, a.due)
		}
	}
	if soonest == math.MaxUint64 {
		return 0
	}
	return max(soonest, 1)
}

func init() {
	const c = "coreinit"

	reg(c, "OSCreateAlarm", func(m *Machine, cpu *CPU, t *Thread) { m.osAlarmInit(arg(cpu, 0), 0) })
	reg(c, "OSCreateAlarmEx", func(m *Machine, cpu *CPU, t *Thread) { m.osAlarmInit(arg(cpu, 0), arg(cpu, 1)) })

	// OSSetAlarm(alarm, u64 delay, handler) and OSSetAlarmEx(..., queue)
	oneShot := func(withQueue bool) hleFunc {
		return func(m *Machine, cpu *CPU, t *Thread) {
			var queue uint32
			if withQueue {
				queue = arg(cpu, 5)
			}
			if delay := arg64(cpu, 2); int64(delay) > 0 {
				when := m.bootTime + m.Time() + delay
				m.armAlarm(cpu.Core, arg(cpu, 0), when, 0, false, arg(cpu, 4), queue)
			}
			ret(cpu, 1)
		}
	}
	reg(c, "OSSetAlarm", oneShot(false))
	reg(c, "OSSetAlarmEx", oneShot(true))

	// OSSetPeriodicAlarm(alarm, u64 start, u64 period, handler) and
	// OSSetPeriodicAlarmEx(..., queue)
	periodic := func(withQueue bool) hleFunc {
		return func(m *Machine, cpu *CPU, t *Thread) {
			var queue uint32
			if withQueue {
				queue = arg(cpu, 7)
			}
			m.armAlarm(cpu.Core, arg(cpu, 0), arg64(cpu, 2), arg64(cpu, 4), true, arg(cpu, 6), queue)
			ret(cpu, 1)
		}
	}
	reg(c, "OSSetPeriodicAlarm", periodic(false))
	reg(c, "OSSetPeriodicAlarmEx", periodic(true))

	reg(c, "OSCancelAlarm", func(m *Machine, cpu *CPU, t *Thread) { retBool(cpu, m.disarmAlarm(arg(cpu, 0))) })
	// OSCancelAlarms(group): every armed alarm tagged with group
	reg(c, "OSCancelAlarms", func(m *Machine, cpu *CPU, t *Thread) {
		group := arg(cpu, 0)
		if group == 0 || group == 0xffffffff {
			return
		}
		for _, a := range m.alarms {
			if a.armed && m.Mem.Read32(a.addr+oaGroup) == group {
				m.disarmAlarm(a.addr)
			}
		}
	})
	reg(c, "OSSetAlarmUserData", func(m *Machine, cpu *CPU, t *Thread) {
		m.Mem.Write32(arg(cpu, 0)+oaUserData, arg(cpu, 1))
	})
	reg(c, "OSGetAlarmUserData", func(m *Machine, cpu *CPU, t *Thread) {
		ret(cpu, m.Mem.Read32(arg(cpu, 0)+oaUserData))
	})
	reg(c, "OSSetAlarmTag", func(m *Machine, cpu *CPU, t *Thread) {
		if group := arg(cpu, 1); group != 0xffffffff {
			m.Mem.Write32(arg(cpu, 0)+oaGroup, group)
		}
	})
	// OSWaitAlarm(alarm): blocks until the alarm fires (1) or is cancelled
	// (0); 0 at once when it isn't armed
	reg(c, "OSWaitAlarm", func(m *Machine, cpu *CPU, t *Thread) {
		addr := arg(cpu, 0)
		if t.Woken {
			retBool(cpu, m.Mem.Read32(addr+oaState) != alarmCancelled)
			return
		}
		if a := m.alarmRecord(addr); a == nil || !a.armed {
			ret(cpu, 0)
			return
		}
		t.block(alarmWaitKey(addr), 0)
	})

	initQueue := func(m *Machine, q, name uint32) {
		m.Mem.Fill(q, 0, aqBytes)
		m.Mem.Write32(q+aqTag, aqMagic)
		m.Mem.Write32(q+aqName, name)
		m.Mem.Write32(q+aqSelf, q)
	}
	reg(c, "OSInitAlarmQueue", func(m *Machine, cpu *CPU, t *Thread) { initQueue(m, arg(cpu, 0), 0) })
	reg(c, "OSInitAlarmQueueEx", func(m *Machine, cpu *CPU, t *Thread) { initQueue(m, arg(cpu, 0), arg(cpu, 1)) })
	// OSGetAlarmFromQueue(queue, OSAlarm **alarm, void **context, noWait):
	// takes the oldest fired alarm, blocking for one unless noWait. A
	// periodic alarm is armed again as it is taken.
	reg(c, "OSGetAlarmFromQueue", func(m *Machine, cpu *CPU, t *Thread) {
		q, alarmOut, ctxOut, noWait := arg(cpu, 0), arg(cpu, 1), arg(cpu, 2), arg(cpu, 3) != 0
		mem := m.Mem
		addr := mem.Read32(q + aqFirst)
		if addr == 0 {
			if !noWait {
				t.block(keyAlarmQ|uint64(q), 0)
				return
			}
			mem.Write32(alarmOut, 0)
			mem.Write32(ctxOut, 0)
			ret(cpu, 0)
			return
		}
		m.alarmQueueDrop(addr)
		if period := mem.Read64(addr + oaPeriod); period != 0 {
			m.armAlarm(cpu.Core, addr, mem.Read64(addr+oaStart), period, true, mem.Read32(addr+oaHandler), q)
		}
		mem.Write32(alarmOut, addr)
		mem.Write32(ctxOut, mem.Read32(addr+oaContext))
		ret(cpu, 1)
	})
}
