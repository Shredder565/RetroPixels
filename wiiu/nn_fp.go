// This Source Code Form is subject to the terms of the Mozilla Public License,
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at https://mozilla.org/MPL/2.0/.
//
// Based on Cemu (https://github.com/cemu-project/Cemu), src/Cafe/OS/libs/nn_fp/nn_fp.cpp
// and src/Cafe/IOSU/legacy/iosu_fpd.cpp, rewritten in Go for RetroPixels.

package wiiu

// nn_fp, friends: offline, as Cemu is with online play turned off. Logging
// in "succeeds" without logging in (Splatoon waits for it otherwise).

func init() {
	f := "nn_fp"
	zero := func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0) }
	for _, n := range []string{"Initialize__Q2_2nn2fpFv", "InitializeAdmin__Q2_2nn2fpFv", "Finalize__Q2_2nn2fpFv",
		"FinalizeAdmin__Q2_2nn2fpFv", "SetNotificationHandler__Q2_2nn2fpFUiPFQ3_2nn2fp16NotificationTypeUiPv_vPv",
		"IsOnline__Q2_2nn2fpFv", "HasLoggedIn__Q2_2nn2fpFv", "GetMyPrincipalId__Q2_2nn2fpFv",
		"UpdateGameMode__Q2_2nn2fpFPCQ3_2nn2fp8GameModePCw", "UpdateGameMode__Q2_2nn2fpFPCQ3_2nn2fp8GameModePCwUi",
		"UpdateGameModeDescription__Q2_2nn2fpFPCw", "IsPreferenceValid__Q2_2nn2fpFv"} {
		reg(f, n, zero)
	}
	one := func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 1) }
	reg(f, "IsInitialized__Q2_2nn2fpFv", one)
	reg(f, "IsInitializedAdmin__Q2_2nn2fpFv", one)
	// (callback, user): callback(nn::Result *, user) with success
	reg(f, "LoginAsync__Q2_2nn2fpFPFQ2_2nn6ResultPv_vPv", func(m *Machine, cpu *CPU, t *Thread) {
		res := m.threadBlock(nil, "nn_fp.result", 4)
		m.Mem.Write32(res, 0)
		m.queueCallback(cpu.Core, arg(cpu, 0), res, arg(cpu, 1))
		ret(cpu, 0)
	})
	// (u32 *pids, u32 *count, start, max): no friends
	list := func(m *Machine, cpu *CPU, t *Thread) {
		if p := arg(cpu, 1); p != 0 {
			m.Mem.Write32(p, 0)
		}
		ret(cpu, 0)
	}
	for _, n := range []string{"GetFriendList__Q2_2nn2fpFPUiT1UiT3", "GetFriendRequestList__Q2_2nn2fpFPUiT1UiT3",
		"GetFriendListAll__Q2_2nn2fpFPUiT1UiT3"} {
		reg(f, n, list)
	}
}
