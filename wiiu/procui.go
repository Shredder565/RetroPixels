package wiiu

// proc_ui (the foreground / background messages every program processes),
// sysapp (launching other titles) and the networking library, which is
// offline here.

// ProcUI is the program's foreground state.
type ProcUI struct {
	Exiting int // 0 = running; 1 = asked to quit; 2 = told it's exiting
}

const (
	procuiForeground = 0
	procuiBackground = 1
	procuiRelease    = 2
	procuiExiting    = 3
)

// RequestQuit asks the program to finish (as the HOME menu's Close does).
func (m *Machine) RequestQuit() {
	if m.procui.Exiting == 0 {
		m.procui.Exiting = 1
	}
}

func init() {
	p := "proc_ui"
	status := func(m *Machine, cpu *CPU, t *Thread) {
		switch m.procui.Exiting {
		case 0:
			ret(cpu, procuiForeground)
		case 1:
			m.procui.Exiting = 2
			ret(cpu, procuiRelease)
		default:
			ret(cpu, procuiExiting)
		}
	}
	reg(p, "ProcUIProcessMessages", status)
	reg(p, "ProcUISubProcessMessages", status)
	for _, n := range []string{"ProcUIInit", "ProcUIInitEx", "ProcUIRegisterCallback", "ProcUIRegisterBackgroundCallback",
		"ProcUIShutdown", "ProcUIDrawDoneRelease", "ProcUISetMEM1Storage", "ProcUISetSaveCallback",
		"ProcUISetBucketStorage", "ProcUIClearCallbacks", "ProcUISetCallbackStackSize", "ProcUIRegisterCallbackCore"} {
		reg(p, n, func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0) })
	}
	reg(p, "ProcUIIsRunning", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 1) })
	reg(p, "ProcUIInForeground", func(m *Machine, cpu *CPU, t *Thread) { retBool(cpu, m.procui.Exiting == 0) })
	reg(p, "ProcUIInShutdown", func(m *Machine, cpu *CPU, t *Thread) { retBool(cpu, m.procui.Exiting != 0) })
	reg(p, "ProcUICalcMemorySize", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0) })

	// leaving the program for another title (the menu, or the launcher that
	// started it) ends it here
	s := "sysapp"
	leave := func(m *Machine, cpu *CPU, t *Thread) {
		m.RequestQuit()
		ret(cpu, 0)
	}
	for _, n := range []string{"SYSLaunchMenu", "SYSRelaunchTitle", "_SYSLaunchMenuWithCheckingAccount",
		"SYSLaunchTitle", "_SYSLaunchTitleWithStdArgsInNoSplash", "SYSLaunchSettings", "SYSLaunchMiiStudio",
		"_SYSLaunchSettings", "SYSSwitchToBrowser", "SYSSwitchToEShop", "_SYSSwitchToMainApp", "SYSSwitchToOverlayFromHBM",
		"_SYSLaunchTitleByPathFromLauncher", "SYSLaunchTitleByPath"} {
		reg(s, n, leave)
	}
	reg(s, "SYSCheckTitleExists", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0) })
	reg(s, "_SYSGetSystemApplicationTitleId", func(m *Machine, cpu *CPU, t *Thread) {
		ret64(cpu, 0x0005001010040100|uint64(arg(cpu, 0)))
	})
	reg(s, "SYSGetCallerTitleId", func(m *Machine, cpu *CPU, t *Thread) { ret64(cpu, 0) })
	reg(s, "SYSGetCallerUPID", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0) })
	reg(s, "SYSGetLauncherArgs", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0) })
	reg(s, "_SYSGetLauncherArgs", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0) })
	reg(s, "SYSDeserializeSysArgs", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0) })
	reg(s, "_SYSDeserializeStandardArg", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0) })
	reg(s, "_SYSDirectlyReturnToCaller", leave)
	reg(s, "SYSClearSysArgs", func(m *Machine, cpu *CPU, t *Thread) {})

	// networking: offline
	n := "nsysnet"
	reg(n, "socket_lib_init", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0) })
	reg(n, "socket_lib_finish", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0) })
	for _, f := range []string{"socket", "connect", "bind", "listen", "accept", "send", "sendto", "recv", "recvfrom",
		"setsockopt", "getsockopt", "select", "socketclose", "shutdown", "getpeername", "getsockname", "gethostbyname",
		"getaddrinfo", "inet_aton", "ioctl", "SOSocket", "SOConnect", "SOBind", "SOSend", "SORecv"} {
		reg(n, f, func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0xffffffff) })
	}
	reg(n, "socketlasterr", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 50) }) // ENETDOWN
	reg(n, "__get_h_errno", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, m.threadBlock(t, "h_errno", 4)) })
	reg(n, "inet_ntoa", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, m.cstrAlloc("0.0.0.0")) })
	reg(n, "freeaddrinfo", func(m *Machine, cpu *CPU, t *Thread) {})

	ac := "nn_ac"
	for _, f := range []string{"ACInitialize", "ACFinalize", "Initialize__Q2_2nn2acFv", "Finalize__Q2_2nn2acFv"} {
		reg(ac, f, func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0) })
	}
	notConnected := func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0xa0323800) } // a "not connected" result
	for _, f := range []string{"ACConnect", "ACConnectAsync", "ACGetAssignedAddress", "Connect__Q2_2nn2acFv",
		"ConnectAsync__Q2_2nn2acFv", "GetAssignedAddress__Q2_2nn2acFPUl", "ACIsApplicationConnected",
		"IsApplicationConnected__Q2_2nn2acFPb"} {
		reg(ac, f, notConnected)
	}
}
