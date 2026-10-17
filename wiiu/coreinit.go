package wiiu

// coreinit: the core of Cafe OS that every program links with. This file has
// startup and exit, the console, time, system information, atomics, the
// dynamic loader and memory functions; coreinit_thread.go has threads and
// synchronisation, and heaps.go the heaps behind the MEM* functions.

import (
	"math/bits"
	"time"
)

// the OSSystemInfo the program reads clock speeds from
var systemInfoAddr uint32

func (m *Machine) initSystemData() {
	systemInfoAddr = m.sysAlloc(0x20, 32)
	mem := m.Mem
	mem.Write32(systemInfoAddr+0, BusHz)
	mem.Write32(systemInfoAddr+4, CoreHz)
	mem.Write64(systemInfoAddr+8, 0) // base time
	m.bootTime = osTicksSince2000(time.Now())
}

// osTicksSince2000: Cafe OS time counts timer ticks from 2000-01-01.
func osTicksSince2000(t time.Time) uint64 {
	d := t.Sub(time.Date(2000, 1, 1, 0, 0, 0, 0, time.Local))
	return uint64(d.Seconds() * TimerHz)
}

func init() {
	c := "coreinit"

	// startup: the RPLs' entry points, then the program's
	reg(c, "__RPStartup", func(m *Machine, cpu *CPU, t *Thread) {
		if mod := m.nextInit(); mod != nil {
			cpu.GPR[3] = m.moduleHandle(mod.Name)
			cpu.GPR[4] = 1 // loading
			cpu.LR = m.thunk("coreinit", "__RPStartup")
			cpu.PC = mod.Entry
			m.jumped = true
			return
		}
		main := m.mainModule()
		cpu.GPR[1] = (cpu.GPR[1]) &^ 7
		cpu.GPR[2] = main.Info.SDA2Base
		cpu.GPR[13] = main.Info.SDABase
		cpu.GPR[3] = 1
		cpu.GPR[4] = m.argv
		cpu.LR = m.thunk("coreinit", "exit")
		cpu.PC = main.Entry
		m.jumped = true
	})
	reg(c, "__RPThreadReturn", func(m *Machine, cpu *CPU, t *Thread) {
		m.exitThread(t, cpu.GPR[3])
		t.yield()
	})
	exit := func(m *Machine, cpu *CPU, t *Thread) {
		m.exitProgram(arg(cpu, 0))
		if t != nil {
			t.State = thDone
			t.yield()
		}
	}
	reg(c, "exit", exit)
	reg(c, "_Exit", exit)
	reg(c, "OSFatal", func(m *Machine, cpu *CPU, t *Thread) {
		msg := m.Mem.String(arg(cpu, 0), 1024)
		m.logf("OSFatal: %s", msg)
		m.fatal("the program stopped with an error: " + msg)
	})
	reg(c, "OSPanic", func(m *Machine, cpu *CPU, t *Thread) {
		file := m.Mem.String(arg(cpu, 0), 256)
		line := arg(cpu, 1)
		msg := m.sprintf(m.Mem.String(arg(cpu, 2), 1024), newVarargs(m, cpu, 3))
		m.logf("OSPanic %s:%d: %s", file, line, msg)
		m.fatal("the program stopped with an error: " + msg)
	})

	// the console
	reg(c, "OSReport", func(m *Machine, cpu *CPU, t *Thread) {
		m.report(m.sprintf(m.Mem.String(arg(cpu, 0), 4096), newVarargs(m, cpu, 1)))
	})
	reg(c, "OSReportInfo", func(m *Machine, cpu *CPU, t *Thread) {
		m.report(m.sprintf(m.Mem.String(arg(cpu, 0), 4096), newVarargs(m, cpu, 1)))
	})
	reg(c, "OSReportWarn", func(m *Machine, cpu *CPU, t *Thread) {
		m.report(m.sprintf(m.Mem.String(arg(cpu, 0), 4096), newVarargs(m, cpu, 1)))
	})
	reg(c, "OSReportVerbose", func(m *Machine, cpu *CPU, t *Thread) {})
	reg(c, "OSVReport", func(m *Machine, cpu *CPU, t *Thread) {
		m.report(m.sprintf(m.Mem.String(arg(cpu, 0), 4096), newVaList(m, cpu, arg(cpu, 1))))
	})
	for _, n := range []string{"COSError", "COSWarn", "COSInfo", "COSVerbose"} {
		reg(c, n, func(m *Machine, cpu *CPU, t *Thread) {
			m.report(m.sprintf(m.Mem.String(arg(cpu, 1), 4096), newVarargs(m, cpu, 2)))
		})
	}
	reg(c, "OSConsoleWrite", func(m *Machine, cpu *CPU, t *Thread) {
		m.report(string(m.Mem.Bytes(arg(cpu, 0), int(min(arg(cpu, 1), 65536)))))
	})
	reg(c, "__OSConsoleWrite", func(m *Machine, cpu *CPU, t *Thread) {
		m.report(string(m.Mem.Bytes(arg(cpu, 0), int(min(arg(cpu, 1), 65536)))))
	})
	snprintf := func(m *Machine, cpu *CPU, buf, size uint32, s string) {
		if size > 0 {
			b := []byte(s)
			if uint32(len(b)) > size-1 {
				b = b[:size-1]
			}
			m.Mem.WriteBytes(buf, append(b, 0))
		}
		ret(cpu, uint32(len(s)))
	}
	reg(c, "__os_snprintf", func(m *Machine, cpu *CPU, t *Thread) {
		s := m.sprintf(m.Mem.String(arg(cpu, 2), 4096), newVarargs(m, cpu, 3))
		snprintf(m, cpu, arg(cpu, 0), arg(cpu, 1), s)
	})
	reg(c, "__os_vsnprintf", func(m *Machine, cpu *CPU, t *Thread) {
		s := m.sprintf(m.Mem.String(arg(cpu, 2), 4096), newVaList(m, cpu, arg(cpu, 3)))
		snprintf(m, cpu, arg(cpu, 0), arg(cpu, 1), s)
	})

	// time
	reg(c, "OSGetTime", func(m *Machine, cpu *CPU, t *Thread) { ret64(cpu, m.bootTime+m.Time()) })
	reg(c, "OSGetSystemTime", func(m *Machine, cpu *CPU, t *Thread) { ret64(cpu, m.Time()) })
	reg(c, "OSGetTick", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, uint32(m.bootTime+m.Time())) })
	reg(c, "OSGetSystemTick", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, uint32(m.Time())) })
	reg(c, "OSGetSystemInfo", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, systemInfoAddr) })
	reg(c, "OSTicksToCalendarTime", func(m *Machine, cpu *CPU, t *Thread) {
		ticks := int64(arg64(cpu, 0))
		out := arg(cpu, 2)
		base := time.Date(2000, 1, 1, 0, 0, 0, 0, time.UTC)
		ns := ticks / (TimerHz / 1000000) * 1000
		tm := base.Add(time.Duration(ns))
		mem := m.Mem
		mem.Write32(out+0x00, uint32(tm.Second()))
		mem.Write32(out+0x04, uint32(tm.Minute()))
		mem.Write32(out+0x08, uint32(tm.Hour()))
		mem.Write32(out+0x0c, uint32(tm.Day()))
		mem.Write32(out+0x10, uint32(tm.Month()-1))
		mem.Write32(out+0x14, uint32(tm.Year()))
		mem.Write32(out+0x18, uint32(tm.Weekday()))
		mem.Write32(out+0x1c, uint32(tm.YearDay()-1))
		mem.Write32(out+0x20, uint32(tm.Nanosecond()/1000000))
		mem.Write32(out+0x24, uint32(tm.Nanosecond()/1000%1000))
	})
	reg(c, "OSCalendarTimeToTicks", func(m *Machine, cpu *CPU, t *Thread) {
		in := arg(cpu, 0)
		mem := m.Mem
		g := func(o uint32) int { return int(int32(mem.Read32(in + o))) }
		tm := time.Date(g(0x14), time.Month(g(0x10)+1), g(0x0c), g(0x08), g(0x04), g(0x00),
			g(0x20)*1000000+g(0x24)*1000, time.UTC)
		d := tm.Sub(time.Date(2000, 1, 1, 0, 0, 0, 0, time.UTC))
		ret64(cpu, uint64(int64(d/time.Microsecond)*(TimerHz/1000000)))
	})

	// cores and the system
	reg(c, "OSGetCoreId", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, uint32(cpu.Core)) })
	reg(c, "OSGetCoreCount", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 3) })
	reg(c, "OSIsMainCore", func(m *Machine, cpu *CPU, t *Thread) { retBool(cpu, cpu.Core == 1) })
	reg(c, "OSGetMainCoreId", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 1) })
	reg(c, "OSGetTitleID", func(m *Machine, cpu *CPU, t *Thread) { ret64(cpu, m.Files.TitleID) })
	reg(c, "OSGetOSID", func(m *Machine, cpu *CPU, t *Thread) { ret64(cpu, 0x000500101000400a) })
	reg(c, "OSGetUPID", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 15) })
	reg(c, "OSGetPFID", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 15) })
	reg(c, "OSIsDebuggerPresent", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0) })
	reg(c, "OSIsDebuggerInitialized", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0) })
	reg(c, "OSGetConsoleType", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0x13000048) })
	reg(c, "OSGetSecurityLevel", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0) })
	reg(c, "OSIsECOMode", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0) })
	reg(c, "OSIsColdBoot", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 1) })
	reg(c, "OSGetBootPMFlags", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0) })
	reg(c, "OSGetAppFlags", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0) })
	reg(c, "OSGetSystemMode", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0) })
	reg(c, "OSIsHomeButtonMenuEnabled", func(m *Machine, cpu *CPU, t *Thread) { retBool(cpu, m.homeMenu) })
	reg(c, "OSEnableHomeButtonMenu", func(m *Machine, cpu *CPU, t *Thread) {
		m.homeMenu = arg(cpu, 0) != 0
		ret(cpu, 1)
	})
	reg(c, "OSBlockThreadsOnExit", func(m *Machine, cpu *CPU, t *Thread) {})
	reg(c, "OSGetArgcArgv", func(m *Machine, cpu *CPU, t *Thread) {
		m.Mem.Write32(arg(cpu, 0), 1)
		m.Mem.Write32(arg(cpu, 1), m.argv)
		ret(cpu, 1)
	})
	reg(c, "OSGetMemBound", func(m *Machine, cpu *CPU, t *Thread) {
		var a, n uint32
		switch arg(cpu, 0) {
		case 1:
			a, n = MEM1Base, MEM1Size
		case 2:
			hp := m.heaps.get(m.heaps.Base[arenaMEM2])
			a, n = hp.Start, hp.Hi-hp.Start
		default:
			ret(cpu, 0xffffffff)
			return
		}
		m.Mem.Write32(arg(cpu, 1), a)
		m.Mem.Write32(arg(cpu, 2), n)
		ret(cpu, 0)
	})
	fg := func(m *Machine, cpu *CPU, t *Thread) {
		if a := arg(cpu, 0); a != 0 {
			m.Mem.Write32(a, FGBase)
		}
		if a := arg(cpu, 1); a != 0 {
			m.Mem.Write32(a, FGSize)
		}
		ret(cpu, 1)
	}
	reg(c, "OSGetForegroundBucket", fg)
	reg(c, "OSGetForegroundBucketFreeArea", fg)
	reg(c, "OSEffectiveToPhysical", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, toPhysical(arg(cpu, 0))) })
	reg(c, "OSPhysicalToEffectiveCached", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, toEffective(arg(cpu, 0))) })
	reg(c, "OSPhysicalToEffectiveUncached", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, toEffective(arg(cpu, 0))) })
	reg(c, "OSIsAddressValid", func(m *Machine, cpu *CPU, t *Thread) { retBool(cpu, m.Mem.Mapped(arg(cpu, 0))) })
	reg(c, "__OSValidateAddressSpaceRange", func(m *Machine, cpu *CPU, t *Thread) {
		retBool(cpu, m.Mem.Mapped(arg(cpu, 1)) && m.Mem.Mapped(arg(cpu, 1)+arg(cpu, 2)-1))
	})

	// interrupts (nothing interrupts a library call here)
	reg(c, "OSDisableInterrupts", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, b2u(!m.intsOff[cpu.Core])); m.intsOff[cpu.Core] = true })
	reg(c, "OSEnableInterrupts", func(m *Machine, cpu *CPU, t *Thread) {
		ret(cpu, b2u(!m.intsOff[cpu.Core]))
		m.intsOff[cpu.Core] = false
	})
	reg(c, "OSRestoreInterrupts", func(m *Machine, cpu *CPU, t *Thread) {
		ret(cpu, b2u(!m.intsOff[cpu.Core]))
		m.intsOff[cpu.Core] = arg(cpu, 0) == 0
	})
	reg(c, "OSIsInterruptEnabled", func(m *Machine, cpu *CPU, t *Thread) { retBool(cpu, !m.intsOff[cpu.Core]) })

	// cache and barriers (memory is always coherent here)
	for _, n := range []string{"DCFlushRange", "DCFlushRangeNoSync", "DCStoreRange", "DCStoreRangeNoSync",
		"DCInvalidateRange", "DCTouchRange", "OSMemoryBarrier", "OSCoherencyBarrier",
		"OSEnforceInorderIO", "DCFlushAll", "DCInvalidateAll"} {
		reg(c, n, func(m *Machine, cpu *CPU, t *Thread) {})
	}
	reg(c, "ICInvalidateRange", func(m *Machine, cpu *CPU, t *Thread) { m.Mem.InvalidateCode(arg(cpu, 0), arg(cpu, 1)) })
	reg(c, "DCZeroRange", func(m *Machine, cpu *CPU, t *Thread) {
		a, n := arg(cpu, 0), arg(cpu, 1)
		start := a &^ 31
		m.Mem.Fill(start, 0, (a+n+31)&^31-start)
	})

	// memory functions
	reg(c, "memcpy", func(m *Machine, cpu *CPU, t *Thread) {
		m.Mem.Move(arg(cpu, 0), arg(cpu, 1), arg(cpu, 2))
	})
	reg(c, "memmove", func(m *Machine, cpu *CPU, t *Thread) {
		m.Mem.Move(arg(cpu, 0), arg(cpu, 1), arg(cpu, 2))
	})
	reg(c, "memset", func(m *Machine, cpu *CPU, t *Thread) {
		m.Mem.Fill(arg(cpu, 0), byte(arg(cpu, 1)), arg(cpu, 2))
	})
	reg(c, "memclr", func(m *Machine, cpu *CPU, t *Thread) {
		m.Mem.Fill(arg(cpu, 0), 0, arg(cpu, 1))
	})
	reg(c, "OSBlockMove", func(m *Machine, cpu *CPU, t *Thread) {
		m.Mem.Move(arg(cpu, 0), arg(cpu, 1), arg(cpu, 2))
	})
	reg(c, "OSBlockSet", func(m *Machine, cpu *CPU, t *Thread) {
		m.Mem.Fill(arg(cpu, 0), byte(arg(cpu, 1)), arg(cpu, 2))
	})

	// atomics (one host thread runs every core, so these are simple)
	reg(c, "OSCompareAndSwapAtomic", func(m *Machine, cpu *CPU, t *Thread) {
		p := arg(cpu, 0)
		if m.Mem.Read32(p) == arg(cpu, 1) {
			m.Mem.Write32(p, arg(cpu, 2))
			ret(cpu, 1)
		} else {
			ret(cpu, 0)
		}
	})
	reg(c, "OSCompareAndSwapAtomicEx", func(m *Machine, cpu *CPU, t *Thread) {
		p := arg(cpu, 0)
		old := m.Mem.Read32(p)
		m.Mem.Write32(arg(cpu, 3), old)
		if old == arg(cpu, 1) {
			m.Mem.Write32(p, arg(cpu, 2))
			ret(cpu, 1)
		} else {
			ret(cpu, 0)
		}
	})
	atomic := func(name string, f func(old, v uint32) uint32) {
		reg(c, name, func(m *Machine, cpu *CPU, t *Thread) {
			p := arg(cpu, 0)
			old := m.Mem.Read32(p)
			m.Mem.Write32(p, f(old, arg(cpu, 1)))
			ret(cpu, old)
		})
	}
	atomic("OSSwapAtomic", func(o, v uint32) uint32 { return v })
	atomic("OSAddAtomic", func(o, v uint32) uint32 { return o + v })
	atomic("OSAndAtomic", func(o, v uint32) uint32 { return o & v })
	atomic("OSOrAtomic", func(o, v uint32) uint32 { return o | v })
	atomic("OSXorAtomic", func(o, v uint32) uint32 { return o ^ v })
	reg(c, "OSTestAndSetAtomic", func(m *Machine, cpu *CPU, t *Thread) {
		p, b := arg(cpu, 0), uint32(1)<<(arg(cpu, 1)&31)
		old := m.Mem.Read32(p)
		m.Mem.Write32(p, old|b)
		retBool(cpu, old&b != 0)
	})
	reg(c, "OSTestAndClearAtomic", func(m *Machine, cpu *CPU, t *Thread) {
		p, b := arg(cpu, 0), uint32(1)<<(arg(cpu, 1)&31)
		old := m.Mem.Read32(p)
		m.Mem.Write32(p, old&^b)
		retBool(cpu, old&b != 0)
	})
	reg(c, "OSGetAtomic64", func(m *Machine, cpu *CPU, t *Thread) { ret64(cpu, m.Mem.Read64(arg(cpu, 0))) })
	reg(c, "OSSetAtomic64", func(m *Machine, cpu *CPU, t *Thread) {
		p := arg(cpu, 0)
		old := m.Mem.Read64(p)
		m.Mem.Write64(p, arg64(cpu, 2))
		ret64(cpu, old)
	})
	reg(c, "OSCompareAndSwapAtomic64", func(m *Machine, cpu *CPU, t *Thread) {
		p := arg(cpu, 0)
		if m.Mem.Read64(p) == arg64(cpu, 2) {
			m.Mem.Write64(p, arg64(cpu, 4))
			ret(cpu, 1)
		} else {
			ret(cpu, 0)
		}
	})
	reg(c, "OSCompareAndSwapAtomicEx64", func(m *Machine, cpu *CPU, t *Thread) {
		p := arg(cpu, 0)
		old := m.Mem.Read64(p)
		m.Mem.Write64(m.Mem.Read32(cpu.GPR[1]+8), old)
		if old == arg64(cpu, 2) {
			m.Mem.Write64(p, arg64(cpu, 4))
			ret(cpu, 1)
		} else {
			ret(cpu, 0)
		}
	})
	atomic64 := func(name string, f func(old, v uint64) uint64) {
		reg(c, name, func(m *Machine, cpu *CPU, t *Thread) {
			p := arg(cpu, 0)
			old := m.Mem.Read64(p)
			m.Mem.Write64(p, f(old, arg64(cpu, 2)))
			ret64(cpu, old)
		})
	}
	atomic64("OSSwapAtomic64", func(o, v uint64) uint64 { return v })
	atomic64("OSAddAtomic64", func(o, v uint64) uint64 { return o + v })
	atomic64("OSAndAtomic64", func(o, v uint64) uint64 { return o & v })
	atomic64("OSOrAtomic64", func(o, v uint64) uint64 { return o | v })
	atomic64("OSXorAtomic64", func(o, v uint64) uint64 { return o ^ v })
	reg(c, "OSTestAndSetAtomic64", func(m *Machine, cpu *CPU, t *Thread) {
		p, b := arg(cpu, 0), uint64(1)<<(arg(cpu, 1)&63)
		old := m.Mem.Read64(p)
		m.Mem.Write64(p, old|b)
		retBool(cpu, old&b != 0)
	})
	reg(c, "OSTestAndClearAtomic64", func(m *Machine, cpu *CPU, t *Thread) {
		p, b := arg(cpu, 0), uint64(1)<<(arg(cpu, 1)&63)
		old := m.Mem.Read64(p)
		m.Mem.Write64(p, old&^b)
		retBool(cpu, old&b != 0)
	})

	// the dynamic loader
	reg(c, "OSDynLoad_Acquire", func(m *Machine, cpu *CPU, t *Thread) {
		name := m.Mem.String(arg(cpu, 0), 256)
		h, err := m.acquireModule(name)
		if out := arg(cpu, 1); out != 0 {
			m.Mem.Write32(out, h)
		}
		if err != 0 {
			ret(cpu, err)
			return
		}
		ret(cpu, 0)
		// libraries loaded now run their entry points before the call returns
		if len(m.initQueue) > 0 && t != nil {
			if m.dynReturn == nil {
				m.dynReturn = map[*Thread]uint32{}
			}
			m.dynReturn[t] = cpu.LR
			cpu.PC = m.thunk("coreinit", "__RPDynLoadInit")
			m.jumped = true
		}
	})
	reg(c, "__RPDynLoadInit", func(m *Machine, cpu *CPU, t *Thread) {
		if mod := m.nextInit(); mod != nil {
			cpu.GPR[3] = m.moduleHandle(mod.Name)
			cpu.GPR[4] = 1 // loading
			cpu.LR = m.thunk("coreinit", "__RPDynLoadInit")
			cpu.PC = mod.Entry
			m.jumped = true
			return
		}
		ret(cpu, 0)
		cpu.PC = m.dynReturn[t]
		delete(m.dynReturn, t)
		m.jumped = true
	})
	reg(c, "OSDynLoad_AcquireContainingModule", func(m *Machine, cpu *CPU, t *Thread) {
		addr := arg(cpu, 0)
		h := uint32(0)
		for _, mod := range m.Modules {
			if (addr >= mod.TextLo && addr < mod.TextHi) || (addr >= mod.DataLo && addr < mod.DataHi) {
				h = m.moduleHandle(mod.Name)
			}
		}
		if h == 0 {
			h = m.moduleHandle("coreinit")
		}
		m.Mem.Write32(arg(cpu, 2), h)
		ret(cpu, 0)
	})
	reg(c, "OSDynLoad_IsModuleLoaded", func(m *Machine, cpu *CPU, t *Thread) {
		name := libName(m.Mem.String(arg(cpu, 0), 256))
		if m.moduleNamed(name) != nil || hleFuncs[name] != nil {
			if out := arg(cpu, 1); out != 0 {
				m.Mem.Write32(out, m.moduleHandle(name))
			}
		} else if out := arg(cpu, 1); out != 0 {
			m.Mem.Write32(out, 0)
		}
		ret(cpu, 0)
	})
	reg(c, "OSDynLoad_Release", func(m *Machine, cpu *CPU, t *Thread) {})
	reg(c, "OSDynLoad_FindExport", func(m *Machine, cpu *CPU, t *Thread) {
		lib, ok := m.dynHandles[arg(cpu, 0)]
		name := m.Mem.String(arg(cpu, 2), 256)
		out := arg(cpu, 3)
		if !ok {
			ret(cpu, 0xbad10001)
			return
		}
		isData := arg(cpu, 1) != 0
		if mod := m.moduleNamed(lib); mod != nil {
			a, ok := mod.Exports[name]
			if !ok {
				ret(cpu, 0xbad10001)
				return
			}
			m.Mem.Write32(out, a)
			ret(cpu, 0)
			return
		}
		if isData {
			if hleData[lib][name].size == 0 && hleFuncs[lib][name] == nil {
				ret(cpu, 0xbad10001)
				return
			}
			m.Mem.Write32(out, m.dataExport(lib, name))
		} else {
			if hleFuncs[lib][name] == nil {
				m.logf("the program looked for %s.%s, which RetroPixels doesn't have yet", lib, name)
			}
			m.Mem.Write32(out, m.thunk(lib, name))
		}
		ret(cpu, 0)
	})
	reg(c, "OSDynLoad_SetAllocator", func(m *Machine, cpu *CPU, t *Thread) {
		m.dynAlloc, m.dynFree = arg(cpu, 0), arg(cpu, 1)
		ret(cpu, 0)
	})
	reg(c, "OSDynLoad_GetAllocator", func(m *Machine, cpu *CPU, t *Thread) {
		if a := arg(cpu, 0); a != 0 {
			m.Mem.Write32(a, m.dynAlloc)
		}
		if a := arg(cpu, 1); a != 0 {
			m.Mem.Write32(a, m.dynFree)
		}
		ret(cpu, 0)
	})
	reg(c, "OSDynLoad_GetModuleName", func(m *Machine, cpu *CPU, t *Thread) {
		lib := m.dynHandles[arg(cpu, 0)]
		buf, sizep := arg(cpu, 1), arg(cpu, 2)
		size := m.Mem.Read32(sizep)
		if uint32(len(lib)+1) > size {
			m.Mem.Write32(sizep, uint32(len(lib)+1))
			ret(cpu, 0xbad10016)
			return
		}
		m.Mem.WriteString(buf, lib)
		ret(cpu, 0)
	})
	reg(c, "OSDynLoad_AddNotifyCallback", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0) })
	reg(c, "OSDynLoad_DelNotifyCallback", func(m *Machine, cpu *CPU, t *Thread) {})

	// errno of the Green Hills C library the system libraries share
	reg(c, "__gh_errno_ptr", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, m.errnoAddr(t)) })
	reg(c, "__gh_set_errno", func(m *Machine, cpu *CPU, t *Thread) { m.Mem.Write32(m.errnoAddr(t), arg(cpu, 0)) })
	reg(c, "__gh_get_errno", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, m.Mem.Read32(m.errnoAddr(t))) })
	for _, n := range []string{"__ghsLock", "__ghsUnlock", "__ghs_flock_file", "__ghs_funlock_file"} {
		reg(c, n, func(m *Machine, cpu *CPU, t *Thread) {})
	}
	reg(c, "__ghs_flock_create", func(m *Machine, cpu *CPU, t *Thread) { m.Mem.Write32(arg(cpu, 0), 1) })
	reg(c, "__ghs_flock_destroy", func(m *Machine, cpu *CPU, t *Thread) {})
	reg(c, "__get_eh_globals", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, m.threadBlock(t, "eh", 0x10)) })
	reg(c, "__get_eh_mem_manage", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, m.threadBlock(t, "ehm", 0x10)) })
	reg(c, "__get_eh_store_globals", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, m.threadBlock(t, "ehs", 0x10)) })
	reg(c, "__get_eh_store_globals_tdeh", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, m.threadBlock(t, "ehst", 0x10)) })
	reg(c, "__tls_get_addr", func(m *Machine, cpu *CPU, t *Thread) {
		// tls_index { module, offset }
		p := arg(cpu, 0)
		mi, off := m.Mem.Read32(p), m.Mem.Read32(p+4)
		ret(cpu, m.tlsBlock(t, mi)+off)
	})

	// the scheduler's run quantum and other thread-wide settings
	reg(c, "OSSetThreadRunQuantum", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 1) })

	// IM (idle / auto power-down)
	for _, n := range []string{"IMEnableAPD", "IMDisableAPD", "IMEnableDim", "IMDisableDim", "IMSetDimEnableDrc",
		"IMSetDimEnableTv", "IMEnableScreenSaver", "IMDisableScreenSaver", "IMStartAPDVideoMode"} {
		reg(c, n, func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0) })
	}
	for _, n := range []string{"IMIsAPDEnabled", "IMIsAPDEnabledBySysSettings", "IMIsDimEnabled"} {
		reg(c, n, func(m *Machine, cpu *CPU, t *Thread) {
			if a := arg(cpu, 0); a != 0 {
				m.Mem.Write32(a, 0)
			}
			ret(cpu, 0)
		})
	}
	reg(c, "IMGetTimeBeforeAPD", func(m *Machine, cpu *CPU, t *Thread) {
		m.Mem.Write32(arg(cpu, 0), 3600)
		ret(cpu, 0)
	})
	reg(c, "IMGetTimeBeforeDimming", func(m *Machine, cpu *CPU, t *Thread) {
		m.Mem.Write32(arg(cpu, 0), 300)
		ret(cpu, 0)
	})

	// MCP and the user's settings
	reg(c, "MCP_Open", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 1) })
	reg(c, "MCP_Close", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0) })
	reg(c, "MCP_GetSysProdSettings", func(m *Machine, cpu *CPU, t *Thread) {
		// MCPSysProdSettings: product area 2 = USA, game region, ...
		p := arg(cpu, 1)
		m.Mem.Fill(p, 0, 0x46)
		m.Mem.Write32(p+0, 2) // product area: America
		m.Mem.Write32(p+4, 2) // game region
		ret(cpu, 0)
	})
	reg(c, "UCOpen", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 1) })
	reg(c, "UCClose", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0) })
	reg(c, "UCReadSysConfig", func(m *Machine, cpu *CPU, t *Thread) { m.ucRead(cpu); ret(cpu, 0) })
	reg(c, "UCWriteSysConfig", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0) })
}

// physical addresses, as the GPU sees memory
func toPhysical(a uint32) uint32 {
	switch {
	case a >= MEM1Base && a < MEM1Base+MEM1Size:
		return a - MEM1Base
	case a >= DataBase && a < MEM2End:
		return a - DataBase + 0x50000000
	case a >= FGBase && a < FGBase+FGSize:
		return a - FGBase + 0x14000000
	}
	return a
}

func toEffective(p uint32) uint32 {
	switch {
	case p < MEM1Size:
		return p + MEM1Base
	case p >= 0x50000000 && p < 0x90000000:
		return p - 0x50000000 + DataBase
	case p >= 0x14000000 && p < 0x14000000+FGSize:
		return p - 0x14000000 + FGBase
	}
	return p
}

func (m *Machine) fatal(msg string) {
	if m.Err == nil {
		m.Err = &ProgramError{msg}
	}
	for _, c := range m.CPU {
		c.Stop()
	}
}

// ProgramError is the program stopping itself (OSFatal, OSPanic).
type ProgramError struct{ Msg string }

func (e *ProgramError) Error() string { return e.Msg }

// report is the program writing to its console.
func (m *Machine) report(s string) {
	for len(s) > 0 && (s[len(s)-1] == '\n' || s[len(s)-1] == '\r') {
		s = s[:len(s)-1]
	}
	m.logf("%s", s)
}

// moduleHandle is the OSDynLoad handle of a library.
func (m *Machine) moduleHandle(name string) uint32 {
	name = libName(name)
	for h, n := range m.dynHandles {
		if n == name {
			return h
		}
	}
	h := m.sysAlloc(0x40, 16)
	m.Mem.WriteString(h, name)
	if m.dynHandles == nil {
		m.dynHandles = map[uint32]string{}
	}
	m.dynHandles[h] = name
	return h
}

func (m *Machine) acquireModule(name string) (uint32, uint32) {
	lib := libName(name)
	if i := lastSlash(lib); i >= 0 {
		lib = lib[i+1:]
	}
	if m.moduleNamed(lib) != nil || hleFuncs[lib] != nil || knownLibs[lib] {
		return m.moduleHandle(lib), 0
	}
	// one of the title's own RPLs, loaded now
	if mod, err := m.titleLibrary(lib); err != nil {
		m.logf("%v", err)
		return 0, 0xbad10001
	} else if mod != nil {
		return m.moduleHandle(lib), 0
	}
	m.logf("the program asked for the library %s, which RetroPixels doesn't have", name)
	return 0, 0xbad10001
}

func lastSlash(s string) int {
	for i := len(s) - 1; i >= 0; i-- {
		if s[i] == '/' || s[i] == '\\' {
			return i
		}
	}
	return -1
}

// the system libraries a program can load (even ones RetroPixels has
// nothing of yet)
var knownLibs = map[string]bool{}

func init() {
	for _, n := range []string{"avm", "camera", "coreinit", "dc", "dmae", "drmapp", "erreula", "gx2", "h264",
		"lzma920", "mic", "nfc", "nio_prof", "nlibcurl", "nlibnss", "nlibnss2", "nn_ac", "nn_acp", "nn_act",
		"nn_aoc", "nn_boss", "nn_ccr", "nn_cmpt", "nn_dlp", "nn_ec", "nn_fp", "nn_hai", "nn_hpad", "nn_idbe",
		"nn_ndm", "nn_nets2", "nn_nfp", "nn_nim", "nn_olv", "nn_pdm", "nn_save", "nn_sl", "nn_spm", "nn_temp",
		"nn_uds", "nn_vctl", "nsysccr", "nsyshid", "nsyskbd", "nsysnet", "nsysuhs", "nsysuvd", "ntag", "padscore",
		"proc_ui", "sndcore2", "snd_core", "snd_user", "sndcore2", "snduser2", "swkbd", "sysapp", "tcl", "tve",
		"uac", "uac_rpl", "usb_mic", "uvc", "uvd", "vpad", "vpadbase", "zlib125"} {
		knownLibs[n] = true
	}
}

// errnoAddr: each thread's errno
func (m *Machine) errnoAddr(t *Thread) uint32 { return m.threadBlock(t, "errno", 4) }

// threadBlock is a small per-thread block of memory, made on first use.
func (m *Machine) threadBlock(t *Thread, what string, size uint32) uint32 {
	key := what
	if t != nil {
		key = what + ":" + itoa(t.ID)
	}
	if a, ok := m.perThread[key]; ok {
		return a
	}
	a := m.heaps.allocSystem(m, size, 16)
	m.Mem.Fill(a, 0, size)
	if m.perThread == nil {
		m.perThread = map[string]uint32{}
	}
	m.perThread[key] = a
	return a
}

// tlsBlock is thread t's copy of a module's thread-local data.
func (m *Machine) tlsBlock(t *Thread, index uint32) uint32 {
	if t.TLS == nil {
		t.TLS = map[uint32]uint32{}
	}
	if a, ok := t.TLS[index]; ok {
		return a
	}
	var mod *Module
	if index >= 1 && int(index) <= len(m.Modules) {
		mod = m.Modules[index-1]
	}
	size := uint32(0x100)
	if mod != nil && mod.TLSSize > 0 {
		size = mod.TLSSize
	}
	a := m.heaps.allocSystem(m, size, 64)
	m.Mem.Fill(a, 0, size)
	if mod != nil && mod.TLSSize > 0 {
		m.Mem.Move(a, mod.TLSAddr, mod.TLSSize)
	}
	t.TLS[index] = a
	return a
}

func itoa(v uint32) string {
	if v == 0 {
		return "0"
	}
	var b [10]byte
	i := len(b)
	for v > 0 {
		i--
		b[i] = byte('0' + v%10)
		v /= 10
	}
	return string(b[i:])
}

// UC (system settings) reads: fill each requested setting with a default
func (m *Machine) ucRead(cpu *CPU) {
	n, list := arg(cpu, 1), arg(cpu, 2)
	mem := m.Mem
	// UCSysConfig: name[64], access u32, type u32, error i32, dataSize u32, data ptr
	for i := uint32(0); i < n; i++ {
		e := list + i*0x54
		name := mem.String(e, 64)
		size := mem.Read32(e + 0x4c)
		data := mem.Read32(e + 0x50)
		mem.Write32(e+0x48, 0)
		if data == 0 || size == 0 {
			continue
		}
		mem.Fill(data, 0, size)
		switch name {
		case "cafe.language":
			mem.Write32(data, 1) // English
		case "cafe.cntry_reg":
			mem.Write32(data, 49) // United States
		case "cafe.version":
			mem.Write16(data, 5)
		case "cafe.initial_launch":
			mem.Write8(data, 2)
		case "cafe.eco":
			mem.Write8(data, 0)
		case "parent.enable":
			mem.Write8(data, 0)
		}
	}
	_ = bits.Len32
}
