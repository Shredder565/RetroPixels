// This Source Code Form is subject to the terms of the Mozilla Public License,
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at https://mozilla.org/MPL/2.0/.
//
// Based on Cemu (https://github.com/cemu-project/Cemu), src/Cafe/OS/libs/nn_ac/nn_ac.cpp,
// rewritten in Go for RetroPixels.

package wiiu

// nn_ac, the network connection. Cemu always reports a connection; here
// there is none, but every out-parameter is still written the way Cemu
// writes it, so programs never read what was on their stack.

const (
	acNotConnected = 0xa0323800 // a failure nn::Result from nn_ac
	acStatusFailed = 0xffffffff // AC_STATUS_FAILED
)

func init() {
	ac := "nn_ac"
	ok := func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0) }
	for _, f := range []string{"ACInitialize", "ACFinalize", "Initialize__Q2_2nn2acFv", "Finalize__Q2_2nn2acFv"} {
		reg(ac, f, ok)
	}
	fail := func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, acNotConnected) }
	for _, f := range []string{"ACConnect", "ACConnectAsync", "Connect__Q2_2nn2acFv", "ConnectAsync__Q2_2nn2acFv"} {
		reg(ac, f, fail)
	}
	// out32 and out8 write v where argument i points, if it points anywhere
	out32 := func(m *Machine, cpu *CPU, i int, v uint32) {
		if p := arg(cpu, i); p != 0 {
			m.Mem.Write32(p, v)
		}
	}
	out8 := func(m *Machine, cpu *CPU, i int, v uint8) {
		if p := arg(cpu, i); p != 0 {
			m.Mem.Write8(p, v)
		}
	}
	// (bool *): not connected
	reg(ac, "IsApplicationConnected__Q2_2nn2acFPb", func(m *Machine, cpu *CPU, t *Thread) {
		out8(m, cpu, 0, 0)
		ret(cpu, 0)
	})
	// (u32 *)
	reg(ac, "ACIsApplicationConnected", func(m *Machine, cpu *CPU, t *Thread) {
		out32(m, cpu, 0, 0)
		ret(cpu, 0)
	})
	// (bool *connected, u32 *apType)
	reg(ac, "IsSystemConnected__Q2_2nn2acFPbPQ3_2nn2ac6ApType", func(m *Machine, cpu *CPU, t *Thread) {
		out8(m, cpu, 0, 0)
		out32(m, cpu, 1, 0)
		ret(cpu, 0)
	})
	// (config id, bool *)
	reg(ac, "IsConfigExisting__Q2_2nn2acFQ3_2nn2ac11ConfigIdNumPb", func(m *Machine, cpu *CPU, t *Thread) {
		out8(m, cpu, 1, 0)
		ret(cpu, 0)
	})
	status := func(m *Machine, cpu *CPU, t *Thread) {
		out32(m, cpu, 0, acStatusFailed)
		ret(cpu, 0)
	}
	for _, f := range []string{"GetConnectStatus__Q2_2nn2acFPQ3_2nn2ac6Status", "GetStatus__Q2_2nn2acFPQ3_2nn2ac6Status",
		"ACGetConnectStatus", "ACGetStatus"} {
		reg(ac, f, status)
	}
	// (nn::Result *): the last connect failed
	reg(ac, "GetConnectResult__Q2_2nn2acFPQ2_2nn6Result", func(m *Machine, cpu *CPU, t *Thread) {
		out32(m, cpu, 0, acNotConnected)
		ret(cpu, 0)
	})
	reg(ac, "GetLastErrorCode__Q2_2nn2acFPUi", func(m *Machine, cpu *CPU, t *Thread) {
		out32(m, cpu, 0, 0)
		ret(cpu, 0)
	})
	// (u32 *): no address
	addr := func(m *Machine, cpu *CPU, t *Thread) {
		out32(m, cpu, 0, 0)
		ret(cpu, acNotConnected)
	}
	for _, f := range []string{"ACGetAssignedAddress", "GetAssignedAddress__Q2_2nn2acFPUl",
		"GetAssignedSubnet__Q2_2nn2acFPUl"} {
		reg(ac, f, addr)
	}
	// (nn::Result *)
	reg(ac, "ACIsSuccess", func(m *Machine, cpu *CPU, t *Thread) {
		p := arg(cpu, 0)
		retBool(cpu, p != 0 && m.Mem.Read32(p)&0x80000000 == 0)
	})
	reg(ac, "ACIsFailure", func(m *Machine, cpu *CPU, t *Thread) {
		p := arg(cpu, 0)
		retBool(cpu, p != 0 && m.Mem.Read32(p)&0x80000000 != 0)
	})
}
