package ctr

// APT: the applet manager. It hands the application its lock, its
// notification and parameter events, and the wake-up parameter that tells
// it to start running.

type aptParam struct {
	sender, cmd uint32
	data        []byte
	handle      uint32
}

// One APT state is shared by every APT:x session.
type aptState struct {
	lock          uint32
	notify, param *event
	params        []aptParam
	notifications []uint32
}

type aptService struct {
	n string
	m *Machine
}

func newAPT(m *Machine, name string) *aptService { return &aptService{n: name, m: m} }

func (v *aptService) name() string { return v.n }

func (v *aptService) state() *aptState {
	m := v.m
	if m.apt == nil {
		m.apt = &aptState{notify: &event{label: "APT notification"}, param: &event{label: "APT parameter"}}
		m.apt.lock = m.kern.add(&mutex{})
	}
	return m.apt
}

func (v *aptService) handle(c *ipc) {
	st := v.state()
	k := c.m.kern
	switch c.id() {
	case 0x0001: // GetLockHandle(flags) -> attributes, power button state, lock
		c.push(0, 0)
		c.handles(st.lock)
	case 0x0002: // Initialize(appid, attributes) -> notification event, parameter event
		c.handles(k.add(st.notify), k.add(st.param))
	case 0x0003: // Enable: the home menu wakes the application
		st.params = append(st.params, aptParam{sender: 0x101, cmd: 1})
		k.signal(st.param)
	case 0x0005: // GetAppletManInfo -> position, requested, home menu, active
		c.push(0, 0x300, 0x101, 0x300)
	case 0x0006: // GetAppletInfo -> title id, media type, registered, loaded, attributes
		c.push(0, 0, 0, 1, 1, 0)
	case 0x0009: // IsRegistered(appid) -> registered
		c.push(1)
	case 0x000b: // InquireNotification -> notification type
		var n uint32
		if len(st.notifications) > 0 {
			n, st.notifications = st.notifications[0], st.notifications[1:]
		}
		c.push(n)
	case 0x000d, 0x000e: // ReceiveParameter / GlanceParameter(appid, size)
		if len(st.params) == 0 {
			c.result(0xc8a0cff0)
			c.push(0, 0, 0)
			c.handles(0)
			return
		}
		p := st.params[0]
		if c.id() == 0x000d {
			st.params = st.params[1:]
		}
		addr, room := c.staticBuf(0)
		n := uint32(len(p.data))
		if n > room {
			n = room
		}
		c.m.mem.writeBytes(addr, p.data[:n])
		c.push(p.sender, p.cmd, n)
		c.handles(p.handle)
		c.trans = append(c.trans, n<<14|2, addr)
	case 0x000f: // CancelParameter -> success
		c.push(1)
	case 0x0043: // NotifyToWait
	case 0x0044: // GetSharedFont -> address, handle
		c.push(0)
		c.handles(0)
	case 0x004b: // AppletUtility
		c.push(0)
	case 0x004f: // SetAppCpuTimeLimit
	case 0x0050: // GetAppCpuTimeLimit -> percent
		c.push(30)
	case 0x0101: // CheckNew3DS
		c.push(0)
	default:
		// SetScreenCapPostPermission, ReplySleepQuery and the rest
		normal := c.header >> 6 & 63
		_ = normal
	}
}
