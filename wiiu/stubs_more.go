package wiiu

// More library calls games make at start-up that need little or no work
// here: online services (offline), add-on content (none), sound effects
// RetroPixels doesn't run, and small coreinit helpers.

func init() {
	zero := func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0) }
	one := func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 1) }
	none := func(m *Machine, cpu *CPU, t *Thread) {}
	set := func(lib string, f hleFunc, names ...string) {
		for _, n := range names {
			if hleFuncs[lib][n] == nil {
				reg(lib, n, f)
			}
		}
	}

	c := "coreinit"
	set(c, one, "FSGetClientNum")
	set(c, zero, "OSSetExceptionCallback", "OSSetExceptionCallbackEx")
	// the C library's locks: one word each, never contended between programs
	set(c, func(m *Machine, cpu *CPU, t *Thread) {
		if p := arg(cpu, 0); p != 0 {
			m.Mem.Write32(p, 1)
		}
	}, "__ghs_mtx_init")
	set(c, none, "__ghs_mtx_dst", "__ghs_mtx_lock", "__ghs_mtx_unlock")
	reg(c, "__ghs_flock_ptr", func(m *Machine, cpu *CPU, t *Thread) {
		if m.flockWord == 0 {
			m.flockWord = m.sysAlloc(16, 16)
		}
		ret(cpu, m.flockWord)
	})

	// accounts, Miiverse, online play: offline (friends are in nn_fp.go)
	set("nn_act", zero, "Cancel__Q2_2nn3actFv")
	set("nsysnet", zero, "NSSLInit", "NSSLFinish")
	set("nlibcurl", zero, "curl_global_init_mem", "curl_global_init", "curl_global_cleanup")

	// add-on content: none installed
	a := "nn_aoc"
	set(a, zero, "AOC_Initialize", "AOC_Finalize", "AOC_CloseTitle", "AOC_UnmountTitle")
	reg(a, "AOC_CalculateWorkBufferSize", func(m *Machine, cpu *CPU, t *Thread) {
		ret(cpu, 0x80+min(arg(cpu, 0), 256)*0x61) // as Cemu
	})
	reg(a, "AOC_ListTitle", func(m *Machine, cpu *CPU, t *Thread) {
		if p := arg(cpu, 0); p != 0 {
			m.Mem.Write32(p, 0)
		}
		ret(cpu, 0)
	})
	// (u32 *bought, u64 title in r5:r6, u16 *ids, count, work, size): nothing bought
	reg(a, "AOC_GetPurchaseInfo", func(m *Machine, cpu *CPU, t *Thread) {
		if p, n := arg(cpu, 0), arg(cpu, 5); p != 0 {
			m.Mem.Fill(p, 0, min(n, 0x10000)*4)
		}
		ret(cpu, 0)
	})
	reg(a, "AOC_OpenTitle", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0xfffffffd) })

	// sound
	for _, s := range []string{"snd_core", "sndcore2"} {
		set(s, zero, "AXUserIsProtected", "AXRmtGetSamplesLeft", "AXRmtAdvancePtr", "AXRmtGetSamples")
		set(s, one, "AXCheckVoiceOffsets")
	}
	// sound effects (reverb, delay, chorus): set up, but not heard here
	for _, s := range []string{"snd_user", "snduser2"} {
		for _, fx := range []string{"ReverbHi", "ReverbStd", "Delay", "Chorus", "ReverbHiDpl2", "ReverbStdDpl2",
			"DelayDpl2", "ChorusDpl2", "MultiChReverb", "MultiChDelay", "MultiChChorus", "ReverbHiExp", "DelayExp",
			"ChorusExp", "ReverbStdExp"} {
			reg(s, "AXFX"+fx+"GetMemSize", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0x100) })
			set(s, one, "AXFX"+fx+"Init", "AXFX"+fx+"Settings", "AXFX"+fx+"SettingsUpdate")
			set(s, none, "AXFX"+fx+"Shutdown", "AXFX"+fx+"Callback")
		}
		set(s, none, "AXFXSetHooks", "AXFXGetHooks")
	}

	// controllers
	set("vpad", none, "VPADSetGyroDirReviseBase", "VPADDisableGyroDirRevise", "VPADEnableGyroDirRevise",
		"VPADSetGyroDirReviseParam", "VPADDisableStickCrossClamp", "VPADEnableStickCrossClamp")
	set("padscore", none, "KPADDisableStickCrossClamp", "KPADEnableStickCrossClamp", "KPADSetCrossStickEmulationParamsL",
		"KPADSetCrossStickEmulationParamsR")

	// the error viewer
	set("erreula", zero, "ErrEulaGetSelectButtonNumError__3RplFv", "ErrEulaJump__3RplFPCcUi",
		"ErrEulaIsDecideSelectButtonError__3RplFv")

	// no Wii Remote speaker here
	m1 := func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0xffffffff) }
	set("padscore", zero, "WPADGetSensorBarPosition", "WPADIsSpeakerEnabled", "WPADGetSpeakerVolume",
		"WPADSetSpeakerVolume", "WPADCanSendStreamData")
	set("padscore", m1, "WPADControlSpeaker", "WPADSendStreamData")
	for _, s := range []string{"snd_core", "sndcore2"} {
		set(s, zero, "AXRegisterExceedCallback", "AXSetDspLoadLimit", "AXGetDspLoadLimit", "AXSetAuxAReturnVolume",
			"AXSetAuxBReturnVolume", "AXSetAuxCReturnVolume", "AXGetDspCycles", "AXGetMaxDspCycles")
	}
	for _, s := range []string{"snd_user", "snduser2"} {
		set(s, one, "AXFXMultiChReverbParametersPreset", "AXFXReverbHiParametersPreset", "AXFXReverbStdParametersPreset")
	}
	set("gx2", none, "GX2SetVerifyCallback", "GX2RSetDebugOptions", "GX2SetDebugMode", "GX2SetVerifyLevel")

	set("padscore", none, "KPADReset")
	set("nn_act", zero, "IsNetworkAccountEx__Q2_2nn3actFUc")
	// the account's UUID: any fixed 16 bytes
	uuid := func(m *Machine, cpu *CPU, t *Thread) {
		if p := arg(cpu, 0); p != 0 {
			m.Mem.WriteBytes(p, []byte("RetroPixelsWiiU1"))
		}
		ret(cpu, 0)
	}
	set("nn_act", uuid, "GetUuid__Q2_2nn3actFP7ACTUuid", "GetUuidEx__Q2_2nn3actFP7ACTUuidUc",
		"GetUuidEx__Q2_2nn3actFP7ACTUuidUcUi")

	set("padscore", zero, "KPADSetControlDpdCallback", "KPADSetControlMplsCallback", "KPADSetSamplingCallback",
		"KPADSetConnectCallback", "WPADDisableBluetooth", "WPADEnableBluetooth")
	// no USB keyboard
	k := "nsyskbd"
	set(k, zero, "KBDInit", "KBDInitRegionJP", "KBDInitRegionUS", "KBDInitRegionEU", "KBDSetCountry", "KBDTeardown",
		"KBDSetAttachCallback", "KBDSetKeyCallback")
	reg(k, "KBDGetChannelStatus", func(m *Machine, cpu *CPU, t *Thread) {
		if p := arg(cpu, 1); p != 0 {
			m.Mem.Write32(p, 1) // disconnected
		}
		ret(cpu, 0)
	})
	// (channel, KBDKeyState *): no key, 14 bytes
	reg(k, "KBDGetKey", func(m *Machine, cpu *CPU, t *Thread) {
		if ch, p := arg(cpu, 0), arg(cpu, 1); ch < 4 && p != 0 {
			m.Mem.Fill(p, 0, 0xe)
			m.Mem.Write8(p, uint8(ch))
		}
		ret(cpu, 0)
	})
	set("vpad", zero, "VPADIsEnableGyroZeroPlay", "VPADIsEnableGyroZeroDrift", "VPADIsEnableGyroDirRevise",
		"VPADIsEnableGyroAccRevise")
	for _, s := range []string{"snd_core", "sndcore2"} {
		reg(s, "AXGetDRCVSMode", func(m *Machine, cpu *CPU, t *Thread) {
			if p := arg(cpu, 0); p != 0 {
				m.Mem.Write32(p, 0)
			}
			ret(cpu, 0)
		})
	}
}
