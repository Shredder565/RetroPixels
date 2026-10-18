package wiiu

// coreinit: the callback threads (which run alarms, deallocators and other
// calls the system makes into the program), giving up the core, and the
// system message queues. Threads are in coreinit_threads.go, synchronisation
// in coreinit_sync.go and alarms in coreinit_alarm.go.

func init() {
	c := "coreinit"

	sysQueue := func(name string) hleFunc {
		return func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, m.systemQueue(name)) }
	}
	reg(c, "OSGetSystemMessageQueue", sysQueue("system"))
	reg(c, "OSGetDefaultAppIOQueue", sysQueue("appio"))

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
// this thread runs again. A thread with interrupts off keeps the core.
func (m *Machine) preempt(cpu *CPU) {
	t := m.current[cpu.Core]
	if t != nil && t.IntsOff && t.runnable(cpu.Core) {
		return
	}
	cpu.Stop()
	if t != nil {
		m.runSeq++
		t.LastRun = m.runSeq
	}
	m.current[cpu.Core] = nil
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

// alarmThread makes the hidden thread that runs a core's callbacks.
func (m *Machine) alarmThread(core int) {
	if m.alarmTh[core] != nil {
		return
	}
	addr := m.sysAlloc(osThreadSize, 64)
	stack := m.allocSystem(0x8000, 64) + 0x8000
	t := m.initThread(addr, m.thunk("coreinit", "__RPAlarmLoop"), 0, 0, stack, 0x8000, 0, 1<<core, threadDriver)
	t.Ctx.PC = m.thunk("coreinit", "__RPAlarmLoop")
	t.BasePrio, t.Priority = -1, -1 // above every program thread, like an interrupt
	t.Saved = t.Ctx.GPR[1]
	t.State, t.Suspend = thReady, 0
	m.syncThread(t)
	m.Mem.Write32(addr+osThreadName, m.cstrAlloc("{RetroPixels alarms}"))
	m.alarmTh[core] = t
	if m.alarmContext == 0 {
		m.alarmContext = m.sysAlloc(0x320, 32)
		m.Mem.Write32(m.alarmContext, 0x4f53436f)   // "OSCo"
		m.Mem.Write32(m.alarmContext+4, 0x6e747874) // "ntxt"
	}
}
