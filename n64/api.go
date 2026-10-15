package n64

// Small helpers for programs that drive a Machine (RetroPixels' page host).

// FPS is how many frames the console shows per second (50 for PAL games).
func (m *Machine) FPS() float64 {
	if m.pal {
		return 50
	}
	return 60
}

// SaveChanged reports whether the game wrote to its save memory since the
// last call.
func (m *Machine) SaveChanged() bool {
	d := m.Save.dirty
	m.Save.dirty = false
	return d
}

// RDRAM is the console's main memory (8 MB with the Expansion Pak), in the
// console's own big-endian byte order. Changes show up in the running game.
func (m *Machine) RDRAM() []byte { return m.rdram }

// ROM is the game's cartridge ROM in big-endian (.z64) byte order.
func (m *Machine) ROM() []byte { return m.rom }
