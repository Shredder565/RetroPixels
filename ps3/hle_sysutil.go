package ps3

// cellSysutil: system callbacks, settings, video and audio output, message
// dialogs and save data; and the PSN and network libraries, which report
// that the console is offline.

type sysutilState struct {
	cbs     [4]struct{ fn, arg uint32 }
	pending []sysutilEvent
}

type sysutilEvent struct {
	status, param uint64
	dialog        uint32 // a dialog's callback instead (fn), with arg
	dialogArg     uint32
}

func (m *Machine) sysutil() *sysutilState {
	if m.sysu == nil {
		m.sysu = &sysutilState{}
	}
	return m.sysu
}

func init() {
	reg("cellSysutilRegisterCallback", func(m *Machine, t *Thread) { // (slot, func, userdata)
		s := t.arg(0)
		if s > 3 {
			t.ret(0x8002b101)
			return
		}
		m.sysutil().cbs[s].fn = t.arg(1)
		m.sysutil().cbs[s].arg = t.arg(2)
		t.ret(0)
	})
	reg("cellSysutilUnregisterCallback", func(m *Machine, t *Thread) {
		if s := t.arg(0); s <= 3 {
			m.sysutil().cbs[s].fn = 0
		}
		t.ret(0)
	})
	reg("cellSysutilCheckCallback", func(m *Machine, t *Thread) {
		su := m.sysutil()
		t.ret(0)
		var next func()
		next = func() {
			if len(su.pending) == 0 {
				t.ret(0)
				return
			}
			e := su.pending[0]
			su.pending = su.pending[1:]
			if e.dialog != 0 {
				m.callGuest(t, e.dialog, []uint64{e.status, uint64(e.dialogArg)}, func(uint64) { next() })
				return
			}
			for _, cb := range su.cbs {
				if cb.fn != 0 {
					fn := cb
					m.callGuest(t, fn.fn, []uint64{e.status, e.param, uint64(fn.arg)}, func(uint64) { next() })
					// one call per registered callback: queue the rest
					for _, other := range su.cbs {
						if other.fn != 0 && other != fn {
							_ = other
						}
					}
					return
				}
			}
			next()
		}
		next()
	})
	reg("cellSysutilGetSystemParamInt", func(m *Machine, t *Thread) { // (id, value*)
		var v uint32
		switch t.arg(0) {
		case 0x111: // language: English (US)
			v = 1
		case 0x112: // enter button: cross
			v = 1
		case 0x114: // date format
			v = 1
		case 0x115: // time format: 24 hours
			v = 1
		case 0x121: // parental level
			v = 9
		case 0x151: // PAD rumble
			v = 1
		}
		m.Mem.Write32(t.arg(1), v)
		t.ret(0)
	})
	reg("cellSysutilGetSystemParamString", func(m *Machine, t *Thread) { // (id, buf, size)
		s := "RetroPixels"
		size := t.arg(2)
		if uint32(len(s)) >= size {
			s = s[:size-1]
		}
		m.Mem.WriteString(t.arg(1), s)
		t.ret(0)
	})

	// --- video out: one 1280x720 TV at 59.94 Hz
	reg("cellVideoOutGetState", func(m *Machine, t *Thread) { // (videoOut, deviceIndex, state*)
		s := t.arg(2)
		m.Mem.Fill(s, 0, 16)
		m.Mem.Write8(s, 0)   // enabled
		m.Mem.Write8(s+1, 1) // RGB
		m.Mem.Write8(s+8, 2) // 720p
		m.Mem.Write8(s+9, 0) // progressive
		m.Mem.Write8(s+10, 0)
		m.Mem.Write8(s+11, 2)  // 16:9
		m.Mem.Write16(s+14, 1) // 59.94 Hz
		t.ret(0)
	})
	reg("cellVideoOutGetResolution", func(m *Machine, t *Thread) { // (id, resolution*)
		w, h := uint16(0), uint16(0)
		switch t.arg(0) {
		case 1:
			w, h = 1920, 1080
		case 2:
			w, h = 1280, 720
		case 4:
			w, h = 720, 480
		case 5:
			w, h = 720, 576
		default:
			t.ret(0x8002b222)
			return
		}
		m.Mem.Write16(t.arg(1), w)
		m.Mem.Write16(t.arg(1)+2, h)
		t.ret(0)
	})
	reg("cellVideoOutGetResolutionAvailability", func(m *Machine, t *Thread) {
		switch t.arg(1) {
		case 2, 4, 5:
			t.ret(1)
		default:
			t.ret(0)
		}
	})
	reg("cellVideoOutGetNumberOfDevice", func(m *Machine, t *Thread) { t.ret(1) })
	reg("cellVideoOutGetDeviceInfo", func(m *Machine, t *Thread) { // (videoOut, deviceIndex, info*)
		i := t.arg(2)
		m.Mem.Fill(i, 0, 0x60)
		m.Mem.Write8(i, 2)   // HDMI
		m.Mem.Write8(i+1, 0) // available
		m.Mem.Write8(i+3, 1) // one mode
		m.Mem.Write8(i+8, 2) // 720p
		m.Mem.Write8(i+11, 2)
		m.Mem.Write16(i+14, 1)
		t.ret(0)
	})

	// --- audio out: stereo LPCM 48 kHz
	reg("cellAudioOutGetState", func(m *Machine, t *Thread) { // (audioOut, deviceIndex, state*)
		s := t.arg(2)
		m.Mem.Fill(s, 0, 20)
		m.Mem.Write8(s, 0)
		m.Mem.Write8(s+12, 0)  // LPCM
		m.Mem.Write8(s+13, 2)  // channels
		m.Mem.Write8(s+14, 4)  // 48 kHz
		m.Mem.Write32(s+16, 1) // 2ch layout
		t.ret(0)
	})
	reg("cellAudioOutConfigure", func(m *Machine, t *Thread) { t.ret(0) })
	reg("cellAudioOutGetSoundAvailability", func(m *Machine, t *Thread) { // (audioOut, type, fs, option)
		if t.arg(1) == 0 {
			t.ret(2)
		} else {
			t.ret(0)
		}
	})
	reg("cellAudioOutGetNumberOfDevice", func(m *Machine, t *Thread) { t.ret(1) })
	reg("cellAudioOutGetDeviceInfo", func(m *Machine, t *Thread) {
		i := t.arg(2)
		m.Mem.Fill(i, 0, 0x40)
		m.Mem.Write8(i, 2)
		m.Mem.Write8(i+2, 1)
		m.Mem.Write8(i+3, 1)
		m.Mem.Write8(i+4, 0)
		m.Mem.Write8(i+5, 2)
		m.Mem.Write8(i+6, 4)
		t.ret(0)
	})
	reg("cellAudioOutGetConfiguration", func(m *Machine, t *Thread) { // (audioOut, config*, option*)
		c := t.arg(1)
		m.Mem.Fill(c, 0, 32)
		m.Mem.Write8(c, 2) // channels
		m.Mem.Write8(c+1, 0)
		t.ret(0)
	})

	// --- message dialogs: logged; buttons answer "yes"/"OK"
	reg("cellMsgDialogOpen2", func(m *Machine, t *Thread) { // (type, msg*, callback, userData, extParam)
		msg := m.Mem.String(t.arg(1), 512)
		m.Logf("message dialog (type %x): %q", t.arg(0), msg)
		typ := t.arg(0)
		if cb := t.arg(2); cb != 0 && typ&0x70 != 0 { // has buttons
			m.sysutil().pending = append(m.sysutil().pending, sysutilEvent{status: 1, dialog: cb, dialogArg: t.arg(3)})
		}
		t.ret(0)
	})
	reg("cellMsgDialogClose", func(m *Machine, t *Thread) { t.ret(0) })
	reg("cellMsgDialogAbort", func(m *Machine, t *Thread) { t.ret(0) })
	reg("cellMsgDialogProgressBarInc", func(m *Machine, t *Thread) { t.ret(0) })
	reg("cellMsgDialogProgressBarSetMsg", func(m *Machine, t *Thread) { t.ret(0) })

	// --- PSN and network: offline
	const npNotInit = 0x8002aa01
	for _, n := range []string{"sceNpInit", "sceNp2Init", "sceNpMatching2Init2", "sceNpScoreInit", "sceNpLookupInit",
		"sceNpTusInit", "sceNpCommerce2Init", "sceNpTrophyInit", "sceNpBasicRegisterHandler", "sceNpManagerRegisterCallback",
		"sceNpManagerUnregisterCallback", "sceNpBasicUnregisterHandler", "sceNpTerm", "sceNp2Term", "sceNpScoreTerm",
		"sceNpLookupTerm", "sceNpTusTerm", "sceNpCommerce2Term", "sceNpTrophyTerm", "sceNpCustomMenuRegisterActions",
		"sceNpCustomMenuActionSetActivation", "sceNpBasicSetPresence", "cellNetCtlInit", "cellNetCtlTerm",
		"cellNetCtlAddHandler", "cellNetCtlDelHandler", "sys_net_initialize_network_ex", "sys_net_finalize_network",
		"sys_net_free_thread_context", "cellSysutilAvc2InitParam", "sceNpMatching2RegisterContextCallback",
		"sceNpMatching2RegisterRoomEventCallback", "sceNpMatching2RegisterLobbyMessageCallback",
		"sceNpMatching2SetDefaultRequestOptParam", "sceNpScoreSetTimeout", "sceNpTrophyDestroyContext",
		"sceNpTrophyDestroyHandle", "sceNpTrophyAbortHandle", "sceNpMatching2ContextStop", "sceNpMatching2DestroyContext",
		"sceNpSignalingDestroyCtx", "sceNpScoreDestroyTitleCtx", "sceNpLookupDestroyTitleCtx", "sceNpTusDestroyTitleCtx"} {
		reg(n, func(m *Machine, t *Thread) { t.ret(0) })
	}
	reg("sceNpManagerGetStatus", func(m *Machine, t *Thread) { // offline
		m.Mem.Write32(t.arg(0), 0xffffffff)
		t.ret(0)
	})
	reg("sceNpManagerGetNpId", func(m *Machine, t *Thread) { t.ret(0x8002aa0d) }) // not signed in
	reg("sceNpManagerGetOnlineId", func(m *Machine, t *Thread) { t.ret(0x8002aa0d) })
	reg("sceNpManagerGetOnlineName", func(m *Machine, t *Thread) { t.ret(0x8002aa0d) })
	reg("sceNpManagerGetCachedInfo", func(m *Machine, t *Thread) { t.ret(0x8002aa0d) })
	reg("sceNpManagerGetContentRatingFlag", func(m *Machine, t *Thread) {
		m.Mem.Write32(t.arg(0), 0)
		m.Mem.Write32(t.arg(1), 0)
		t.ret(0)
	})
	reg("sceNpManagerGetChatRestrictionFlag", func(m *Machine, t *Thread) {
		m.Mem.Write32(t.arg(0), 0)
		t.ret(0)
	})
	reg("sceNpDrmIsAvailable", func(m *Machine, t *Thread) { // (k_licensee*, path)
		k := make([]byte, 16)
		m.Mem.ReadBytes(t.arg(0), k)
		if m.klic == nil {
			m.klic = k
		}
		m.Logf("sceNpDrmIsAvailable(%x, %s)", k, m.Mem.String(t.arg(1), 256))
		t.ret(0)
	})
	reg("sceNpDrmVerifyUpgradeLicense", func(m *Machine, t *Thread) { t.ret(0) }) // the full game
	reg("sceNpTrophyCreateContext", func(m *Machine, t *Thread) {
		m.Mem.Write32(t.arg(0), 1)
		t.ret(0)
	})
	reg("sceNpTrophyCreateHandle", func(m *Machine, t *Thread) {
		m.Mem.Write32(t.arg(0), 1)
		t.ret(0)
	})
	reg("sceNpTrophyRegisterContext", func(m *Machine, t *Thread) { // (ctx, handle, statusCb, arg, options)
		// the callback gets told installation finished
		cb, arg := t.arg(2), t.arg(3)
		t.ret(0)
		if cb != 0 {
			// SCE_NP_TROPHY_STATUS_PROCESSING_COMPLETE = 6
			m.callGuest(t, cb, []uint64{uint64(t.arg(0)), 6, 100, 100, uint64(arg)}, func(uint64) { t.ret(0) })
		}
	})
	reg("sceNpTrophyGetRequiredDiskSpace", func(m *Machine, t *Thread) {
		m.Mem.Write64(t.arg(2), 0)
		t.ret(0)
	})
	reg("sceNpTrophyUnlockTrophy", func(m *Machine, t *Thread) {
		m.Logf("trophy %d unlocked", t.arg(2))
		if t.arg(3) != 0 {
			m.Mem.Write32(t.arg(3), 0xffffffff)
		}
		t.ret(0)
	})
	reg("cellNetCtlGetState", func(m *Machine, t *Thread) {
		m.Mem.Write32(t.arg(0), 0) // disconnected
		t.ret(0)
	})
	reg("cellNetCtlGetInfo", func(m *Machine, t *Thread) { t.ret(0x80130106) }) // not connected
	reg("cellNetCtlNetStartDialogLoadAsync", func(m *Machine, t *Thread) { t.ret(0x80130106) })
	reg("cellNetCtlNetStartDialogUnloadAsync", func(m *Machine, t *Thread) { t.ret(0) })
	reg("_sys_net_errno_loc", func(m *Machine, t *Thread) {
		a, ok := m.misc["errno"].(uint32)
		if !ok {
			a = m.sysAlloc(4, 4)
			m.misc["errno"] = a
		}
		t.ret(a)
	})
	for _, n := range []string{"socket", "bind", "connect", "send", "sendto", "recv", "recvfrom", "setsockopt",
		"socketpoll", "socketclose", "gethostbyname", "gethostbyaddr"} {
		reg(n, func(m *Machine, t *Thread) { t.retS(-1) })
	}
	reg("inet_addr", func(m *Machine, t *Thread) { t.ret(0xffffffff) })
	reg("inet_ntoa", func(m *Machine, t *Thread) {
		a := m.sysAlloc(16, 8)
		m.Mem.WriteString(a, "0.0.0.0")
		t.ret(a)
	})
}
