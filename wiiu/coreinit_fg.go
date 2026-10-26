// Original code for RetroPixels. Not derived from Cemu.

package wiiu

// The foreground bucket is a 64 MB region the foreground program shares
// with the system. The program may use the first 40 MB freely; the rest is
// divided into areas the OS keeps data in (clipboard, title switching
// parameters, transition audio and frames, a small save area).

const (
	fgAreaFree = 0
	fgAreaSave = 6

	fgFreeSize = 40 << 20
)

// fgLayout lists the bucket's areas in address order.
var fgLayout = []struct{ id, offset, size uint32 }{
	{0, 0x0000000, 0x2800000},
	{7, 0x2800000, 0x0400000},
	{1, 0x2c00000, 0x0900000},
	{2, 0x3500000, 0x03c0000},
	{3, 0x38c0000, 0x01c0000},
	{4, 0x3a80000, 0x03c0000},
	{5, 0x3e40000, 0x01bf000},
	{6, 0x3fff000, 0x0001000},
}

// fgArea is the address of area id, or 0.
func fgArea(id uint32) uint32 {
	for _, a := range fgLayout {
		if a.id == id {
			return FGBase + a.offset
		}
	}
	return 0
}

// fgFreeArea is the part of the bucket the program may use.
func (m *Machine) fgFreeArea() (addr, size uint32) { return fgArea(fgAreaFree), fgFreeSize }

// initForegroundBucket stamps the save area as initialised; the rest of the
// bucket starts out zero like any fresh memory.
func (m *Machine) initForegroundBucket() {
	save := fgArea(fgAreaSave)
	m.Mem.Write32(save, 0x53617665) // "Save"
	m.Mem.Write32(save+8, m.Mem.Read32(save+8)|0x300)
}

func init() {
	const c = "coreinit"
	// writeBoth stores addr and size through the two pointer arguments.
	writeBoth := func(m *Machine, cpu *CPU, addr, size uint32) {
		for i, v := range [2]uint32{addr, size} {
			if p := arg(cpu, i); p != 0 {
				m.Mem.Write32(p, v)
			}
		}
	}
	// OSGetForegroundBucket(u32 *addr, u32 *size)
	reg(c, "OSGetForegroundBucket", func(m *Machine, cpu *CPU, t *Thread) {
		writeBoth(m, cpu, FGBase, FGSize)
		ret(cpu, 1)
	})
	// OSGetForegroundBucketFreeArea(u32 *addr, u32 *size)
	reg(c, "OSGetForegroundBucketFreeArea", func(m *Machine, cpu *CPU, t *Thread) {
		a, n := m.fgFreeArea()
		writeBoth(m, cpu, a, n)
		ret(cpu, 1)
	})
	// OSCopyFromClipboard(buf, u32 *size): the clipboard is always empty.
	// A call that only asks for the size (no buffer, *size 0) fails.
	reg(c, "OSCopyFromClipboard", func(m *Machine, cpu *CPU, t *Thread) {
		buf, sizep := arg(cpu, 0), arg(cpu, 1)
		if sizep == 0 || buf == 0 && m.Mem.Read32(sizep) == 0 {
			ret(cpu, 0)
			return
		}
		m.Mem.Write32(sizep, 0)
		ret(cpu, 1)
	})
}
