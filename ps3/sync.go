package ps3

import "fmt"

// Kernel synchronisation objects (mutexes, condition variables, semaphores,
// event flags, event queues and ports) and the lightweight mutexes and
// condition variables of sysPrxForUser, which live in the game's memory.
//
// Waiting threads are blocked with a function that tries to finish the
// wait; the scheduler calls it when it looks for a thread to run.

type mutex struct {
	name      string
	recursive bool
	owner     *Thread
	count     uint32
	conds     int
}

type cond struct {
	mu      *mutex
	waiters []*condWaiter
}

type condWaiter struct {
	t        *Thread
	signaled bool
}

type semaphore struct {
	val, max int32
}

type eventFlag struct {
	bits    uint64
	waiters int
}

type event struct{ source, d1, d2, d3 uint64 }

type eventQueue struct {
	id     uint32
	key    uint64
	size   int
	typ    uint32 // 1 PPU, 2 SPU
	events []event
	name   string
}

type eventPort struct {
	name  uint64
	queue *eventQueue
}

const timeoutNone = 0

func (m *Machine) newObj(o any) uint32 {
	id := m.newID()
	m.objects[id] = o
	return id
}

// tryLock takes mu for t if it can.
func (mu *mutex) tryLock(t *Thread) bool {
	if mu.owner == nil {
		mu.owner = t
		mu.count = 1
		return true
	}
	return false
}

func init() {
	// --- mutex
	sysc(100, func(m *Machine, t *Thread) { // sys_mutex_create(id*, attr*)
		attr := t.arg(1)
		mu := &mutex{recursive: m.Mem.Read32(attr+4) == 0x10, name: m.Mem.String(attr+24, 8)}
		m.Mem.Write32(t.arg(0), m.newObj(mu))
		t.ret(0)
	})
	sysc(101, func(m *Machine, t *Thread) { // destroy
		mu, ok := m.objects[t.arg(0)].(*mutex)
		if !ok {
			t.ret(eSRCH)
			return
		}
		if mu.owner != nil || mu.conds > 0 {
			t.ret(eBUSY)
			return
		}
		delete(m.objects, t.arg(0))
		t.ret(0)
	})
	sysc(102, func(m *Machine, t *Thread) { // lock(id, timeout)
		mu, ok := m.objects[t.arg(0)].(*mutex)
		if !ok {
			t.ret(eSRCH)
			return
		}
		if mu.owner == t {
			if mu.recursive {
				mu.count++
				t.ret(0)
			} else {
				t.ret(eDEADLK)
			}
			return
		}
		t.ret(0)
		m.blockTimeout(t, "mutex "+mu.name, usToTB(t.arg64(1)), func() bool { return mu.tryLock(t) }, func() { t.ret(eTIMEDOUT) })
	})
	sysc(103, func(m *Machine, t *Thread) { // trylock
		mu, ok := m.objects[t.arg(0)].(*mutex)
		if !ok {
			t.ret(eSRCH)
			return
		}
		if mu.owner == t {
			if mu.recursive {
				mu.count++
				t.ret(0)
			} else {
				t.ret(eDEADLK)
			}
			return
		}
		if mu.tryLock(t) {
			t.ret(0)
		} else {
			t.ret(eBUSY)
		}
	})
	sysc(104, func(m *Machine, t *Thread) { // unlock
		mu, ok := m.objects[t.arg(0)].(*mutex)
		if !ok {
			t.ret(eSRCH)
			return
		}
		if mu.owner != t {
			t.ret(ePERM)
			return
		}
		mu.count--
		if mu.count == 0 {
			mu.owner = nil
		}
		t.ret(0)
	})

	// --- condition variable
	sysc(105, func(m *Machine, t *Thread) { // sys_cond_create(id*, mutex, attr*)
		mu, ok := m.objects[t.arg(1)].(*mutex)
		if !ok {
			t.ret(eSRCH)
			return
		}
		mu.conds++
		m.Mem.Write32(t.arg(0), m.newObj(&cond{mu: mu}))
		t.ret(0)
	})
	sysc(106, func(m *Machine, t *Thread) {
		c, ok := m.objects[t.arg(0)].(*cond)
		if !ok {
			t.ret(eSRCH)
			return
		}
		if len(c.waiters) > 0 {
			t.ret(eBUSY)
			return
		}
		c.mu.conds--
		delete(m.objects, t.arg(0))
		t.ret(0)
	})
	sysc(107, func(m *Machine, t *Thread) { // wait(id, timeout)
		c, ok := m.objects[t.arg(0)].(*cond)
		if !ok {
			t.ret(eSRCH)
			return
		}
		if c.mu.owner != t {
			t.ret(ePERM)
			return
		}
		count := c.mu.count
		c.mu.owner, c.mu.count = nil, 0
		w := &condWaiter{t: t}
		c.waiters = append(c.waiters, w)
		t.ret(0)
		reacquire := func() bool {
			if c.mu.tryLock(t) {
				c.mu.count = count
				return true
			}
			return false
		}
		m.blockTimeout(t, "cond", usToTB(t.arg64(1)), func() bool { return w.signaled && reacquire() }, func() {
			c.remove(w)
			t.ret(eTIMEDOUT)
			// still has to get the mutex back
			m.block(t, "cond (relock)", reacquire)
		})
	})
	sysc(108, func(m *Machine, t *Thread) { // signal
		c, ok := m.objects[t.arg(0)].(*cond)
		if !ok {
			t.ret(eSRCH)
			return
		}
		c.signal(1)
		t.ret(0)
	})
	sysc(109, func(m *Machine, t *Thread) { // signal_all
		c, ok := m.objects[t.arg(0)].(*cond)
		if !ok {
			t.ret(eSRCH)
			return
		}
		c.signal(-1)
		t.ret(0)
	})
	sysc(110, func(m *Machine, t *Thread) { // signal_to
		c, ok := m.objects[t.arg(0)].(*cond)
		if !ok {
			t.ret(eSRCH)
			return
		}
		for i, w := range c.waiters {
			if w.t.ID == t.arg(1) {
				w.signaled = true
				c.waiters = append(c.waiters[:i], c.waiters[i+1:]...)
				t.ret(0)
				return
			}
		}
		t.ret(ePERM)
	})

	// --- semaphore
	sysc(90, func(m *Machine, t *Thread) { // sys_semaphore_create(id*, attr*, initial, max)
		s := &semaphore{val: t.argS(2), max: t.argS(3)}
		m.Mem.Write32(t.arg(0), m.newObj(s))
		t.ret(0)
	})
	sysc(91, func(m *Machine, t *Thread) {
		if _, ok := m.objects[t.arg(0)].(*semaphore); !ok {
			t.ret(eSRCH)
			return
		}
		delete(m.objects, t.arg(0))
		t.ret(0)
	})
	sysc(92, func(m *Machine, t *Thread) { // wait(id, timeout)
		s, ok := m.objects[t.arg(0)].(*semaphore)
		if !ok {
			t.ret(eSRCH)
			return
		}
		t.ret(0)
		m.blockTimeout(t, "semaphore", usToTB(t.arg64(1)), func() bool {
			if s.val > 0 {
				s.val--
				return true
			}
			return false
		}, func() { t.ret(eTIMEDOUT) })
	})
	sysc(93, func(m *Machine, t *Thread) { // trywait
		s, ok := m.objects[t.arg(0)].(*semaphore)
		if !ok {
			t.ret(eSRCH)
			return
		}
		if s.val > 0 {
			s.val--
			t.ret(0)
		} else {
			t.ret(eBUSY)
		}
	})
	sysc(94, func(m *Machine, t *Thread) { // post(id, count)
		s, ok := m.objects[t.arg(0)].(*semaphore)
		if !ok {
			t.ret(eSRCH)
			return
		}
		n := t.argS(1)
		if n < 0 || s.val+n > s.max {
			t.ret(eBUSY)
			return
		}
		s.val += n
		t.ret(0)
	})
	sysc(114, func(m *Machine, t *Thread) { // get_value
		s, ok := m.objects[t.arg(0)].(*semaphore)
		if !ok {
			t.ret(eSRCH)
			return
		}
		m.Mem.Write32(t.arg(1), uint32(s.val))
		t.ret(0)
	})

	// --- event flag
	sysc(82, func(m *Machine, t *Thread) { // sys_event_flag_create(id*, attr*, init)
		f := &eventFlag{bits: t.arg64(2)}
		m.Mem.Write32(t.arg(0), m.newObj(f))
		t.ret(0)
	})
	sysc(83, func(m *Machine, t *Thread) {
		delete(m.objects, t.arg(0))
		t.ret(0)
	})
	sysc(85, func(m *Machine, t *Thread) { // wait(id, bits, mode, result*, timeout)
		f, ok := m.objects[t.arg(0)].(*eventFlag)
		if !ok {
			t.ret(eSRCH)
			return
		}
		pat, mode, res := t.arg64(1), t.arg(2), t.arg(3)
		t.ret(0)
		m.blockTimeout(t, "event flag", usToTB(t.arg64(4)), func() bool {
			if !f.match(pat, mode) {
				return false
			}
			if res != 0 {
				m.Mem.Write64(res, f.bits)
			}
			f.clear(pat, mode)
			return true
		}, func() {
			if res != 0 {
				m.Mem.Write64(res, f.bits)
			}
			t.ret(eTIMEDOUT)
		})
	})
	sysc(86, func(m *Machine, t *Thread) { // trywait
		f, ok := m.objects[t.arg(0)].(*eventFlag)
		if !ok {
			t.ret(eSRCH)
			return
		}
		pat, mode, res := t.arg64(1), t.arg(2), t.arg(3)
		if !f.match(pat, mode) {
			t.ret(eBUSY)
			return
		}
		if res != 0 {
			m.Mem.Write64(res, f.bits)
		}
		f.clear(pat, mode)
		t.ret(0)
	})
	sysc(87, func(m *Machine, t *Thread) { // set
		f, ok := m.objects[t.arg(0)].(*eventFlag)
		if !ok {
			t.ret(eSRCH)
			return
		}
		f.bits |= t.arg64(1)
		t.ret(0)
	})
	sysc(118, func(m *Machine, t *Thread) { // clear
		f, ok := m.objects[t.arg(0)].(*eventFlag)
		if !ok {
			t.ret(eSRCH)
			return
		}
		f.bits &= t.arg64(1)
		t.ret(0)
	})
	sysc(139, func(m *Machine, t *Thread) { // get
		f, ok := m.objects[t.arg(0)].(*eventFlag)
		if !ok {
			t.ret(eSRCH)
			return
		}
		m.Mem.Write64(t.arg(1), f.bits)
		t.ret(0)
	})

	// --- event queues and ports
	sysc(128, func(m *Machine, t *Thread) { // sys_event_queue_create(id*, attr*, key, size)
		attr := t.arg(1)
		q := &eventQueue{key: t.arg64(2), size: int(t.arg(3)), typ: m.Mem.Read32(attr + 4), name: m.Mem.String(attr+8, 8)}
		if q.size <= 0 || q.size > 127 {
			q.size = 127
		}
		q.id = m.newObj(q)
		m.Mem.Write32(t.arg(0), q.id)
		t.ret(0)
	})
	sysc(129, func(m *Machine, t *Thread) { // destroy
		delete(m.objects, t.arg(0))
		t.ret(0)
	})
	sysc(130, func(m *Machine, t *Thread) { // receive(id, event*, timeout)
		q, ok := m.objects[t.arg(0)].(*eventQueue)
		if !ok {
			t.ret(eSRCH)
			return
		}
		m.blockTimeout(t, "event queue "+q.name, usToTB(t.arg64(2)), func() bool {
			if _, ok := m.objects[q.id]; !ok {
				t.ret(eCANCELED)
				return true
			}
			if len(q.events) == 0 {
				return false
			}
			e := q.events[0]
			q.events = q.events[1:]
			t.GPR[3] = 0
			t.GPR[4], t.GPR[5], t.GPR[6], t.GPR[7] = e.source, e.d1, e.d2, e.d3
			return true
		}, func() { t.ret(eTIMEDOUT) })
	})
	sysc(131, func(m *Machine, t *Thread) { // tryreceive(id, event_array*, size, number*)
		q, ok := m.objects[t.arg(0)].(*eventQueue)
		if !ok {
			t.ret(eSRCH)
			return
		}
		arr, size := t.arg(1), int(t.arg(2))
		n := 0
		for n < size && len(q.events) > 0 {
			e := q.events[0]
			q.events = q.events[1:]
			a := arr + uint32(n)*32
			m.Mem.Write64(a, e.source)
			m.Mem.Write64(a+8, e.d1)
			m.Mem.Write64(a+16, e.d2)
			m.Mem.Write64(a+24, e.d3)
			n++
		}
		m.Mem.Write32(t.arg(3), uint32(n))
		t.ret(0)
	})
	sysc(133, func(m *Machine, t *Thread) { // drain
		if q, ok := m.objects[t.arg(0)].(*eventQueue); ok {
			q.events = nil
		}
		t.ret(0)
	})
	sysc(134, func(m *Machine, t *Thread) { // sys_event_port_create(id*, type, name)
		p := &eventPort{name: t.arg64(2)}
		id := m.newObj(p)
		if p.name == 0 {
			p.name = 1<<32 | uint64(id)
		}
		m.Mem.Write32(t.arg(0), id)
		t.ret(0)
	})
	sysc(135, func(m *Machine, t *Thread) {
		delete(m.objects, t.arg(0))
		t.ret(0)
	})
	sysc(136, func(m *Machine, t *Thread) { // connect_local(port, queue)
		p, ok := m.objects[t.arg(0)].(*eventPort)
		q, ok2 := m.objects[t.arg(1)].(*eventQueue)
		if !ok || !ok2 {
			t.ret(eSRCH)
			return
		}
		p.queue = q
		t.ret(0)
	})
	sysc(137, func(m *Machine, t *Thread) { // disconnect
		if p, ok := m.objects[t.arg(0)].(*eventPort); ok {
			p.queue = nil
		}
		t.ret(0)
	})
	sysc(138, func(m *Machine, t *Thread) { // send(port, d1, d2, d3)
		p, ok := m.objects[t.arg(0)].(*eventPort)
		if !ok {
			t.ret(eSRCH)
			return
		}
		if p.queue == nil {
			t.ret(eNOTCONN)
			return
		}
		t.ret(p.queue.push(event{p.name, t.arg64(1), t.arg64(2), t.arg64(3)}))
	})
}

const eNOTCONN = 0x80010017

func (q *eventQueue) push(e event) uint32 {
	if len(q.events) >= q.size {
		return eBUSY
	}
	q.events = append(q.events, e)
	return 0
}

func (c *cond) signal(n int) {
	for n != 0 && len(c.waiters) > 0 {
		c.waiters[0].signaled = true
		c.waiters = c.waiters[1:]
		n--
	}
}

func (c *cond) remove(w *condWaiter) {
	for i, x := range c.waiters {
		if x == w {
			c.waiters = append(c.waiters[:i], c.waiters[i+1:]...)
			return
		}
	}
}

func (f *eventFlag) match(pat uint64, mode uint32) bool {
	if mode&0xf == 2 { // OR
		return f.bits&pat != 0
	}
	return f.bits&pat == pat // AND
}

func (f *eventFlag) clear(pat uint64, mode uint32) {
	switch mode & 0xf0 {
	case 0x10:
		f.bits &^= pat
	case 0x20:
		f.bits = 0
	}
}

// ---- lightweight mutexes and condition variables (sysPrxForUser)

const (
	lwFree = 0xFFFFFFFF
	lwDead = 0xFFFFFFFE
)

type lwcondWaiter struct {
	t        *Thread
	signaled bool
}

func (m *Machine) lwTryLock(t *Thread, mu uint32) bool {
	if m.Mem.Read32(mu) == lwFree {
		m.Mem.Write32(mu, t.ID)
		m.Mem.Write32(mu+12, 1)
		return true
	}
	return false
}

func init() {
	reg("sys_lwmutex_create", func(m *Machine, t *Thread) { // (lwmutex*, attr*)
		mu, attr := t.arg(0), t.arg(1)
		proto := m.Mem.Read32(attr)
		rec := m.Mem.Read32(attr + 4)
		m.Mem.Write32(mu, lwFree)
		m.Mem.Write32(mu+4, 0)
		m.Mem.Write32(mu+8, proto|rec)
		m.Mem.Write32(mu+12, 0)
		m.Mem.Write32(mu+16, m.newObj(&mutex{name: m.Mem.String(attr+8, 8)}))
		m.Mem.Write32(mu+20, 0)
		t.ret(0)
	})
	reg("sys_lwmutex_destroy", func(m *Machine, t *Thread) {
		mu := t.arg(0)
		if o := m.Mem.Read32(mu); o != lwFree {
			if o == lwDead {
				t.ret(eINVAL)
			} else {
				t.ret(eBUSY)
			}
			return
		}
		m.Mem.Write32(mu, lwDead)
		t.ret(0)
	})
	lock := func(m *Machine, t *Thread, try bool) {
		mu := t.arg(0)
		o := m.Mem.Read32(mu)
		if o == t.ID {
			if m.Mem.Read32(mu+8)&0x10 != 0 {
				m.Mem.Write32(mu+12, m.Mem.Read32(mu+12)+1)
				t.ret(0)
			} else {
				t.ret(eDEADLK)
			}
			return
		}
		if o == lwDead {
			t.ret(eINVAL)
			return
		}
		if _, ok := m.objects[m.Mem.Read32(mu+16)].(*mutex); !ok && o != lwFree {
			// never created: the kernel doesn't know its sleep queue
			t.ret(eSRCH)
			return
		}
		if try {
			if m.lwTryLock(t, mu) {
				t.ret(0)
			} else {
				t.ret(eBUSY)
			}
			return
		}
		t.ret(0)
		m.blockTimeout(t, fmt.Sprintf("lwmutex %08X (owner %x)", mu, o), usToTB(t.arg64(1)), func() bool { return m.lwTryLock(t, mu) }, func() { t.ret(eTIMEDOUT) })
	}
	reg("sys_lwmutex_lock", func(m *Machine, t *Thread) { lock(m, t, false) })
	reg("sys_lwmutex_trylock", func(m *Machine, t *Thread) { lock(m, t, true) })
	reg("sys_lwmutex_unlock", func(m *Machine, t *Thread) {
		mu := t.arg(0)
		if m.Mem.Read32(mu) != t.ID {
			t.ret(ePERM)
			return
		}
		n := m.Mem.Read32(mu+12) - 1
		m.Mem.Write32(mu+12, n)
		if n == 0 {
			m.Mem.Write32(mu, lwFree)
		}
		t.ret(0)
	})

	reg("sys_lwcond_create", func(m *Machine, t *Thread) { // (lwcond*, lwmutex*, attr*)
		lc := t.arg(0)
		m.Mem.Write32(lc, t.arg(1))
		m.Mem.Write32(lc+4, m.newObj(&[]*lwcondWaiter{}))
		t.ret(0)
	})
	reg("sys_lwcond_destroy", func(m *Machine, t *Thread) {
		id := m.Mem.Read32(t.arg(0) + 4)
		if w, ok := m.objects[id].(*[]*lwcondWaiter); ok && len(*w) > 0 {
			t.ret(eBUSY)
			return
		}
		delete(m.objects, id)
		t.ret(0)
	})
	reg("sys_lwcond_wait", func(m *Machine, t *Thread) { // (lwcond*, timeout)
		lc := t.arg(0)
		mu := m.Mem.Read32(lc)
		ws, ok := m.objects[m.Mem.Read32(lc+4)].(*[]*lwcondWaiter)
		if !ok {
			t.ret(eSRCH)
			return
		}
		if m.Mem.Read32(mu) != t.ID {
			t.ret(ePERM)
			return
		}
		count := m.Mem.Read32(mu + 12)
		m.Mem.Write32(mu, lwFree)
		m.Mem.Write32(mu+12, 0)
		w := &lwcondWaiter{t: t}
		*ws = append(*ws, w)
		reacquire := func() bool {
			if m.lwTryLock(t, mu) {
				m.Mem.Write32(mu+12, count)
				return true
			}
			return false
		}
		t.ret(0)
		m.blockTimeout(t, "lwcond", usToTB(t.arg64(1)), func() bool { return w.signaled && reacquire() }, func() {
			for i, x := range *ws {
				if x == w {
					*ws = append((*ws)[:i], (*ws)[i+1:]...)
					break
				}
			}
			t.ret(eTIMEDOUT)
			m.block(t, "lwcond (relock)", reacquire)
		})
	})
	signal := func(m *Machine, t *Thread, n int, to uint32) {
		ws, ok := m.objects[m.Mem.Read32(t.arg(0)+4)].(*[]*lwcondWaiter)
		if !ok {
			t.ret(eSRCH)
			return
		}
		for i := 0; i < len(*ws) && n != 0; {
			w := (*ws)[i]
			if to != 0 && w.t.ID != to {
				i++
				continue
			}
			w.signaled = true
			*ws = append((*ws)[:i], (*ws)[i+1:]...)
			n--
		}
		t.ret(0)
	}
	reg("sys_lwcond_signal", func(m *Machine, t *Thread) { signal(m, t, 1, 0) })
	reg("sys_lwcond_signal_all", func(m *Machine, t *Thread) { signal(m, t, -1, 0) })
	reg("sys_lwcond_signal_to", func(m *Machine, t *Thread) { signal(m, t, 1, t.arg(1)) })

	// spinlocks: a word, 0 when free
	reg("sys_spinlock_initialize", func(m *Machine, t *Thread) { m.Mem.Write32(t.arg(0), 0) })
	reg("sys_spinlock_lock", func(m *Machine, t *Thread) {
		a := t.arg(0)
		m.block(t, "spinlock", func() bool {
			if m.Mem.Read32(a) == 0 {
				m.Mem.Write32(a, 0xabadcafe)
				return true
			}
			return false
		})
	})
	reg("sys_spinlock_trylock", func(m *Machine, t *Thread) {
		a := t.arg(0)
		if m.Mem.Read32(a) == 0 {
			m.Mem.Write32(a, 0xabadcafe)
			t.ret(0)
		} else {
			t.ret(eBUSY)
		}
	})
	reg("sys_spinlock_unlock", func(m *Machine, t *Thread) { m.Mem.Write32(t.arg(0), 0) })
}
