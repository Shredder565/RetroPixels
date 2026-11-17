package wiiu

// The GamePad's touch panel. It reports raw 12-bit positions (0-4095) with
// y counting up from the bottom of the screen, and the system calibrates
// them to pixels. RetroPixels' panel uses the middle 3840 steps of each
// axis for the screen, with a 128-step border round it.

const (
	touchBorder = 128
	touchSpan   = 3840
)

// a touch sample's validity: 0 is good, bits 0 and 1 mark x and y invalid
const (
	touchValid     = 0
	touchInvalidXY = 3
)

// sizeof(VPADStatus)
const vpadStatusSize = 0xac

// tpRaw turns a touch at fx, fy (0 to 1, across and down) into the panel's
// raw position.
func tpRaw(fx, fy float32) (uint16, uint16) {
	fx = min(max(fx, 0), 1)
	fy = min(max(fy, 0), 1)
	x := touchBorder + fx*touchSpan
	y := 4095 - touchBorder - fy*touchSpan
	return uint16(x), uint16(y)
}

// rawToPixels turns a raw panel position into a pixel on a w x h screen.
func rawToPixels(x, y, w, h int32) (int32, int32) {
	fx := float64(max(x-touchBorder, 0)) / touchSpan
	fy := float64(max(4095-touchBorder-y, 0)) / touchSpan
	return int32(fx * float64(w)), int32(fy * float64(h))
}

// calibrateTouch copies the VPADTouchData at in (x, y, touched, validity,
// 16 bits each) to out with x and y turned into pixels.
func (m *Machine) calibrateTouch(out, in uint32, w, h int32) {
	mem := m.Mem
	px, py := rawToPixels(int32(int16(mem.Read16(in))), int32(int16(mem.Read16(in+2))), w, h)
	mem.Write16(out, uint16(px))
	mem.Write16(out+2, uint16(py))
	mem.Write16(out+4, mem.Read16(in+4))
	mem.Write16(out+6, mem.Read16(in+6))
}

// tpCalibrationResolution is the screen VPADGetTPCalibratedPointEx scales
// to: 0 is 1920x1080, 1 is 1280x720, 2 is 854x480.
func tpCalibrationResolution(r uint32) (int32, int32, bool) {
	sizes := [][2]int32{{1920, 1080}, {1280, 720}, {854, 480}}
	if r >= uint32(len(sizes)) {
		return 0, 0, false
	}
	return sizes[r][0], sizes[r][1], true
}
