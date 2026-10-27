// Package core defines what every native RetroPixels emulator provides to the
// native app shell (window, sound, controllers, saves, recording).
//
// The native cores are Go ports of the emulators in web/emu; they behave the
// same way, frame for frame, so the native app plays exactly like the web one.
package core

import (
	"errors"
	"fmt"
)

// Options are what the shell hands a core when it opens a game.
type Options struct {
	SampleRate int    // sound output rate in Hz; cores make samples at this rate
	SRAM       []byte // battery save to restore, or nil
	Name       string // the game's file name, for cores that pick behaviour from it
	// Path: where the game's file is on disk ("" when it isn't a file, as
	// in the web player), for cores whose games are folders of files
	Path string
	// Dips: arcade machine settings (DIP switches) the player chose, by id;
	// settings not in it (or not valid) take the game's defaults.
	Dips map[string]int
	// Firmware: chip programs the player supplied, by chip name ("DSP-3",
	// "ST018", ...), for cartridges with such chips (opts.firmware in the web
	// player). A core that needs one it wasn't given returns an error naming
	// it (snes.FirmwareError).
	Firmware map[string][]byte
	// DataDir: where the program keeps its own files (saves/, firmware/),
	// for cores that install a game before playing it ("" = their own pick)
	DataDir string
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

// Memory is implemented by cores whose memory Archipelago's clients
// (archipelago.gg) can read and write; RetroPixels Native serves them through
// internal/apbridge. Domains are named the way BizHawk's cores name them, as
// web/emu/archipelago.js names them for the web player, because that's what
// the BizHawk Client's game handlers ask for:
//
//	gb      ROM, WRAM, VRAM, OAM, HRAM, System Bus; CartRAM when the cartridge has RAM
//	gba     ROM, EWRAM, IWRAM, BIOS, PALRAM, VRAM, OAM, SRAM, System Bus
//	nes     RAM, PRG ROM, WRAM, CIRAM (nametables), OAM, PALRAM, System Bus,
//	        CHR VROM or CHR VRAM; Battery RAM (= WRAM) for battery cartridges
//	n64     RDRAM, ROM, System Bus
//	segaMD  68K RAM, Z80 RAM, MD CART, SRAM, M68K BUS
//	sms     Main RAM, ROM, Cart (Volatile) RAM
//	snes    CARTROM, CARTRAM, WRAM, VRAM, CGRAM, OAM, APURAM, System Bus;
//	        SA1 IRAM for SA-1 cartridges (the SNI Client's usb2snes address
//	        space is mapped onto CARTROM, CARTRAM, WRAM, VRAM, APURAM, CGRAM
//	        and OAM: internal/apbridge's SNIRegions)
//
// The shell calls these only on the game's thread, between frames, and only
// for addr..addr+len-1 inside the domain (MemorySize); they must not change
// how the game runs (no side effects of reading a hardware register).
type Memory interface {
	MemoryDomains() []string
	MemorySize(domain string) int
	ReadMemory(domain string, addr uint32, buf []byte) error
	WriteMemory(domain string, addr uint32, data []byte) error
}

// Domain is one memory domain, for cores that implement Memory with Domains.
type Domain struct {
	Name string
	Mem  []byte // plain memory, or nil when Peek is used
	Size int    // the domain's size when Peek is used, or to use only the start of Mem
	// byte by byte (System Bus views); Poke nil = writes are dropped
	Peek func(addr uint32) byte
	Poke func(addr uint32, v byte)
	// Wrote, when set, is called after a write (to mark a save as changed)
	Wrote func()
}

func (d *Domain) size() int {
	if d.Peek != nil {
		return d.Size
	}
	if d.Size > 0 && d.Size < len(d.Mem) {
		return d.Size
	}
	return len(d.Mem)
}

// Domains implements Memory over a list of domains.
type Domains []Domain

func (ds Domains) find(name string, addr uint32, n int) (*Domain, error) {
	for i := range ds {
		if d := &ds[i]; d.Name == name {
			if int64(addr)+int64(n) > int64(d.size()) {
				return nil, errors.New("address out of range for " + name)
			}
			return d, nil
		}
	}
	return nil, fmt.Errorf("no memory domain named %q", name)
}

func (ds Domains) MemoryDomains() []string {
	out := make([]string, len(ds))
	for i := range ds {
		out[i] = ds[i].Name
	}
	return out
}

func (ds Domains) MemorySize(name string) int {
	for i := range ds {
		if ds[i].Name == name {
			return ds[i].size()
		}
	}
	return 0
}

func (ds Domains) ReadMemory(name string, addr uint32, buf []byte) error {
	d, err := ds.find(name, addr, len(buf))
	if err != nil {
		return err
	}
	if d.Peek == nil {
		copy(buf, d.Mem[addr:])
		return nil
	}
	for i := range buf {
		buf[i] = d.Peek(addr + uint32(i))
	}
	return nil
}

func (ds Domains) WriteMemory(name string, addr uint32, data []byte) error {
	d, err := ds.find(name, addr, len(data))
	if err != nil {
		return err
	}
	switch {
	case d.Peek == nil:
		copy(d.Mem[addr:], data)
	case d.Poke != nil:
		for i, v := range data {
			d.Poke(addr+uint32(i), v)
		}
	}
	if d.Wrote != nil {
		d.Wrote()
	}
	return nil
}
