package pixelds

// Texture packs and the 3D render scale (DS games; see nds/texpack.go and
// nds/hires.go), and making a pack from the game (package texpack).

import (
	"errors"

	"retropixels/cores/pixelds/nds"
	"retropixels/cores/pixelds/texpack"
)

var errDSOnly = errors.New("texture packs and the 3D resolution are for DS games")

func (e *Emulator) ds() *nds.Machine {
	if d, ok := e.m.(dsMachine); ok {
		return d.Machine
	}
	return nil
}

// SetRenderScale draws DS 3D at n times the DS's resolution (1-8).
func (e *Emulator) SetRenderScale(n int) error {
	d := e.ds()
	if d == nil {
		return errDSOnly
	}
	d.SetRenderScale(n)
	return nil
}

// RenderScale is the DS 3D render scale (1 for 3DS games).
func (e *Emulator) RenderScale() int {
	if d := e.ds(); d != nil {
		return d.RenderScale()
	}
	return 1
}

// Display is the picture to show: the screens at the render scale, with
// its size (Screen and Width/Height stay at the handheld's own size, for
// the stylus, recording and screenshots of the game as it is).
func (e *Emulator) Display() ([]uint32, int, int) {
	if d := e.ds(); d != nil {
		if f := d.FrameHi(); f != nil {
			sc := d.RenderScale()
			return f, e.w * sc, e.h * sc
		}
	}
	return e.Screen(), e.w, e.h
}

// LoadTexturePack loads a texture pack folder ("" for none) and says how
// many pictures it has.
func (e *Emulator) LoadTexturePack(dir string) (int, error) {
	d := e.ds()
	if d == nil {
		return 0, errDSOnly
	}
	if err := d.SetTexturePack(dir); err != nil {
		return 0, err
	}
	return d.TexturePackSize(), nil
}

// EnableTexturePack turns the loaded texture pack on or off.
func (e *Emulator) EnableTexturePack(on bool) {
	if d := e.ds(); d != nil {
		d.EnableTexturePack(on)
	}
}

// DumpTextures writes the game's textures into dir as it draws them ("" to
// stop).
func (e *Emulator) DumpTextures(dir string) error {
	d := e.ds()
	if d == nil {
		return errDSOnly
	}
	return d.DumpTextures(dir)
}

// TexturesDumped is how many textures the dump folder has.
func (e *Emulator) TexturesDumped() int {
	if d := e.ds(); d != nil {
		return d.TexturesDumped()
	}
	return 0
}

// MakeTexturePack writes a texture pack for a DS game (see texpack.Make).
func MakeTexturePack(rom []byte, o texpack.Options) (texpack.Report, error) {
	if sys, _ := Detect("", rom); sys != DS {
		return texpack.Report{}, errDSOnly
	}
	return texpack.Make(rom, o)
}
