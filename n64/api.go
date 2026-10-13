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
