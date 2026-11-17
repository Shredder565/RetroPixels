package wiiu

// nn_ac, the network connection manager. RetroPixels has no network for
// games, so connecting fails and every status says "not connected". Every
// out-parameter is still filled in, so a game never reads leftovers from its
// own stack.

const (
	acErrNoConnection = 0xa0323800 // nn::Result: nn_ac, not connected
	acStatusFailed    = 0xffffffff
)

func init() {
	const lib = "nn_ac"
	result := func(v uint32) hleFunc {
		return func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, v) }
	}
	// put stores v (of the given width in bytes) where argument i points
	put := func(m *Machine, cpu *CPU, i, width int, v uint32) {
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
	// fills returns a function that fills in out-parameters (argument
	// index, width, value) and returns r
	type out struct {
		arg, width int
		v          uint32
	}
	fills := func(r uint32, outs ...out) hleFunc {
		return func(m *Machine, cpu *CPU, t *Thread) {
			for _, o := range outs {
				put(m, cpu, o.arg, o.width, o.v)
			}
			ret(cpu, r)
		}
	}

	for _, n := range []string{"ACInitialize", "ACFinalize", "Initialize__Q2_2nn2acFv", "Finalize__Q2_2nn2acFv"} {
		reg(lib, n, result(0))
	}
	for _, n := range []string{"ACConnect", "ACConnectAsync", "Connect__Q2_2nn2acFv", "ConnectAsync__Q2_2nn2acFv"} {
		reg(lib, n, result(acErrNoConnection))
	}
	reg(lib, "IsApplicationConnected__Q2_2nn2acFPb", fills(0, out{0, 1, 0}))
	reg(lib, "ACIsApplicationConnected", fills(0, out{0, 4, 0}))
	reg(lib, "IsSystemConnected__Q2_2nn2acFPbPQ3_2nn2ac6ApType", fills(0, out{0, 1, 0}, out{1, 4, 0}))
	reg(lib, "IsConfigExisting__Q2_2nn2acFQ3_2nn2ac11ConfigIdNumPb", fills(0, out{1, 1, 0}))
	for _, n := range []string{"GetConnectStatus__Q2_2nn2acFPQ3_2nn2ac6Status", "GetStatus__Q2_2nn2acFPQ3_2nn2ac6Status",
		"ACGetConnectStatus", "ACGetStatus"} {
		reg(lib, n, fills(0, out{0, 4, acStatusFailed}))
	}
	reg(lib, "GetConnectResult__Q2_2nn2acFPQ2_2nn6Result", fills(0, out{0, 4, acErrNoConnection}))
	reg(lib, "GetLastErrorCode__Q2_2nn2acFPUi", fills(0, out{0, 4, 0}))
	for _, n := range []string{"ACGetAssignedAddress", "GetAssignedAddress__Q2_2nn2acFPUl", "GetAssignedSubnet__Q2_2nn2acFPUl"} {
		reg(lib, n, fills(acErrNoConnection, out{0, 4, 0}))
	}

	// ACIsSuccess / ACIsFailure(nn::Result *): a result is a failure when
	// its top bit is set
	failed := func(m *Machine, cpu *CPU) (bool, bool) {
		p := arg(cpu, 0)
		if p == 0 {
			return false, false
		}
		return m.Mem.Read32(p)&0x80000000 != 0, true
	}
	reg(lib, "ACIsSuccess", func(m *Machine, cpu *CPU, t *Thread) {
		f, ok := failed(m, cpu)
		retBool(cpu, ok && !f)
	})
	reg(lib, "ACIsFailure", func(m *Machine, cpu *CPU, t *Thread) {
		f, ok := failed(m, cpu)
		retBool(cpu, ok && f)
	})
}
