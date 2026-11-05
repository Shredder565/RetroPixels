package ctr

import "fmt"

// System services, answered in Go when the application sends an IPC
// request (SendSyncRequest) on a session. Each command reads its request
// from the thread's command buffer (TLS + 0x80) and writes the reply
// there.

type service interface {
	name() string
	handle(c *ipc)
}

// ipc is one request.
type ipc struct {
	m      *Machine
	buf    uint32
	header uint32
	res    uint32   // the result code
	out    []uint32 // normal reply words after the result
	trans  []uint32 // translate reply words
}

func (c *ipc) id() uint32       { return c.header >> 16 }
func (c *ipc) arg(i int) uint32 { return c.m.mem.Read32(c.buf + 4*uint32(i+1)) }
func (c *ipc) result(r uint32)  { c.res = r }
func (c *ipc) push(v ...uint32) { c.out = append(c.out, v...) }
func (c *ipc) push64(v uint64)  { c.out = append(c.out, uint32(v), uint32(v>>32)) }
func (c *ipc) handles(h ...uint32) {
	c.trans = append(c.trans, uint32(len(h)-1)<<26)
	c.trans = append(c.trans, h...)
}

// staticBuf returns where the client wants static buffer i written (its
// receive descriptors at TLS + 0x180) and the room there.
func (c *ipc) staticBuf(i int) (uint32, uint32) {
	tls := c.buf - 0x80
	d := c.m.mem.Read32(tls + 0x180 + 8*uint32(i))
	return c.m.mem.Read32(tls + 0x184 + 8*uint32(i)), d >> 14
}

type services struct {
	m      *Machine
	byName map[string]service
	srv    *srvService
	log    map[string]bool
}

func newServices(m *Machine) *services {
	s := &services{m: m, byName: map[string]service{}, log: map[string]bool{}}
	s.srv = &srvService{s: s}
	for _, v := range []service{
		newAPT(m, "APT:U"), newAPT(m, "APT:A"), newAPT(m, "APT:S"),
		newGSP(m), newHID(m, "hid:USER"), newHID(m, "hid:SPVR"),
		newFS(m, "fs:USER"), newCFG(m, "cfg:u"), newCFG(m, "cfg:s"), newCFG(m, "cfg:i"),
		&ptmService{"ptm:u"}, &ptmService{"ptm:sysm"},
		newDSP(m),
	} {
		s.byName[v.name()] = v
	}
	return s
}

func (s *services) port(name string) service {
	switch name {
	case "srv:":
		return s.srv
	case "err:f":
		return &stubService{n: "err:f", m: s.m}
	}
	return nil
}

func (s *services) get(name string) service {
	if v, ok := s.byName[name]; ok {
		return v
	}
	if name == "APT:U" || name == "apt:u" {
		return s.byName["APT:U"]
	}
	if v, ok := s.byName[name]; ok {
		return v
	}
	v := &stubService{n: name, m: s.m}
	s.byName[name] = v
	return v
}

func (s *services) call(svc service, buf uint32) {
	c := &ipc{m: s.m, buf: buf, header: s.m.mem.Read32(buf)}
	svc.handle(c)
	c.out = append([]uint32{c.res}, c.out...)
	normal := uint32(len(c.out))
	s.m.mem.Write32(buf, c.id()<<16|normal<<6|uint32(len(c.trans)))
	for i, v := range c.out {
		s.m.mem.Write32(buf+4+4*uint32(i), v)
	}
	for i, v := range c.trans {
		s.m.mem.Write32(buf+4+4*(normal+uint32(i)), v)
	}
}

// srv: the service manager.
type srvService struct {
	s            *services
	notification *event
}

func (*srvService) name() string { return "srv:" }

func (v *srvService) handle(c *ipc) {
	k := c.m.kern
	switch c.id() {
	case 0x0001: // RegisterClient
	case 0x0002: // EnableNotification -> semaphore
		v.notification = &event{label: "srv notification"}
		c.push()
		c.handles(k.add(&semaphore{max: 1}))
	case 0x0005: // GetServiceHandle(name, len, flags) -> session
		b := make([]byte, 0, 8)
		for i := 0; i < 2; i++ {
			w := c.arg(i)
			for j := 0; j < 4; j++ {
				b = append(b, byte(w>>(8*j)))
			}
		}
		n := c.arg(2)
		if n > 8 {
			n = 8
		}
		name := string(b[:n])
		c.push()
		c.handles(k.add(&session{svc: v.s.get(name)}))
	case 0x0009, 0x000a, 0x000b: // Subscribe, Unsubscribe, ReceiveNotification
		c.push(0)
	default:
		c.m.logf("srv: command %04x", c.id())
	}
}

// stubService answers every command with success and zeros, for the
// services a program opens but doesn't need answers from.
type stubService struct {
	n string
	m *Machine
}

func (v *stubService) name() string { return v.n }

func (v *stubService) handle(c *ipc) {
	key := fmt.Sprintf("%s %04x", v.n, c.id())
	if !c.m.svcs.log[key] {
		c.m.svcs.log[key] = true
		c.m.logf("service %s: command %04x answered with success", v.n, c.id())
	}
	normal := c.header >> 6 & 63
	for i := uint32(0); i < normal && i < 4; i++ {
		c.push(0)
	}
}
