package ps3

// LV2 system calls (sc with the call's number in r11).

import (
	"fmt"
	"time"
)

type syscallFn func(m *Machine, t *Thread)

var syscalls = map[uint32]syscallFn{}

func sysc(n uint32, fn syscallFn) { syscalls[n] = fn }

func (m *Machine) syscall(c *PPU) {
	t := m.cur
	n := uint32(t.GPR[11])
	fn := syscalls[n]
	if fn == nil {
		name := fmt.Sprintf("syscall %d", n)
		m.Missing[name]++
		if m.Missing[name] <= 3 {
			m.Logf("missing %s(%x, %x, %x, %x) at %08X", name, t.GPR[3], t.GPR[4], t.GPR[5], t.GPR[6], c.PC-4)
		}
		t.GPR[3] = eNOSYS
		return
	}
	if traceSyscalls {
		m.Logf("syscall %d(%x, %x, %x, %x) at %08X", n, t.GPR[3], t.GPR[4], t.GPR[5], t.GPR[6], c.PC-4)
	}
	fn(m, t)
	if t.state != thReady {
		c.Stop()
	}
}

var traceSyscalls = false

// startTime is the clock the game sees at the start (real time then
// virtual time after).
var startTime = time.Now()

func (m *Machine) now() time.Time {
	return startTime.Add(time.Duration(m.tb * 1000000000 / TBFreq))
}

func init() {
	sysc(1, func(m *Machine, t *Thread) { t.ret(1) }) // sys_process_getpid
	sysc(22, func(m *Machine, t *Thread) { // sys_process_exit
		m.Logf("the game exited (%d)", t.argS(0))
		m.Exited = true
		m.cpu.Stop()
	})
	sysc(25, func(m *Machine, t *Thread) { // sys_process_get_sdk_version
		m.Mem.Write32(t.arg(1), m.proc.sdk)
		t.ret(0)
	})
	sysc(30, func(m *Machine, t *Thread) { t.ret(eNOSYS) }) // _sys_process_get_paramsfo

	// --- PPU threads
	sysc(41, func(m *Machine, t *Thread) { m.threadExit(t, t.GPR[3]); m.cpu.Stop() })
	sysc(43, func(m *Machine, t *Thread) { t.ret(0); m.cpu.Stop() }) // yield
	sysc(44, func(m *Machine, t *Thread) { // join
		id, vp := t.arg(0), t.arg(1)
		o, ok := m.objects[id].(*Thread)
		if !ok {
			t.ret(eSRCH)
			return
		}
		if o == t {
			t.ret(eDEADLK)
			return
		}
		m.block(t, "join "+o.Name, func() bool {
			if o.state != thExited {
				return false
			}
			if vp != 0 {
				m.Mem.Write64(vp, o.exitValue)
			}
			o.joined = true
			t.ret(0)
			return true
		})
	})
	sysc(45, func(m *Machine, t *Thread) { // detach
		if o, ok := m.objects[t.arg(0)].(*Thread); ok {
			o.joinable = false
			t.ret(0)
			return
		}
		t.ret(eSRCH)
	})
	sysc(46, func(m *Machine, t *Thread) { // get_join_state
		m.Mem.Write32(t.arg(0), boolU32(t.joinable))
		t.ret(0)
	})
	sysc(47, func(m *Machine, t *Thread) { // set_priority
		if o, ok := m.objects[t.arg(0)].(*Thread); ok {
			o.Prio = t.argS(1)
			t.ret(0)
			return
		}
		t.ret(eSRCH)
	})
	sysc(48, func(m *Machine, t *Thread) { // get_priority
		if o, ok := m.objects[t.arg(0)].(*Thread); ok {
			m.Mem.Write32(t.arg(1), uint32(o.Prio))
			t.ret(0)
			return
		}
		t.ret(eSRCH)
	})
	sysc(49, func(m *Machine, t *Thread) { // get_stack_information
		m.Mem.Write32(t.arg(0), t.Stack)
		m.Mem.Write32(t.arg(0)+4, t.StackSize)
		t.ret(0)
	})
	sysc(52, func(m *Machine, t *Thread) { // _sys_ppu_thread_create
		idp, param, arg := t.arg(0), t.arg(1), t.arg64(2)
		prio, stack, name := t.argS(4), t.arg(5), m.Mem.String(t.arg(7), 64)
		entry := m.Mem.Read32(param)
		nt := m.newThread(name, entry, arg, prio, stack)
		if nt == nil {
			t.ret(eNOMEM)
			return
		}
		if tls := m.Mem.Read32(param + 4); tls != 0 {
			nt.GPR[13] = uint64(tls)
		}
		m.Mem.Write64(idp, uint64(nt.ID))
		t.ret(0)
	})
	sysc(53, func(m *Machine, t *Thread) { // start
		if o, ok := m.objects[t.arg(0)].(*Thread); ok && o.state == thStopped {
			o.state = thReady
			t.ret(0)
			return
		}
		t.ret(eSRCH)
	})
	sysc(56, func(m *Machine, t *Thread) { // rename
		if o, ok := m.objects[t.arg(0)].(*Thread); ok {
			o.Name = m.Mem.String(t.arg(1), 64)
		}
		t.ret(0)
	})

	// --- time
	sysc(141, func(m *Machine, t *Thread) { // sys_timer_usleep
		us := t.arg64(0)
		t.ret(0)
		m.blockTimeout(t, "usleep", max(usToTB(us), 1), func() bool { return false }, func() {})
	})
	sysc(142, func(m *Machine, t *Thread) { // sys_timer_sleep
		s := t.arg64(0)
		t.ret(0)
		m.blockTimeout(t, "sleep", max(usToTB(s*1000000), 1), func() bool { return false }, func() {})
	})
	sysc(144, func(m *Machine, t *Thread) { // sys_time_get_timezone
		m.Mem.Write32(t.arg(0), 0)
		m.Mem.Write32(t.arg(1), 0)
		t.ret(0)
	})
	sysc(145, func(m *Machine, t *Thread) { // sys_time_get_current_time
		n := m.now()
		m.Mem.Write64(t.arg(0), uint64(n.Unix()))
		m.Mem.Write64(t.arg(1), uint64(n.Nanosecond()))
		t.ret(0)
	})
	sysc(147, func(m *Machine, t *Thread) { t.GPR[3] = TBFreq }) // sys_time_get_timebase_frequency

	// --- memory
	sysc(348, func(m *Machine, t *Thread) { // sys_memory_allocate(size, flags, addr*)
		size, flags, ap := t.arg(0), t.arg(1), t.arg(2)
		align := uint32(0x100000)
		if flags&0xf00 == 0x200 {
			align = 0x10000
		}
		if size == 0 || size%align != 0 {
			t.ret(eALIGN)
			return
		}
		a := m.user.alloc(size, align)
		if a == 0 {
			t.ret(eNOMEM)
			return
		}
		m.Mem.Fill(a, 0, size)
		m.Mem.Write32(ap, a)
		m.Logf("sys_memory_allocate(%x) = %08X", size, a)
		t.ret(0)
	})
	sysc(349, func(m *Machine, t *Thread) { // sys_memory_free
		if _, ok := m.user.free(t.arg(0)); !ok {
			t.ret(eINVAL)
			return
		}
		t.ret(0)
	})
	sysc(351, func(m *Machine, t *Thread) { // sys_memory_get_page_attribute
		m.Mem.Write64(t.arg(1), 0x40000)
		m.Mem.Write32(t.arg(1)+8, 0x10000)
		m.Mem.Write32(t.arg(1)+12, 1)
		t.ret(0)
	})
	sysc(352, func(m *Machine, t *Thread) { // sys_memory_get_user_memory_size
		total := uint32(0x0D500000)
		m.Mem.Write32(t.arg(0), total)
		m.Mem.Write32(t.arg(0)+4, total-m.user.inUse())
		t.ret(0)
	})

	// --- TTY
	sysc(402, func(m *Machine, t *Thread) { m.Mem.Write32(t.arg(3), 0); t.ret(0) })
	sysc(403, func(m *Machine, t *Thread) { // sys_tty_write(ch, buf, len, written*)
		n := t.arg(2)
		buf := make([]byte, min(n, 4096))
		m.Mem.ReadBytes(t.arg(1), buf)
		m.ttyWrite(string(buf))
		if t.arg(3) != 0 {
			m.Mem.Write32(t.arg(3), n)
		}
		t.ret(0)
	})
	sysc(988, func(m *Machine, t *Thread) { t.ret(0) }) // sys_dbg_*
}

func (m *Machine) ttyWrite(s string) {
	m.tty += s
	for {
		i := indexByte(m.tty, '\n')
		if i < 0 {
			break
		}
		m.Logf("TTY: %s", m.tty[:i])
		m.tty = m.tty[i+1:]
	}
	if len(m.tty) > 4096 {
		m.Logf("TTY: %s", m.tty)
		m.tty = ""
	}
}

func indexByte(s string, c byte) int {
	for i := 0; i < len(s); i++ {
		if s[i] == c {
			return i
		}
	}
	return -1
}

func boolU32(b bool) uint32 {
	if b {
		return 1
	}
	return 0
}
