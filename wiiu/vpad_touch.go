// This Source Code Form is subject to the terms of the Mozilla Public License,
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at https://mozilla.org/MPL/2.0/.
//
// Based on Cemu (https://github.com/cemu-project/Cemu), src/Cafe/OS/libs/vpad/vpad.cpp
// and src/input/emulated/VPADController.cpp, rewritten in Go for RetroPixels.

package wiiu

// The GamePad's touch panel gives raw 12-bit coordinates that cover the
// screen with a margin: x from 92 across 3883 steps, y from the bottom.
const (
	tpOffsetX = 92
	tpOffsetY = 254
	tpScaleX  = 3883
	tpScaleY  = 3694

	tpValid     = 0
	tpInvalidXY = 3
)

// vpadStatusSize is sizeof(VPADStatus).
const vpadStatusSize = 0xac

// tpRaw turns a touch at fx, fy (0..1 across and down) into raw coordinates.
func tpRaw(fx, fy float32) (uint16, uint16) {
	fx = min(max(fx, 0), 1)
	fy = min(max(fy, 0), 1)
	return uint16(fx*tpScaleX + tpOffsetX), uint16(4095 - fy*tpScaleY - tpOffsetY)
}

// tpToResolution turns raw coordinates into pixels on a w x h screen.
func tpToResolution(x, y int32, w, h int32) (int32, int32) {
	x -= tpOffsetX
	y = 4095 - y - tpOffsetY
	x = max(x, 0)
	y = max(y, 0)
	return int32(float64(x) / tpScaleX * float64(w)), int32(float64(y) / tpScaleY * float64(h))
}

// calibrateTouch copies a VPADTouchData from in to out with x and y in pixels.
func (m *Machine) calibrateTouch(out, in uint32, w, h int32) {
	mem := m.Mem
	x, y := int32(int16(mem.Read16(in))), int32(int16(mem.Read16(in+2)))
	touch, valid := mem.Read16(in+4), mem.Read16(in+6)
	cx, cy := tpToResolution(x, y, w, h)
	mem.Write16(out, uint16(cx))
	mem.Write16(out+2, uint16(cy))
	mem.Write16(out+4, touch)
	mem.Write16(out+6, valid)
}

// tpCalibrationResolution is the screen size for VPADGetTPCalibratedPointEx.
func tpCalibrationResolution(r uint32) (int32, int32, bool) {
	switch r {
	case 0:
		return 1920, 1080, true
	case 1:
		return 1280, 720, true
	case 2:
		return 854, 480, true
	}
	return 0, 0, false
}
