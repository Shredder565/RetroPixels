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

	// friends, accounts, Miiverse, online play: offline
	set("nn_fp", zero, "Initialize__Q2_2nn2fpFv", "Finalize__Q2_2nn2fpFv",
		"SetNotificationHandler__Q2_2nn2fpFUiPFQ3_2nn2fp16NotificationTypeUiPv_vPv",
		"IsOnline__Q2_2nn2fpFv", "HasLoggedIn__Q2_2nn2fpFv", "LoginAsync__Q2_2nn2fpFPFQ2_2nn6ResultPv_vPv",
		"GetFriendList__Q2_2nn2fpFPUiPUiUiUi", "UpdateGameMode__Q2_2nn2fpFPCQ3_2nn2fp8GameModePCw",
		"UpdateGameModeDescription__Q2_2nn2fpFPCw", "IsPreferenceValid__Q2_2nn2fpFv")
	set("nn_fp", one, "IsInitialized__Q2_2nn2fpFv")
	set("nn_act", zero, "Cancel__Q2_2nn3actFv")
	set("nsysnet", zero, "NSSLInit", "NSSLFinish")
	set("nlibcurl", zero, "curl_global_init_mem", "curl_global_init", "curl_global_cleanup")

	// add-on content: none installed
	a := "nn_aoc"
	set(a, zero, "AOC_Initialize", "AOC_Finalize", "AOC_CloseTitle", "AOC_UnmountTitle")
	reg(a, "AOC_CalculateWorkBufferSize", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0x200+arg(cpu, 0)*0x80) })
	reg(a, "AOC_ListTitle", func(m *Machine, cpu *CPU, t *Thread) {
		if p := arg(cpu, 0); p != 0 {
			m.Mem.Write32(p, 0)
		}
		ret(cpu, 0)
	})
	reg(a, "AOC_GetPurchaseInfo", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0xfffffffd) })
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
}
