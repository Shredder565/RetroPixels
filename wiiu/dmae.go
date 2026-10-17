package wiiu

// DMAE, the DMA engine: copies and fills a program starts and waits on by
// timestamp. Here they happen at once and are done by the time they're
// asked about.

func init() {
	d := "dmae"
	now := func(m *Machine) uint64 { return m.bootTime + m.Time() }
	// (dst, src, words, endian) -> timestamp
	reg(d, "DMAECopyMem", func(m *Machine, cpu *CPU, t *Thread) {
		dst, src, n, endian := arg(cpu, 0), arg(cpu, 1), arg(cpu, 2), arg(cpu, 3)
		buf := make([]byte, n*4)
		m.Mem.ReadBytes(src, buf)
		for i := 0; i+3 < len(buf); i += 4 {
			switch endian {
			case 1: // 8 in 16
				buf[i], buf[i+1], buf[i+2], buf[i+3] = buf[i+1], buf[i], buf[i+3], buf[i+2]
			case 2: // 8 in 32
				buf[i], buf[i+1], buf[i+2], buf[i+3] = buf[i+3], buf[i+2], buf[i+1], buf[i]
			}
		}
		m.Mem.WriteBytes(dst, buf)
		m.Mem.InvalidateCode(dst, n*4)
		m.gpuWritten(dst, n*4)
		ret64(cpu, now(m))
	})
	// (dst, value, words) -> timestamp
	reg(d, "DMAEFillMem", func(m *Machine, cpu *CPU, t *Thread) {
		dst, v, n := arg(cpu, 0), arg(cpu, 1), arg(cpu, 2)
		for i := uint32(0); i < n; i++ {
			m.Mem.Write32(dst+i*4, v)
		}
		m.gpuWritten(dst, n*4)
		ret64(cpu, now(m))
	})
	reg(d, "DMAEWaitDone", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 1) })
	reg(d, "DMAEGetRetiredTimeStamp", func(m *Machine, cpu *CPU, t *Thread) { ret64(cpu, now(m)) })
	reg(d, "DMAEGetLastSubmittedTimeStamp", func(m *Machine, cpu *CPU, t *Thread) { ret64(cpu, now(m)) })
	// (u64 *semaphore, op): 0 signals (adds one), 1 waits (takes one)
	reg(d, "DMAESemaphore", func(m *Machine, cpu *CPU, t *Thread) {
		p, op := arg(cpu, 0), arg(cpu, 1)
		if p != 0 {
			v := m.Mem.Read64(p)
			if op == 0 {
				v++
			} else if v > 0 {
				v--
			}
			m.Mem.Write64(p, v)
		}
		ret64(cpu, now(m))
	})
	reg(d, "DMAEInit", func(m *Machine, cpu *CPU, t *Thread) {})
	reg(d, "DMAEGetTimeout", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 10000) })
	reg(d, "DMAESetTimeout", func(m *Machine, cpu *CPU, t *Thread) {})
}
