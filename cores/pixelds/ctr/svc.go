package ctr

// The supervisor calls (SVCs) an application makes.

import (
	"sort"

	"retropixels/cores/pixelds/arm"
)

func (k *kernel) svc(n uint32) {
	c := k.cpu
	r := &c.R
	switch n {
	case 0x01: // ControlMemory(op, addr0, addr1, size, perm) -> addr
		addr, res := k.controlMemory(r[0], r[1], r[2], r[3], r[4])
		r[0], r[1] = res, addr
	case 0x02: // QueryMemory(addr) -> base, size, perm, state, page flags
		k.queryMemory(r[2])
	case 0x03: // ExitProcess
		k.m.exited = true
		for _, t := range k.threads {
			t.state = thDead
		}
		k.cur = nil
		c.Halted = true
	case 0x08: // CreateThread(prio, entry, arg, stacktop, core) -> handle
		t, err := k.newThread(r[1], r[2], r[3], int32(r[0]))
		if err != nil {
			r[0] = rcOutOfMemory
			return
		}
		r[0], r[1] = rcOK, k.add(t)
		k.reschedule(false)
	case 0x09: // ExitThread
		k.cur.state = thDead
		k.tlsUsed[k.cur.tlsIdx] = false
		k.cur = nil
		k.update()
		if k.cur == nil {
			c.Halted = true
		}
	case 0x0a: // SleepThread(ns)
		ns := int64(uint64(r[0]) | uint64(r[1])<<32)
		r[0] = rcOK
		if ns <= 0 {
			k.reschedule(true)
			return
		}
		k.cur.sleeping = true
		k.block(ns)
	case 0x0b: // GetThreadPriority(handle) -> prio
		if t, ok := k.get(r[1]).(*thread); ok {
			r[0], r[1] = rcOK, uint32(t.prio)
		} else {
			r[0] = rcInvalidHandle
		}
	case 0x0c: // SetThreadPriority(handle, prio)
		if t, ok := k.get(r[0]).(*thread); ok {
			t.prio = int32(r[1])
			r[0] = rcOK
			k.reschedule(false)
		} else {
			r[0] = rcInvalidHandle
		}
	case 0x13: // CreateMutex(locked) -> handle
		x := &mutex{}
		if r[1] != 0 {
			x.owner, x.count = k.cur, 1
		}
		r[0], r[1] = rcOK, k.add(x)
	case 0x14: // ReleaseMutex(handle)
		x, ok := k.get(r[0]).(*mutex)
		if !ok {
			r[0] = rcInvalidHandle
			return
		}
		r[0] = rcOK
		if x.owner == k.cur {
			x.count--
			if x.count <= 0 {
				x.owner, x.count = nil, 0
			}
		}
		k.update()
	case 0x15: // CreateSemaphore(initial, max) -> handle
		r[0], r[1] = rcOK, k.add(&semaphore{count: int32(r[1]), max: int32(r[2])})
	case 0x16: // ReleaseSemaphore(handle, count) -> previous count
		s, ok := k.get(r[1]).(*semaphore)
		if !ok {
			r[0] = rcInvalidHandle
			return
		}
		prev := s.count
		s.count += int32(r[2])
		if s.count > s.max && s.max > 0 {
			s.count = s.max
		}
		r[0], r[1] = rcOK, uint32(prev)
		k.update()
	case 0x17: // CreateEvent(reset type) -> handle
		r[0], r[1] = rcOK, k.add(&event{reset: r[1]})
	case 0x18: // SignalEvent(handle)
		switch e := k.get(r[0]).(type) {
		case *event:
			r[0] = rcOK
			k.signal(e)
		case *timer:
			r[0] = rcOK
			k.signal(&e.event)
		default:
			r[0] = rcInvalidHandle
		}
	case 0x19: // ClearEvent(handle)
		switch e := k.get(r[0]).(type) {
		case *event:
			e.set = false
			r[0] = rcOK
		default:
			r[0] = rcInvalidHandle
		}
	case 0x1a: // CreateTimer(reset type) -> handle
		t := &timer{event: event{reset: r[1]}, at: -1}
		k.timers = append(k.timers, t)
		r[0], r[1] = rcOK, k.add(t)
	case 0x1b: // SetTimer(handle, initial ns, interval ns)
		t, ok := k.get(r[0]).(*timer)
		if !ok {
			r[0] = rcInvalidHandle
			return
		}
		initial := int64(uint64(r[2]) | uint64(r[3])<<32)
		interval := int64(uint64(r[1]) | uint64(r[4])<<32)
		t.at = k.now() + nsToTicks(initial)
		t.interval = nsToTicks(interval)
		r[0] = rcOK
	case 0x1c: // CancelTimer(handle)
		if t, ok := k.get(r[0]).(*timer); ok {
			t.at = -1
			r[0] = rcOK
		} else {
			r[0] = rcInvalidHandle
		}
	case 0x1d: // ClearTimer(handle)
		if t, ok := k.get(r[0]).(*timer); ok {
			t.set = false
			r[0] = rcOK
		} else {
			r[0] = rcInvalidHandle
		}
	case 0x1e: // CreateMemoryBlock(addr, size, myperm, otherperm) -> handle
		k.createMemoryBlock(r[1], r[2])
	case 0x1f: // MapMemoryBlock(handle, addr, myperm, otherperm)
		b, ok := k.get(r[0]).(*memBlock)
		if !ok {
			r[0] = rcInvalidHandle
			return
		}
		addr := r[1]
		if addr == 0 {
			addr = k.freeRegion(0x10000000, 0x14000000, b.size)
		}
		for i, pa := range b.pages {
			k.mem.mapPhys(addr+uint32(i)*pageSize, pa, pageSize)
		}
		r[0] = rcOK
	case 0x20: // UnmapMemoryBlock(handle, addr)
		if b, ok := k.get(r[0]).(*memBlock); ok {
			k.mem.unmap(r[1], b.size)
			r[0] = rcOK
		} else {
			r[0] = rcInvalidHandle
		}
	case 0x21: // CreateAddressArbiter -> handle
		r[0], r[1] = rcOK, k.add(&arbiter{})
	case 0x22: // ArbitrateAddress(handle, addr, type, value, timeout)
		k.arbitrate(r[1], r[2], int32(r[3]), int64(uint64(r[4])|uint64(r[5])<<32))
	case 0x23: // CloseHandle
		delete(k.handles, r[0])
		r[0] = rcOK
	case 0x24: // WaitSynchronization1(handle, timeout)
		o, ok := k.get(r[0]).(waitable)
		if !ok {
			r[0] = rcInvalidHandle
			return
		}
		k.wait([]waitable{o}, false, int64(uint64(r[2])|uint64(r[3])<<32), false)
	case 0x25: // WaitSynchronizationN(out, handles, count, all, timeout)
		n := r[2]
		var objs []waitable
		for i := uint32(0); i < n && i < 64; i++ {
			o, ok := k.get(k.mem.Read32(r[1] + 4*i)).(waitable)
			if !ok {
				r[0] = rcInvalidHandle
				return
			}
			objs = append(objs, o)
		}
		timeout := int64(uint64(r[0]) | uint64(r[4])<<32)
		k.wait(objs, r[3] != 0, timeout, true)
	case 0x27: // DuplicateHandle(handle) -> new handle
		o := k.get(r[1])
		if o == nil {
			r[0] = rcInvalidHandle
			return
		}
		r[0], r[1] = rcOK, k.add(o)
	case 0x28: // GetSystemTick
		t := uint64(k.now())
		r[0], r[1] = uint32(t), uint32(t>>32)
	case 0x2a: // GetSystemInfo(type, param) -> value
		var v uint64
		switch r[1] {
		case 0: // memory used in a region
			v = uint64(k.mem.usedBytes())
		case 26: // processes started by the kernel
			v = 5
		}
		r[0], r[1], r[2] = rcOK, uint32(v), uint32(v>>32)
	case 0x2b: // GetProcessInfo(handle, type) -> value
		var v uint64
		switch r[2] {
		case 0, 2: // memory used
			v = uint64(k.mem.usedBytes())
		case 20: // linear heap: physical minus virtual
			v = fcramBase - linearBase
		}
		r[0], r[1], r[2] = rcOK, uint32(v), uint32(v>>32)
	case 0x2d: // ConnectToPort(name) -> session
		name := k.mem.readString(r[1], 12)
		svc := k.m.svcs.port(name)
		if svc == nil {
			r[0] = rcNotFound
			return
		}
		r[0], r[1] = rcOK, k.add(&session{svc: svc})
	case 0x32: // SendSyncRequest(session)
		s, ok := k.get(r[0]).(*session)
		if !ok {
			r[0] = rcInvalidHandle
			return
		}
		r[0] = rcOK
		k.m.svcs.call(s.svc, k.cur.tls+0x80)
	case 0x35: // GetProcessId(handle) -> id
		r[0], r[1] = rcOK, 0x28
	case 0x37: // GetThreadId(handle) -> id
		if t, ok := k.get(r[1]).(*thread); ok {
			r[0], r[1] = rcOK, t.id
		} else {
			r[0] = rcInvalidHandle
		}
	case 0x38: // GetResourceLimit(process) -> handle
		r[0], r[1] = rcOK, k.add(&resourceLimit{})
	case 0x39, 0x3a: // GetResourceLimitLimitValues / CurrentValues(out, limit, names, count)
		for i := uint32(0); i < r[3]; i++ {
			name := k.mem.Read32(r[2] + 4*i)
			var v uint64
			switch name {
			case 1: // memory
				if n == 0x39 {
					v = appMemSize
				} else {
					v = uint64(k.mem.usedBytes())
				}
			case 2: // threads
				v = 32
			case 0: // priority
				v = 0x18
			}
			k.mem.Write32(r[0]+8*i, uint32(v))
			k.mem.Write32(r[0]+8*i+4, uint32(v>>32))
		}
		r[0] = rcOK
	case 0x3c: // Break
		k.m.fail("the program stopped itself (svcBreak, reason %d)", r[0])
	case 0x3d: // OutputDebugString(str, len)
		k.log("debug: %s", string(k.mem.readBytes(r[0], min(r[1], 512))))
		r[0] = rcOK
	case 0x7c: // KernelSetState
		r[0] = rcOK
	case 0x0d, 0x0f: // GetThreadAffinityMask, GetThreadIdealProcessor
		r[0], r[1] = rcOK, 1
	case 0x0e, 0x10: // SetThreadAffinityMask, SetThreadIdealProcessor
		r[0] = rcOK
	case 0x11: // GetCurrentProcessorNumber
		r[0] = 0
	case 0x29: // GetHandleInfo(handle, type) -> value
		r[0], r[1], r[2] = rcOK, 1, 0
	case 0x36: // GetProcessIdOfThread(thread) -> process id
		r[0], r[1] = rcOK, 0x28
	case 0x52, 0x53, 0x54: // Invalidate / Store / FlushProcessDataCache
		r[0] = rcOK
	case 0x7d: // QueryProcessMemory(process, addr)
		k.queryMemory(r[3])
	case 0x26: // SignalAndWait(timeout low, signal, handles, count, all, timeout high)
		switch e := k.get(r[1]).(type) {
		case *timer:
			e.set = true
		case *event:
			e.set = true
		case *semaphore:
			if e.count < e.max {
				e.count++
			}
		case *mutex:
			if e.owner == k.cur && e.count > 0 {
				e.count--
				if e.count == 0 {
					e.owner = nil
				}
			}
		}
		n := r[3]
		var objs []waitable
		for i := uint32(0); i < n && i < 64; i++ {
			o, ok := k.get(k.mem.Read32(r[2] + 4*i)).(waitable)
			if !ok {
				r[0] = rcInvalidHandle
				return
			}
			objs = append(objs, o)
		}
		k.updateNoSwitch()
		k.wait(objs, r[4] != 0, int64(uint64(r[0])|uint64(r[5])<<32), true)
	default:
		k.log("SVC %02x not done (pc %08x)", n, c.PC())
		r[0] = rcNotImpl
	}
}

func (k *kernel) wait(objs []waitable, all bool, timeout int64, out bool) {
	t := k.cur
	t.waitObjs, t.waitAll, t.waitOut = objs, all, out
	if ok, i := k.tryWait(t); ok {
		k.wake(t, rcOK, i)
		k.cpu.R[0] = rcOK
		if out {
			k.cpu.R[1] = uint32(i)
		}
		return
	}
	if timeout == 0 {
		t.waitObjs = nil
		t.waitOut = false
		k.cpu.R[0] = rcTimeout
		return
	}
	k.block(timeout)
}

func (k *kernel) arbitrate(addr, typ uint32, value int32, timeout int64) {
	r := &k.cpu.R
	r[0] = rcOK
	switch typ {
	case 0: // signal: wake up to value threads (all when negative)
		var ws []*thread
		for _, t := range k.threads {
			if t.state == thWaiting && t.onArb && t.arbAddr == addr {
				ws = append(ws, t)
			}
		}
		sort.SliceStable(ws, func(i, j int) bool { return ws[i].prio < ws[j].prio })
		for i, t := range ws {
			if value >= 0 && int32(i) >= value {
				break
			}
			k.wake(t, rcOK, 0)
		}
		k.reschedule(false)
	case 1, 2, 3, 4: // wait if *addr < value (decrementing first for 2 and 4)
		v := int32(k.mem.Read32(addr))
		if typ == 2 || typ == 4 {
			if v < value {
				k.mem.Write32(addr, uint32(v-1))
			}
		}
		if v >= value {
			return
		}
		t := k.cur
		t.onArb, t.arbAddr = true, addr
		if typ == 3 || typ == 4 {
			k.block(timeout)
			if timeout >= 0 {
				// a timed-out arbitration returns timeout
			}
			return
		}
		k.block(-1)
	default:
		r[0] = rcInvalidCombo
	}
}

func (k *kernel) controlMemory(op, addr0, addr1, size, perm uint32) (uint32, uint32) {
	size = (size + pageSize - 1) &^ (pageSize - 1)
	switch op & 0xff {
	case 3: // allocate
		if op&0x10000 != 0 { // linear
			pa, err := k.mem.allocLinear(size)
			if err != nil {
				return 0, rcOutOfMemory
			}
			va := pa - fcramBase + linearBase
			k.mem.mapPhys(va, pa, size)
			return va, rcOK
		}
		if addr0 == 0 {
			addr0 = k.freeRegion(heapBase, heapBase+0x6000000, size)
		}
		pages, err := k.mem.allocPages(size / pageSize)
		if err != nil {
			return 0, rcOutOfMemory
		}
		for i, pa := range pages {
			k.mem.mapPhys(addr0+uint32(i)*pageSize, pa, pageSize)
		}
		return addr0, rcOK
	case 1: // free
		for off := uint32(0); off < size; off += pageSize {
			if pa := k.mem.virtToPhys(addr0 + off); pa != 0 {
				k.mem.freePage(pa)
			}
		}
		k.mem.unmap(addr0, size)
		return addr0, rcOK
	case 4: // map addr1's memory at addr0 too
		for off := uint32(0); off < size; off += pageSize {
			pa := k.mem.virtToPhys(addr1 + off)
			if pa == 0 {
				return 0, rcInvalidAddr
			}
			k.mem.mapPhys(addr0+off, pa, pageSize)
		}
		return addr0, rcOK
	case 5: // unmap the alias
		k.mem.unmap(addr0, size)
		return addr0, rcOK
	case 6: // protect
		return addr0, rcOK
	}
	return 0, rcInvalidCombo
}

// freeRegion finds size bytes of unmapped address space in [lo, hi).
func (k *kernel) freeRegion(lo, hi, size uint32) uint32 {
	for a := lo; a+size <= hi; a += pageSize {
		if k.mem.pages[a>>pageBits] != nil {
			continue
		}
		ok := true
		for off := uint32(0); off < size; off += pageSize {
			if k.mem.pages[(a+off)>>pageBits] != nil {
				ok = false
				a += off
				break
			}
		}
		if ok {
			return a
		}
	}
	return 0
}

func (k *kernel) queryMemory(addr uint32) {
	r := &k.cpu.R
	page := addr &^ (pageSize - 1)
	mapped := k.mem.pages[page>>pageBits] != nil
	base := page
	for base > 0 && (k.mem.pages[(base-pageSize)>>pageBits] != nil) == mapped {
		base -= pageSize
	}
	end := page
	for end < 0xfffff000 && (k.mem.pages[end>>pageBits] != nil) == mapped {
		end += pageSize
	}
	perm, state := uint32(0), uint32(0)
	if mapped {
		perm, state = 3, 5 // read and write, private
		if base < 0x01000000 {
			perm, state = 5, 4 // code: read and execute
		}
	}
	r[0], r[1], r[2], r[3], r[4], r[5] = rcOK, base, end-base, perm, state, 0
}

func (k *kernel) createMemoryBlock(addr, size uint32) {
	r := &k.cpu.R
	size = (size + pageSize - 1) &^ (pageSize - 1)
	b := &memBlock{size: size, label: "app"}
	if addr == 0 {
		pa, err := k.mem.allocLinear(size)
		if err != nil {
			r[0] = rcOutOfMemory
			return
		}
		for off := uint32(0); off < size; off += pageSize {
			b.pages = append(b.pages, pa+off)
		}
	} else {
		for off := uint32(0); off < size; off += pageSize {
			pa := k.mem.virtToPhys(addr + off)
			if pa == 0 {
				r[0] = rcInvalidAddr
				return
			}
			b.pages = append(b.pages, pa)
		}
	}
	r[0], r[1] = rcOK, k.add(b)
}

// a block of memory made by a service (shared memory with the
// application)
func (k *kernel) serviceBlock(size uint32, label string) (*memBlock, uint32) {
	size = (size + pageSize - 1) &^ (pageSize - 1)
	pages, _ := k.mem.allocPages(size / pageSize)
	b := &memBlock{pages: pages, size: size, label: label}
	return b, k.add(b)
}

var _ = arm.ModeUSR
