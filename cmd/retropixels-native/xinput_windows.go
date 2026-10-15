package main

import (
	"sync"
	"syscall"
	"time"
	"unsafe"
)

// Xbox controllers through XInput (xinput1_4.dll on Windows 8 and later,
// xinput9_1_0.dll before that), up to 4.

type xinputGamepad struct {
	Buttons      uint16
	LeftTrigger  uint8
	RightTrigger uint8
	ThumbLX      int16
	ThumbLY      int16
	ThumbRX      int16
	ThumbRY      int16
}

type xinputState struct {
	Packet  uint32
	Gamepad xinputGamepad
}

type xinputVibration struct{ Left, Right uint16 }

// XInput button bits, and the standard gamepad button each one is.
var xinputButtons = [...]struct {
	bit uint16
	pad int
}{
	{0x1000, padA}, {0x2000, padB}, {0x4000, padX}, {0x8000, padY},
	{0x0100, padLB}, {0x0200, padRB}, {0x0020, padView}, {0x0010, padMenu},
	{0x0040, padL3}, {0x0080, padR3},
	{0x0001, padUp}, {0x0002, padDown}, {0x0004, padLeft}, {0x0008, padRight},
}

type xinput struct {
	getState, setState *syscall.LazyProc

	mu        sync.Mutex
	connected [4]bool
	lastTry   [4]time.Time // unplugged slots are slow to ask, so only now and then
	vib       [4]xinputVibration
}

func newXInput() *xinput {
	for _, name := range []string{"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"} {
		d := syscall.NewLazyDLL(name)
		if d.Load() != nil {
			continue
		}
		g, s := d.NewProc("XInputGetState"), d.NewProc("XInputSetState")
		if g.Find() == nil && s.Find() == nil {
			return &xinput{getState: g, setState: s}
		}
	}
	return &xinput{}
}

// axis: a stick reading as -1..1 (y flipped so down is +, like the browser).
func axis(v int16) float32 {
	f := float32(v) / 32767
	if f < -1 {
		f = -1
	}
	return f
}

func (x *xinput) read() [4]PadState {
	var out [4]PadState
	if x.getState == nil {
		return out
	}
	x.mu.Lock()
	defer x.mu.Unlock()
	now := time.Now()
	for i := 0; i < 4; i++ {
		if !x.connected[i] && now.Sub(x.lastTry[i]) < 2*time.Second {
			continue
		}
		var st xinputState
		r, _, _ := x.getState.Call(uintptr(i), uintptr(unsafe.Pointer(&st)))
		if r != 0 {
			x.connected[i] = false
			x.lastTry[i] = now
			continue
		}
		x.connected[i] = true
		p := &out[i]
		p.Connected = true
		for _, b := range xinputButtons {
			if st.Gamepad.Buttons&b.bit != 0 {
				p.Buttons[b.pad] = true
			}
		}
		p.Buttons[padLT] = st.Gamepad.LeftTrigger > 127
		p.Buttons[padRT] = st.Gamepad.RightTrigger > 127
		p.Axes = [4]float32{axis(st.Gamepad.ThumbLX), -axis(st.Gamepad.ThumbLY), axis(st.Gamepad.ThumbRX), -axis(st.Gamepad.ThumbRY)}
	}
	return out
}

// rumble sets the motors of the n-th connected controller (0..1 each).
func (x *xinput) rumble(n int, strong, weak float32) {
	if x.setState == nil {
		return
	}
	x.mu.Lock()
	defer x.mu.Unlock()
	slot := -1
	for i := 0; i < 4; i++ {
		if x.connected[i] {
			if n == 0 {
				slot = i
				break
			}
			n--
		}
	}
	if slot < 0 {
		return
	}
	v := xinputVibration{uint16(clamp01(strong) * 65535), uint16(clamp01(weak) * 65535)}
	if v == x.vib[slot] {
		return
	}
	x.vib[slot] = v
	x.setState.Call(uintptr(slot), uintptr(unsafe.Pointer(&v)))
}

// stopRumble turns every motor off.
func (x *xinput) stopRumble() {
	if x.setState == nil {
		return
	}
	x.mu.Lock()
	defer x.mu.Unlock()
	for i := 0; i < 4; i++ {
		if x.vib[i] != (xinputVibration{}) {
			x.vib[i] = xinputVibration{}
			v := xinputVibration{}
			x.setState.Call(uintptr(i), uintptr(unsafe.Pointer(&v)))
		}
	}
}

func clamp01(f float32) float32 {
	if f < 0 {
		return 0
	}
	if f > 1 {
		return 1
	}
	return f
}
