package n64

import "errors"

// Temporary save states (main memory only) so the page host builds; the real
// ones replace this file.

// SaveState returns a snapshot of the machine.
func (m *Machine) SaveState() []byte {
	return append([]byte("TMP0"), m.rdram...)
}

// LoadState restores a snapshot made by SaveState.
func (m *Machine) LoadState(b []byte) error {
	if len(b) != 4+len(m.rdram) || string(b[:4]) != "TMP0" {
		return errors.New("that save state is for another version")
	}
	copy(m.rdram, b[4:])
	return nil
}
