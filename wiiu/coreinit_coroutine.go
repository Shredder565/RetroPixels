package wiiu

// Coroutines (OSInitCoroutine, OSSwitchCoroutine, ...): a saved set of the
// registers a function call keeps (the stack, r2, r13, r14-r31, f14-f31
// and their paired singles, cr, and where to go on), like setjmp/longjmp.
//
// OSCoroutine, 0x180 bytes:
//   0x00 where to go on   0x04 cr   0x08 gqr1   0x0C r1   0x10 r2   0x14 r13
//   0x18 r14..r31   0x60 f14..f31 (doubles)   0xF0 their paired singles

import "math"

const (
	coNIA = 0x00
	coCR  = 0x04
	coGQR = 0x08
	coR1  = 0x0C
	coR2  = 0x10
	coR13 = 0x14
	coGPR = 0x18
	coFPR = 0x60
	coPS  = 0xF0
)

// coSave stores the registers in co, to go on at lr.
func (m *Machine) coSave(c *CPU, co, lr uint32) {
	mem := m.Mem
	mem.Write32(co+coNIA, lr)
	mem.Write32(co+coCR, c.CR)
	mem.Write32(co+coGQR, c.GQR[1])
	mem.Write32(co+coR1, c.GPR[1])
	mem.Write32(co+coR2, c.GPR[2])
	mem.Write32(co+coR13, c.GPR[13])
	for i := 0; i < 18; i++ {
		mem.Write32(co+coGPR+uint32(i)*4, c.GPR[14+i])
		mem.Write64(co+coFPR+uint32(i)*8, math.Float64bits(c.FPR[14+i]))
		mem.Write64(co+coPS+uint32(i)*8, math.Float64bits(c.PS1[14+i]))
	}
}

// coLoad takes the registers from co and goes on where it says.
func (m *Machine) coLoad(c *CPU, co uint32) {
	mem := m.Mem
	c.CR = mem.Read32(co + coCR)
	c.GQR[1] = mem.Read32(co + coGQR)
	c.GPR[1] = mem.Read32(co + coR1)
	c.GPR[2] = mem.Read32(co + coR2)
	c.GPR[13] = mem.Read32(co + coR13)
	for i := 0; i < 18; i++ {
		c.GPR[14+i] = mem.Read32(co + coGPR + uint32(i)*4)
		c.FPR[14+i] = math.Float64frombits(mem.Read64(co + coFPR + uint32(i)*8))
		c.PS1[14+i] = math.Float64frombits(mem.Read64(co + coPS + uint32(i)*8))
	}
	c.PC = mem.Read32(co + coNIA)
	c.LR = c.PC
	m.jumped = true
}

func init() {
	c := "coreinit"
	// (co, entry, stack): starting it runs entry on that stack
	reg(c, "OSInitCoroutine", func(m *Machine, cpu *CPU, t *Thread) {
		co := arg(cpu, 0)
		m.Mem.Fill(co, 0, 0x180)
		m.Mem.Write32(co+coNIA, arg(cpu, 1))
		m.Mem.Write32(co+coR1, arg(cpu, 2))
		m.Mem.Write32(co+coR2, cpu.GPR[2])
		m.Mem.Write32(co+coR13, cpu.GPR[13])
		m.Mem.Write32(co+coGQR, cpu.GQR[1])
	})
	// (co): like setjmp, 0 now and the value OSLoadCoroutine gives later
	reg(c, "OSSaveCoroutine", func(m *Machine, cpu *CPU, t *Thread) {
		m.coSave(cpu, arg(cpu, 0), cpu.LR)
		ret(cpu, 0)
	})
	// (co, value)
	reg(c, "OSLoadCoroutine", func(m *Machine, cpu *CPU, t *Thread) {
		co, v := arg(cpu, 0), arg(cpu, 1)
		m.coLoad(cpu, co)
		ret(cpu, v)
	})
	// OSSwitchCoroutine is in coreinit_coroutine_cemu.go
}
