package ps3

// sysPrxForUser (threads, TLS, PRX modules, process), cellSysmodule,
// cellGame, cellRtc and the PSN libraries (which say "not signed in").

import (
	"time"
)

func init() {
	// --- threads
	reg("sys_ppu_thread_create", func(m *Machine, t *Thread) { // (id*, entry, arg, prio, stacksize, flags, name*)
		idp, entry, arg, prio, stack, flags := t.arg(0), t.arg(1), t.arg64(2), t.argS(3), t.arg(4), t.arg64(5)
		name := m.Mem.String(t.arg(6), 64)
		nt := m.newThread(name, entry, arg, prio, stack)
		if nt == nil {
			t.ret(eNOMEM)
			return
		}
		nt.joinable = flags&1 != 0
		nt.state = thReady
		m.Mem.Write64(idp, uint64(nt.ID))
		m.Logf("new thread %q (%x) entry %08X arg %x prio %d stack %x", name, nt.ID, m.Mem.Read32(entry), arg, prio, stack)
		t.ret(0)
	})
	reg("sys_ppu_thread_get_id", func(m *Machine, t *Thread) {
		m.Mem.Write64(t.arg(0), uint64(t.ID))
		t.ret(0)
	})
	reg("sys_ppu_thread_exit", func(m *Machine, t *Thread) { m.threadExit(t, t.GPR[3]) })
	reg("sys_initialize_tls", func(m *Machine, t *Thread) { // (main thread id, tls addr, file size, mem size)
		m.tlsImage, m.tlsFile, m.tlsMem = t.arg(1), t.arg(2), t.arg(3)
		t.GPR[13] = uint64(m.allocTLS())
		t.tls = uint32(t.GPR[13])
		t.ret(0)
	})
	reg("sys_time_get_system_time", func(m *Machine, t *Thread) {
		t.GPR[3] = m.tb * 1000000 / TBFreq
	})
	reg("sys_process_exit", func(m *Machine, t *Thread) {
		m.Logf("the game exited (%d)", t.argS(0))
		m.Exited = true
		m.cpu.Stop()
	})
	reg("_sys_process_atexitspawn", func(m *Machine, t *Thread) {})
	reg("_sys_process_at_Exitspawn", func(m *Machine, t *Thread) {})
	reg("sys_prx_exitspawn_with_level", func(m *Machine, t *Thread) {
		m.Logf("the game exited (exitspawn)")
		m.Exited = true
		m.cpu.Stop()
	})

	// --- PRX modules
	reg("sys_prx_load_module", func(m *Machine, t *Thread) { // (path, flags, opt)
		path := m.Mem.String(t.arg(0), 1024)
		hp := m.hostPath(path)
		if hp == "" {
			t.ret(eNOENT)
			return
		}
		b, err := readFileCase(hp)
		if err != nil {
			m.Logf("sys_prx_load_module %s: %v", path, err)
			t.ret(eNOENT)
			return
		}
		elf, err := m.decryptWithGameKey(b)
		if err != nil {
			m.Logf("sys_prx_load_module %s: %v", path, err)
			t.ret(eNOEXEC)
			return
		}
		mod, err := m.loadPRX(elf, path)
		if err != nil {
			m.Logf("sys_prx_load_module %s: %v", path, err)
			t.ret(eNOEXEC)
			return
		}
		t.ret(mod.id)
	})
	reg("sys_prx_start_module", func(m *Machine, t *Thread) { // (id, args, argp, modres*, flags, opt)
		mod, ok := m.objects[t.arg(0)].(*prxModule)
		if !ok {
			t.ret(eSRCH)
			return
		}
		res := t.arg(3)
		if mod.start == 0 {
			if res != 0 {
				m.Mem.Write32(res, 0)
			}
			t.ret(0)
			return
		}
		mod.started = true
		t.ret(0)
		m.callGuest(t, mod.start, []uint64{t.arg64(1), t.arg64(2)}, func(r uint64) {
			m.Logf("module %s started (%x)", mod.name, r)
			if res != 0 {
				m.Mem.Write32(res, uint32(r))
			}
			t.ret(0)
		})
	})
	reg("sys_prx_stop_module", func(m *Machine, t *Thread) {
		mod, ok := m.objects[t.arg(0)].(*prxModule)
		if !ok {
			t.ret(eSRCH)
			return
		}
		res := t.arg(3)
		t.ret(0)
		if mod.stop == 0 {
			return
		}
		m.callGuest(t, mod.stop, []uint64{t.arg64(1), t.arg64(2)}, func(r uint64) {
			if res != 0 {
				m.Mem.Write32(res, uint32(r))
			}
			t.ret(0)
		})
	})
	reg("sys_prx_unload_module", func(m *Machine, t *Thread) { t.ret(0) })
	reg("sys_prx_register_library", func(m *Machine, t *Thread) { t.ret(0) })

	// --- cellSysmodule
	reg("cellSysmoduleLoadModule", func(m *Machine, t *Thread) {
		m.Logf("cellSysmoduleLoadModule(%#x)", t.arg(0))
		t.ret(0)
	})
	reg("cellSysmoduleUnloadModule", func(m *Machine, t *Thread) { t.ret(0) })
	reg("cellSysmoduleIsLoaded", func(m *Machine, t *Thread) { t.ret(0) })

	// --- cellGame
	reg("cellGameBootCheck", func(m *Machine, t *Thread) { // (type*, attributes*, size*, dirName*)
		m.Mem.Write32(t.arg(0), 2) // HDD game
		m.Mem.Write32(t.arg(1), 0)
		if s := t.arg(2); s != 0 {
			m.Mem.Write32(s, 40*1024*1024) // free KB
			m.Mem.Write32(s+4, 0xffffffff)
			m.Mem.Write32(s+8, 0)
		}
		if d := t.arg(3); d != 0 {
			m.Mem.WriteString(d, m.titleID())
		}
		t.ret(0)
	})
	reg("cellGameContentPermit", func(m *Machine, t *Thread) { // (contentInfoPath*, usrdirPath*)
		base := "/dev_hdd0/game/" + m.titleID()
		m.Mem.WriteString(t.arg(0), base)
		m.Mem.WriteString(t.arg(1), base+"/USRDIR")
		t.ret(0)
	})
	reg("cellGameGetParamInt", func(m *Machine, t *Thread) { // (id, value*)
		var v uint32
		switch t.arg(0) {
		case 102:
			v, _ = m.sfo()["PARENTAL_LEVEL"].(uint32)
		case 103:
			v, _ = m.sfo()["RESOLUTION"].(uint32)
		case 104:
			v, _ = m.sfo()["SOUND_FORMAT"].(uint32)
		}
		m.Mem.Write32(t.arg(1), v)
		t.ret(0)
	})
	reg("cellGameGetParamString", func(m *Machine, t *Thread) { // (id, buf, size)
		var s string
		switch id := t.arg(0); {
		case id < 100:
			s = m.sfoString("TITLE")
		case id == 100:
			s = m.titleID()
		case id == 101:
			s = m.sfoString("VERSION")
		case id == 106:
			s = m.sfoString("APP_VER")
		}
		size := t.arg(2)
		if uint32(len(s)) >= size {
			s = s[:size-1]
		}
		m.Mem.WriteString(t.arg(1), s)
		t.ret(0)
	})

	// --- cellRtc
	reg("cellRtcGetCurrentClockLocalTime", func(m *Machine, t *Thread) {
		writeDateTime(m, t.arg(0), m.now())
		t.ret(0)
	})
	reg("cellRtcGetCurrentClock", func(m *Machine, t *Thread) {
		writeDateTime(m, t.arg(0), m.now().UTC())
		t.ret(0)
	})
	reg("cellRtcGetTick", func(m *Machine, t *Thread) { // (datetime*, tick*)
		m.Mem.Write64(t.arg(1), timeToTick(readDateTime(m, t.arg(0))))
		t.ret(0)
	})
	reg("cellRtcSetTick", func(m *Machine, t *Thread) { // (datetime*, tick*)
		writeDateTime(m, t.arg(0), tickToTime(m.Mem.Read64(t.arg(1))))
		t.ret(0)
	})
	reg("cellRtcSetTime_t", func(m *Machine, t *Thread) { // (datetime*, time_t)
		writeDateTime(m, t.arg(0), time.Unix(int64(t.arg64(1)), 0).UTC())
		t.ret(0)
	})
	reg("cellRtcGetWin32FileTime", func(m *Machine, t *Thread) { // (datetime*, u64*)
		tm := readDateTime(m, t.arg(0))
		ft := (tm.Unix()+11644473600)*10000000 + int64(tm.Nanosecond()/100)
		m.Mem.Write64(t.arg(1), uint64(ft))
		t.ret(0)
	})
	reg("cellRtcGetDayOfWeek", func(m *Machine, t *Thread) {
		d := time.Date(int(t.argS(0)), time.Month(t.argS(1)), int(t.argS(2)), 0, 0, 0, 0, time.UTC)
		t.ret(uint32(d.Weekday()))
	})

	reg("cellUserInfoGetList", func(m *Machine, t *Thread) { // (listNum*, listBuf*, currentUserId*)
		if t.arg(0) != 0 {
			m.Mem.Write32(t.arg(0), 1)
		}
		if t.arg(1) != 0 {
			m.Mem.Write32(t.arg(1), 1)
		}
		if t.arg(2) != 0 {
			m.Mem.Write32(t.arg(2), 1)
		}
		t.ret(0)
	})
	reg("cellOvisGetOverlayTableSize", func(m *Machine, t *Thread) { t.ret(0) })
	reg("cellOvisInitializeOverlayTable", func(m *Machine, t *Thread) { t.ret(0) })
	reg("cellVideoOutSetGamma", func(m *Machine, t *Thread) { t.ret(0) })
}

func writeDateTime(m *Machine, a uint32, tm time.Time) {
	m.Mem.Write16(a, uint16(tm.Year()))
	m.Mem.Write16(a+2, uint16(tm.Month()))
	m.Mem.Write16(a+4, uint16(tm.Day()))
	m.Mem.Write16(a+6, uint16(tm.Hour()))
	m.Mem.Write16(a+8, uint16(tm.Minute()))
	m.Mem.Write16(a+10, uint16(tm.Second()))
	m.Mem.Write32(a+12, uint32(tm.Nanosecond()/1000))
}

func readDateTime(m *Machine, a uint32) time.Time {
	return time.Date(int(m.Mem.Read16(a)), time.Month(m.Mem.Read16(a+2)), int(m.Mem.Read16(a+4)),
		int(m.Mem.Read16(a+6)), int(m.Mem.Read16(a+8)), int(m.Mem.Read16(a+10)), int(m.Mem.Read32(a+12))*1000, time.UTC)
}

// ticks: microseconds since 0001-01-01
var tickEpoch = time.Date(1, 1, 1, 0, 0, 0, 0, time.UTC)

func timeToTick(tm time.Time) uint64 {
	d := tm.Sub(tickEpoch) // overflows past 292 years; do it in parts
	_ = d
	days := tm.Sub(time.Date(tm.Year(), 1, 1, 0, 0, 0, 0, time.UTC))
	y := int64(tm.Year() - 1)
	daysBefore := y*365 + y/4 - y/100 + y/400
	return uint64(daysBefore*86400000000 + days.Microseconds())
}

func tickToTime(tk uint64) time.Time {
	const dayUS = 86400000000
	days := int64(tk / dayUS)
	rest := int64(tk % dayUS)
	// find the year
	y := int64(1)
	for {
		yd := int64(365)
		if (y%4 == 0 && y%100 != 0) || y%400 == 0 {
			yd = 366
		}
		if days < yd {
			break
		}
		days -= yd
		y++
		if y > 9999 {
			break
		}
	}
	return time.Date(int(y), 1, 1, 0, 0, 0, 0, time.UTC).AddDate(0, 0, int(days)).Add(time.Duration(rest) * time.Microsecond)
}
