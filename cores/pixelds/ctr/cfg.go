package ctr

import (
	"encoding/binary"
	"unicode/utf16"
)

// CFG (cfg:u): the system settings a game reads: language, region,
// country, the user's name, the console model.

type cfgService struct {
	n string
	m *Machine
}

func newCFG(m *Machine, name string) *cfgService { return &cfgService{n: name, m: m} }

func (v *cfgService) name() string { return v.n }

// configBlock returns a settings block's contents.
func (m *Machine) configBlock(id, size uint32) []byte {
	b := make([]byte, size)
	put := func(data []byte) { copy(b, data) }
	switch id {
	case 0x000a0002: // language: 1 English
		put([]byte{1})
	case 0x000b0000: // country: United States (49)
		put([]byte{0, 0, 2, 49})
	case 0x000a0000: // user name, UTF-16
		name := m.opts.UserName
		if name == "" {
			name = "PixelDS"
		}
		for i, u := range utf16.Encode([]rune(name)) {
			if 2*i+1 < len(b) && i < 10 {
				binary.LittleEndian.PutUint16(b[2*i:], u)
			}
		}
	case 0x000a0001: // birthday: month, day
		put([]byte{1, 1})
	case 0x00070001: // sound output: stereo
		put([]byte{1})
	case 0x000f0004: // model: old 3DS
		put([]byte{0, 3})
	case 0x000d0000: // the accepted EULA version
		put([]byte{0xff, 0xff, 0, 0})
	case 0x00050005: // stereo camera settings
		f := []float32{62.0, 289.0, 76.80000305, 46.08000183, 10.0, 5.0, 55.58000183, 21.56999969}
		for i, x := range f {
			if 4*i+3 < len(b) {
				binary.LittleEndian.PutUint32(b[4*i:], mathFloat32bits(x))
			}
		}
	case 0x00130000: // system setup done
		put([]byte{1})
	}
	return b
}

func (v *cfgService) handle(c *ipc) {
	m := c.m
	switch c.id() {
	case 0x0001, 0x0401, 0x0801: // GetConfigInfoBlk2 / 8 (size, block, ..., buffer)
		size, id := c.arg(0), c.arg(1)
		buf := c.arg(3)
		if c.id() != 0x0001 {
			size, id, buf = c.arg(0), c.arg(1), c.arg(3)
		}
		m.mem.writeBytes(buf, m.configBlock(id, size))
	case 0x0002: // SecureInfoGetRegion -> 1 (USA)
		c.push(1)
	case 0x0003: // GenHashConsoleUnique -> two words
		c.push(0x12345678, 0x9abc)
	case 0x0004: // GetRegionCanadaUSA
		c.push(1)
	case 0x0005: // GetSystemModel -> 0 (3DS)
		c.push(0)
	case 0x0006: // GetModelNintendo2DS -> 1 (not a 2DS)
		c.push(1)
	case 0x0009: // GetCountryCodeString
		c.push(uint32('U') | uint32('S')<<8)
	case 0x000a: // GetCountryCodeID
		c.push(49)
	}
}

// PTM (ptm:u): power: the adapter, the battery, the shell.
type ptmService struct{ n string }

func (v *ptmService) name() string { return v.n }

func (v *ptmService) handle(c *ipc) {
	switch c.id() {
	case 0x0005: // GetAdapterState
		c.push(1)
	case 0x0006: // GetShellState: open
		c.push(1)
	case 0x0007: // GetBatteryLevel
		c.push(5)
	case 0x0008: // GetBatteryChargeState
		c.push(0)
	case 0x000b: // GetStepHistory
	case 0x000c: // GetTotalStepCount
		c.push(0)
	default:
		normal := c.header >> 6 & 63
		for i := uint32(0); i < normal && i < 2; i++ {
			c.push(0)
		}
	}
}

// DSP (dsp::DSP): sound. Not done yet: games run silent.
func newDSP(m *Machine) service { return &stubService{n: "dsp::DSP", m: m} }
