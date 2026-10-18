// This Source Code Form is subject to the terms of the Mozilla Public License,
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at https://mozilla.org/MPL/2.0/.
//
// Based on Cemu (https://github.com/cemu-project/Cemu), src/Cafe/OS/libs/coreinit/
// coreinit_FG.cpp, rewritten in Go for RetroPixels.

package wiiu

// The foreground bucket: 64 MB the program in the foreground shares with
// the system. Only its first area is free for the program; the others hold
// the OS's copy data (the clipboard and title switch parameters), the
// transition audio, frame storage and a save area.

const (
	fgAreaFree = 0
	fgAreaSave = 6

	fgFreeSize = 0x02800000
)

// the areas: id, offset, size
var fgAreas = [...][3]uint32{
	{0, 0, 0x2800000},
	{7, 0x2800000, 0x400000},
	{1, 0x2c00000, 0x900000},
	{2, 0x3500000, 0x3c0000},
	{3, 0x38c0000, 0x1c0000},
	{4, 0x3a80000, 0x3c0000},
	{5, 0x3e40000, 0x1bf000},
	{6, 0x3fff000, 0x1000},
}

func fgArea(id uint32) uint32 {
	for _, a := range fgAreas {
		if a[0] == id {
			return FGBase + a[1]
		}
	}
	return 0
}

func (m *Machine) fgFreeArea() (addr, size uint32) { return fgArea(fgAreaFree), fgFreeSize }

// initForegroundBucket marks the save area as set up (the bucket starts out
// zeroed, as fresh memory is).
func (m *Machine) initForegroundBucket() {
	save := fgArea(fgAreaSave)
	m.Mem.Write32(save, 0x53617665) // "Save"
	m.Mem.Write32(save+8, m.Mem.Read32(save+8)|0x300)
}

func init() {
	c := "coreinit"
	// OSGetForegroundBucket(address out, size out): the whole bucket
	reg(c, "OSGetForegroundBucket", func(m *Machine, cpu *CPU, t *Thread) {
		if p := arg(cpu, 0); p != 0 {
			m.Mem.Write32(p, FGBase)
		}
		if p := arg(cpu, 1); p != 0 {
			m.Mem.Write32(p, FGSize)
		}
		ret(cpu, 1)
	})
	// OSGetForegroundBucketFreeArea: the part the program may use
	reg(c, "OSGetForegroundBucketFreeArea", func(m *Machine, cpu *CPU, t *Thread) {
		a, n := m.fgFreeArea()
		if p := arg(cpu, 0); p != 0 {
			m.Mem.Write32(p, a)
		}
		if p := arg(cpu, 1); p != 0 {
			m.Mem.Write32(p, n)
		}
		ret(cpu, 1)
	})
	// OSCopyFromClipboard(buffer, size in/out): the clipboard is empty
	reg(c, "OSCopyFromClipboard", func(m *Machine, cpu *CPU, t *Thread) {
		buf, size := arg(cpu, 0), arg(cpu, 1)
		if size == 0 || m.Mem.Read32(size) == 0 && buf == 0 {
			ret(cpu, 0)
			return
		}
		m.Mem.Write32(size, 0)
		ret(cpu, 1)
	})
}
