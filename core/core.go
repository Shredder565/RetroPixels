// Package core defines what every native RetroPixels emulator provides to the
// native app shell (window, sound, controllers, saves, recording).
//
// The native cores are Go ports of the emulators in web/emu; they behave the
// same way, frame for frame, so the native app plays exactly like the web one.
package core

// Options are what the shell hands a core when it opens a game.
type Options struct {
	SampleRate int    // sound output rate in Hz; cores make samples at this rate
	SRAM       []byte // battery save to restore, or nil
	Name       string // the game's file name, for cores that pick behaviour from it
	// Dips: arcade machine settings (DIP switches) the player chose, by id;
	// settings not in it (or not valid) take the game's defaults.
	Dips map[string]int
}

// Info describes a system's controls and picture.
type Info struct {
	System  string   // short id, as in web/index.html ("nes", "atari2600", ...)
	Buttons []string // input bit i means Buttons[i] is held (same names and order as the web core)
	Players int      // controllers the system reads
	Stereo  bool     // TakeAudio returns interleaved L,R pairs when true, mono otherwise
	Aspect  float64  // display aspect ratio of the picture (0 = use Width/Height)
}

// Core is one running game.
type Core interface {
	Info() Info
	FPS() float64

	// Width and Height of the current picture; they may change between frames.
	Width() int
	Height() int
	// Screen is the current picture, Width*Height pixels, row by row, each
	// 0x00RRGGBB (so its bytes in memory are B,G,R,0, ready for a 32-bit DIB).
	Screen() []uint32

	SetInput(player int, bits uint32)
	RunFrame()
	// TakeAudio returns the samples made since the last call, as the web core
	// makes them (floats, roughly -1..1; the shell scales and clips).
	TakeAudio() []float32

	Reset()
	SaveState() ([]byte, error)
	LoadState([]byte) error
	// SRAM is the battery save, or nil when the cartridge has none.
	SRAM() []byte
}

// Analog is implemented by cores with analog controls (Atari paddles, N64
// sticks). v is -1..1, as the web cores take it (the 2600 reads axis 0).
type Analog interface {
	SetAnalog(player, axis int, v float32)
}

// Rumbler is implemented by cores whose cartridges can shake the controller;
// Rumble returns 0 (off) to 1 (full).
type Rumbler interface {
	Rumble() float32
}
