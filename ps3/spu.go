package ps3

// SPUs: stage 3.

type spuState struct{}
type spursState struct{}

func (m *Machine) spuIdle() bool    { return false }
func (m *Machine) runSPUs(n uint64) { m.audioTick() }
