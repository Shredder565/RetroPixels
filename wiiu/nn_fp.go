package wiiu

// nn_fp, the friend list service. RetroPixels plays offline: the service
// says it is set up, has no friends and no requests, and reports a
// successful login without going anywhere (some games, Splatoon among them,
// wait for the login callback before going on).

func init() {
	const lib = "nn_fp"
	answer := func(v uint32) hleFunc {
		return func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, v) }
	}
	for _, name := range []string{
		"Initialize__Q2_2nn2fpFv",
		"InitializeAdmin__Q2_2nn2fpFv",
		"Finalize__Q2_2nn2fpFv",
		"FinalizeAdmin__Q2_2nn2fpFv",
		"SetNotificationHandler__Q2_2nn2fpFUiPFQ3_2nn2fp16NotificationTypeUiPv_vPv",
		"IsOnline__Q2_2nn2fpFv",
		"HasLoggedIn__Q2_2nn2fpFv",
		"GetMyPrincipalId__Q2_2nn2fpFv",
		"UpdateGameMode__Q2_2nn2fpFPCQ3_2nn2fp8GameModePCw",
		"UpdateGameMode__Q2_2nn2fpFPCQ3_2nn2fp8GameModePCwUi",
		"UpdateGameModeDescription__Q2_2nn2fpFPCw",
		"IsPreferenceValid__Q2_2nn2fpFv",
	} {
		reg(lib, name, answer(0))
	}
	reg(lib, "IsInitialized__Q2_2nn2fpFv", answer(1))
	reg(lib, "IsInitializedAdmin__Q2_2nn2fpFv", answer(1))

	// LoginAsync(callback, context): later, callback(&result, context)
	// with a success result
	reg(lib, "LoginAsync__Q2_2nn2fpFPFQ2_2nn6ResultPv_vPv", func(m *Machine, cpu *CPU, t *Thread) {
		result := m.threadBlock(nil, "nn_fp.result", 4)
		m.Mem.Write32(result, 0)
		m.queueCallback(cpu.Core, arg(cpu, 0), result, arg(cpu, 1))
		ret(cpu, 0)
	})

	// Get...List(ids, count, offset, max): an empty list
	emptyList := func(m *Machine, cpu *CPU, t *Thread) {
		if count := arg(cpu, 1); count != 0 {
			m.Mem.Write32(count, 0)
		}
		ret(cpu, 0)
	}
	for _, name := range []string{
		"GetFriendList__Q2_2nn2fpFPUiT1UiT3",
		"GetFriendRequestList__Q2_2nn2fpFPUiT1UiT3",
		"GetFriendListAll__Q2_2nn2fpFPUiT1UiT3",
	} {
		reg(lib, name, emptyList)
	}
}
