// Original code for RetroPixels. Not derived from Cemu.

package wiiu

// nn_ac, the network connection manager. RetroPixels never has a network
// connection: connect calls fail and every query says "not connected".
// Each query still writes all of its out-parameters, so a program never
// goes on with whatever happened to be on its stack.

const (
	acNotConnected = 0xa0323800 // the nn::Result for a failed connection
	acStatusFailed = 0xffffffff // ACStatus: AC_STATUS_FAILED
)

func init() {
	const ac = "nn_ac"
	// put writes v (of the given byte width) where argument i points.
	put := func(m *Machine, cpu *CPU, i int, width int, v uint32) {
		p := arg(cpu, i)
		if p == 0 {
			return
		}
		if width == 1 {
			m.Mem.Write8(p, uint8(v))
		} else {
			m.Mem.Write32(p, v)
		}
	}
	// answer makes a function that fills its out-parameters and returns r.
	type out struct {
		arg, width int
		v          uint32
	}
	answer := func(r uint32, outs ...out) hleFunc {
		return func(m *Machine, cpu *CPU, t *Thread) {
			for _, o := range outs {
				put(m, cpu, o.arg, o.width, o.v)
			}
			ret(cpu, r)
		}
	}
	many := func(fn hleFunc, names ...string) {
		for _, n := range names {
			reg(ac, n, fn)
		}
	}

	many(answer(0), "ACInitialize", "ACFinalize", "Initialize__Q2_2nn2acFv", "Finalize__Q2_2nn2acFv")
	many(answer(acNotConnected), "ACConnect", "ACConnectAsync", "Connect__Q2_2nn2acFv", "ConnectAsync__Q2_2nn2acFv")

	// (bool *connected)
	reg(ac, "IsApplicationConnected__Q2_2nn2acFPb", answer(0, out{0, 1, 0}))
	// (u32 *connected)
	reg(ac, "ACIsApplicationConnected", answer(0, out{0, 4, 0}))
	// (bool *connected, ApType *type)
	reg(ac, "IsSystemConnected__Q2_2nn2acFPbPQ3_2nn2ac6ApType", answer(0, out{0, 1, 0}, out{1, 4, 0}))
	// (config id, bool *exists)
	reg(ac, "IsConfigExisting__Q2_2nn2acFQ3_2nn2ac11ConfigIdNumPb", answer(0, out{1, 1, 0}))
	// (Status *)
	many(answer(0, out{0, 4, acStatusFailed}),
		"GetConnectStatus__Q2_2nn2acFPQ3_2nn2ac6Status", "GetStatus__Q2_2nn2acFPQ3_2nn2ac6Status",
		"ACGetConnectStatus", "ACGetStatus")
	// (nn::Result *): how the last attempt ended
	reg(ac, "GetConnectResult__Q2_2nn2acFPQ2_2nn6Result", answer(0, out{0, 4, acNotConnected}))
	// (u32 *code)
	reg(ac, "GetLastErrorCode__Q2_2nn2acFPUi", answer(0, out{0, 4, 0}))
	// (u32 *ip): no address without a connection
	many(answer(acNotConnected, out{0, 4, 0}),
		"ACGetAssignedAddress", "GetAssignedAddress__Q2_2nn2acFPUl", "GetAssignedSubnet__Q2_2nn2acFPUl")

	// (nn::Result *): a result is a failure when its top bit is set
	resultIs := func(failure bool) hleFunc {
		return func(m *Machine, cpu *CPU, t *Thread) {
			p := arg(cpu, 0)
			retBool(cpu, p != 0 && (m.Mem.Read32(p)>>31 == 1) == failure)
		}
	}
	reg(ac, "ACIsSuccess", resultIs(false))
	reg(ac, "ACIsFailure", resultIs(true))
}
