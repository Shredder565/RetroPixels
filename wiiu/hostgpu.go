package wiiu

import "os"

// UseHostGPU moves drawing onto the host GPU where RetroPixels has a
// backend for it (Direct3D 11 on Windows). Draws the backend can't do still
// go to the software renderer, and setting RETROPIXELS_WIIU_GPU=software
// keeps everything there. It reports whether the host GPU is in use.
func (m *Machine) UseHostGPU() bool {
	if m.gpu.hw != nil {
		return true
	}
	if os.Getenv("RETROPIXELS_WIIU_GPU") == "software" {
		m.gpuNote = "turned off (RETROPIXELS_WIIU_GPU=software)"
		return false
	}
	hw, err := newHostGPU(m.gpu)
	if err != nil {
		m.logf("drawing in software: %v", err)
		m.gpuNote = err.Error()
		return false
	}
	m.gpu.hw = hw
	return true
}

// Close lets go of the host GPU, if the machine had one.
func (m *Machine) Close() {
	if m.gpu.hw != nil {
		m.gpu.hw.close()
		m.gpu.hw = nil
	}
}

// Renderer says what draws the pictures: the host GPU, or software (slow)
// and why.
func (m *Machine) Renderer() string {
	if m.gpu.hw != nil {
		return "Direct3D 11"
	}
	if m.gpuNote != "" {
		return "software, which is slow (Direct3D 11: " + m.gpuNote + ")"
	}
	return "software, which is slow"
}
