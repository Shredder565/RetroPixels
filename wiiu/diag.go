package wiiu

// Details for the log when a program crashes or gets stuck: the last
// library calls it made, its registers, and the code around where it was,
// so a problem can be worked out from a player's log without the game.

import (
	"fmt"
	"strings"
)

const recentCalls = 48

type callRec struct {
	n            uint32 // the library call
	a            [4]uint32
	lr, r3, core uint32
}

// noteCall remembers a library call and what it returned.
func (m *Machine) noteCall(n uint32, c *CPU, args [4]uint32) {
	m.recent[m.recentPos%recentCalls] = callRec{n, args, c.LR, c.GPR[3], uint32(c.Core)}
	m.recentPos++
}

// RecentCalls lists the last library calls, oldest first.
func (m *Machine) RecentCalls() string {
	var b strings.Builder
	start := 0
	if m.recentPos > recentCalls {
		start = m.recentPos - recentCalls
	}
	for i := start; i < m.recentPos; i++ {
		r := m.recent[i%recentCalls]
		name := "?"
		if int(r.n) < len(m.hle) {
			name = m.hle[r.n].lib + "." + m.hle[r.n].name
		}
		fmt.Fprintf(&b, "  [%d] %s(%08X %08X %08X %08X) = %08X, from %08X%s\n", r.core, name,
			r.a[0], r.a[1], r.a[2], r.a[3], r.r3, r.lr, m.where(r.lr))
	}
	return b.String()
}

// CodeAt is the program's code around addr (16 words before, 8 after),
// as hex words.
func (m *Machine) CodeAt(addr uint32) string {
	addr &^= 3
	if !m.Mem.Mapped(addr) || addr < 64 {
		return ""
	}
	var b strings.Builder
	fmt.Fprintf(&b, "code at %08X%s:", addr, m.where(addr))
	for a := addr - 64; a < addr+32; a += 4 {
		if (a-(addr-64))%32 == 0 {
			fmt.Fprintf(&b, "\n  %08X:", a)
		}
		mark := " "
		if a == addr {
			mark = ">"
		}
		fmt.Fprintf(&b, "%s%08X", mark, m.Mem.Read32(a))
	}
	return b.String()
}

// Registers of a thread's context, for a crash report.
func registers(c *Context) string {
	var b strings.Builder
	for i := 0; i < 32; i++ {
		fmt.Fprintf(&b, "r%-2d %08X", i, c.GPR[i])
		if i%8 == 7 {
			b.WriteString("\n")
		} else {
			b.WriteString("  ")
		}
	}
	fmt.Fprintf(&b, "pc %08X  lr %08X  ctr %08X  cr %08X  xer %08X", c.PC, c.LR, c.CTR, c.CR, c.XER)
	return b.String()
}

// StuckReport describes where each thread is, with the code it waits in,
// for a program that runs but shows nothing.
func (m *Machine) StuckReport() string {
	var b strings.Builder
	seen := map[uint32]bool{}
	for _, t := range m.threadList {
		if t.State == thDone || t.State == thDormant {
			continue
		}
		for _, a := range []uint32{t.Ctx.PC, t.Ctx.LR} {
			if a < CodeBase || a >= DataBase || seen[a&^3] {
				continue
			}
			seen[a&^3] = true
			if s := m.CodeAt(a); s != "" {
				fmt.Fprintf(&b, "thread %d: %s\n", t.ID, s)
			}
		}
	}
	return b.String()
}

// Pictures is how many pictures the program has shown.
func (m *Machine) Pictures() uint32 { return m.gx2.Flips }
