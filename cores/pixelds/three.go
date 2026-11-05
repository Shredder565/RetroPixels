package pixelds

import (
	"errors"

	"retropixels/cores/pixelds/ctr"
)

// threeDS puts the 3DS machine (cores/pixelds/ctr) behind the front. Its
// saves live in folders (the SD card and the game's save data), so there
// is no save file for the front to keep.
type threeDS struct {
	m      *ctr.Machine
	keys   uint32
	cx, cy int
}

func open3DS(name string, data []byte, o Options) (*Emulator, error) {
	opts := ctr.Options{SDCard: o.SDCard, SaveData: o.SaveDir, UserName: o.UserName, Log: o.Log, SampleRate: o.SampleRate}
	var m *ctr.Machine
	var err error
	if len(data) >= 4 && string(data[:4]) == "3DSX" {
		m, err = ctr.Load3DSX(data, opts)
	} else {
		m, err = ctr.Load3DS(data, opts)
	}
	if err != nil {
		return nil, err
	}
	return &Emulator{System: ThreeDS, m: &threeDS{m: m}, fps: ctr.FPS, w: ctr.TopW, h: ctr.ScreenH * 2,
		bottomX: (ctr.TopW - ctr.BottomW) / 2, bottomW: ctr.BottomW}, nil
}

func (t *threeDS) RunFrame()       { t.m.RunFrame() }
func (t *threeDS) Frame() []uint32 { return t.m.Frame() }

// SetKeys: the DS buttons are the first twelve 3DS ones, in the same
// order; ZL and ZR follow.
func (t *threeDS) SetKeys(bits uint32) {
	k := bits & 0xfff
	if bits&BtnZL != 0 {
		k |= ctr.KeyZL
	}
	if bits&BtnZR != 0 {
		k |= ctr.KeyZR
	}
	t.keys = k
	t.m.SetKeys(t.keys, t.cx, t.cy)
}

func (t *threeDS) setCircle(x, y float64) {
	t.cx, t.cy = int(x*156), int(y*156)
	t.m.SetKeys(t.keys, t.cx, t.cy)
}

func (t *threeDS) SetTouch(x, y int, down bool) { t.m.SetTouch(down, x, y) }
func (t *threeDS) TakeAudio() []float32         { return t.m.TakeAudio() }
func (t *threeDS) Save() []byte                 { return nil }
func (t *threeDS) SaveChanged() bool            { return false }
func (t *threeDS) SaveState() ([]byte, error) {
	return nil, errors.New("save states aren't in PixelDS's 3DS side yet")
}
func (t *threeDS) LoadState([]byte) error {
	return errors.New("save states aren't in PixelDS's 3DS side yet")
}
func (t *threeDS) Title() string    { return t.m.Title() }
func (t *threeDS) GameCode() string { return t.m.ProductCode() }
func (t *threeDS) Err() error       { return t.m.Err() }
