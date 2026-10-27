//go:build !windows

package ps3

import (
	"errors"

	machine "retropixels/ps3"
)

func useGPU(m *machine.Machine) error { return errors.New("only on Windows") }
