package wiiu

// GX2 calls that wait for or report on the GPU's progress. The GPU here
// finishes each command as it's sent, so queries are ready at once and
// waits are over before they start.

func init() {
	x := "gx2"
	// occlusion queries: (query, type) begin and end; the result is how
	// many samples passed. Without counting, everything is said to be
	// visible, which draws too much rather than too little.
	reg(x, "GX2QueryBegin", func(m *Machine, cpu *CPU, t *Thread) {})
	reg(x, "GX2QueryEnd", func(m *Machine, cpu *CPU, t *Thread) {})
	reg(x, "GX2QueryBeginConditionalRender", func(m *Machine, cpu *CPU, t *Thread) {})
	reg(x, "GX2QueryEndConditionalRender", func(m *Machine, cpu *CPU, t *Thread) {})
	// (query, u64 *result) -> ready
	reg(x, "GX2QueryGetOcclusionResult", func(m *Machine, cpu *CPU, t *Thread) {
		if p := arg(cpu, 1); p != 0 {
			m.Mem.Write64(p, 0x10000)
		}
		ret(cpu, 1)
	})
	// (u64 *semaphore, action): the GPU waits on it (0) or signals it (1)
	reg(x, "GX2SetSemaphore", func(m *Machine, cpu *CPU, t *Thread) {
		if p := arg(cpu, 0); p != 0 && arg(cpu, 1) == 1 {
			m.Mem.Write64(p, m.Mem.Read64(p)+1)
		}
	})
	// the GPU waits until memory matches: it already does
	reg(x, "GX2SetGPUFence", func(m *Machine, cpu *CPU, t *Thread) {})
	reg(x, "GX2DrawdoneSync", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 1) })
	reg(x, "GX2WaitForFreeScanBuffer", func(m *Machine, cpu *CPU, t *Thread) {})
	reg(x, "GX2MarkScanBufferCopied", func(m *Machine, cpu *CPU, t *Thread) {})
	reg(x, "GX2SubmitUserTimeStamp", func(m *Machine, cpu *CPU, t *Thread) {
		// (u64 *where, u64 value, type, interrupt): the GPU writes value there
		if p := arg(cpu, 0); p != 0 {
			m.Mem.Write64(p, arg64(cpu, 2))
		}
		if arg(cpu, 5) != 0 {
			m.gx2Event(1) // the end-of-pipe interrupt
		}
	})
	// (surface, level): one mip level's pitch in elements, and the bytes in one slice of it
	mipLevel := func(m *Machine, cpu *CPU) (surfLevel, bool) {
		s := m.readSurface(arg(cpu, 0))
		info := s.info()
		l := int(arg(cpu, 1))
		if l >= len(info.Levels) {
			return surfLevel{}, false
		}
		return info.Levels[l], true
	}
	reg(x, "GX2GetSurfaceMipPitch", func(m *Machine, cpu *CPU, t *Thread) {
		l, _ := mipLevel(m, cpu)
		ret(cpu, l.Pitch)
	})
	reg(x, "GX2GetSurfaceMipSliceSize", func(m *Machine, cpu *CPU, t *Thread) {
		l, _ := mipLevel(m, cpu)
		sf := m.readSurface(arg(cpu, 0))
		slices := sf.d
		if sf.dim == 2 { // 3D: the depth halves each level too
			slices = max(1, sf.d>>arg(cpu, 1))
		}
		ret(cpu, l.Size/slices)
	})
	// the GPU's clock and the CPU's run at the same rate here
	reg(x, "GX2GPUTimeToCPUTime", func(m *Machine, cpu *CPU, t *Thread) { ret64(cpu, arg64(cpu, 0)) })
	reg(x, "GX2SurfaceIsCompressed", func(m *Machine, cpu *CPU, t *Thread) {
		retBool(cpu, formatCompressed(arg(cpu, 0)&0x3f))
	})
	reg(x, "GX2GetSurfaceFormatBits", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, formatBits(arg(cpu, 0)&0x3f)) })
	reg(x, "GX2ResetGPU", func(m *Machine, cpu *CPU, t *Thread) {})
}
