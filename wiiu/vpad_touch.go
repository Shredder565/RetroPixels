// Original code for RetroPixels. Not derived from Cemu.

package wiiu

// The GamePad touch panel reports raw 12-bit coordinates (0..4095) that are
// larger than the screen: the visible area starts tpOffsetX in from the left
// and spans tpScaleX steps across, and the y axis grows upward from
// tpOffsetY above the bottom over tpScaleY steps. VPADGetTPCalibratedPoint
// maps raw values onto a screen resolution.

const (
	tpOffsetX = 92
	tpOffsetY = 254
	tpScaleX  = 3883
	tpScaleY  = 3694

	tpRawMax = 4095

	// VPADTouchData.validity
	tpValid     = 0
	tpInvalidXY = 3 // neither x nor y is valid
)

// vpadStatusSize is the size of a VPADStatus.
const vpadStatusSize = 0xac

func unitClamp(v float32) float32 { return min(max(v, 0), 1) }

// tpRaw gives the raw panel reading for a touch at (fx, fy), fractions of
// the screen's width and height measured from the top left.
func tpRaw(fx, fy float32) (x, y uint16) {
	x = uint16(unitClamp(fx)*tpScaleX + tpOffsetX)
	y = uint16(tpRawMax - tpOffsetY - unitClamp(fy)*tpScaleY)
	return x, y
}

// tpToScreen maps a raw reading onto a w x h screen.
func tpToScreen(rx, ry, w, h int32) (int32, int32) {
	dx := max(rx-tpOffsetX, 0)
	dy := max(tpRawMax-tpOffsetY-ry, 0)
	sx := float64(dx) * float64(w) / tpScaleX
	sy := float64(dy) * float64(h) / tpScaleY
	return int32(sx), int32(sy)
}

// calibrateTouch reads the VPADTouchData { s16 x, s16 y, u16 touched,
// u16 validity } at in and writes it to out with x and y in pixels of a
// w x h screen.
func (m *Machine) calibrateTouch(out, in uint32, w, h int32) {
	mem := m.Mem
	rx := int32(int16(mem.Read16(in)))
	ry := int32(int16(mem.Read16(in + 2)))
	touched, validity := mem.Read16(in+4), mem.Read16(in+6)
	sx, sy := tpToScreen(rx, ry, w, h)
	mem.Write16(out, uint16(sx))
	mem.Write16(out+2, uint16(sy))
	mem.Write16(out+4, touched)
	mem.Write16(out+6, validity)
}

// tpCalibrationResolution is the screen size behind each
// VPADGetTPCalibratedPointEx resolution value.
func tpCalibrationResolution(r uint32) (w, h int32, ok bool) {
	sizes := [...][2]int32{{1920, 1080}, {1280, 720}, {854, 480}}
	if r >= uint32(len(sizes)) {
		return 0, 0, false
	}
	return sizes[r][0], sizes[r][1], true
}
