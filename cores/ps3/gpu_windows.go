package ps3

import machine "retropixels/ps3"

func useGPU(m *machine.Machine) error { return m.UseD3D11() }
