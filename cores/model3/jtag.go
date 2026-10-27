package model3

// The JTAG test port of the video board, reached through system registers
// 0x0C (write) and 0x10 (read). The games read the ID codes of the Real3D
// chips through it to check the board, and write the chips' mode words.

type jtagDev struct {
	asic  bool
	name  int // which ASIC (for mode words): 0 none, 1 Mercury, 2 Venus, 3 Earth, 4 Mars, 5 Jupiter
	id    uint32
	mode  uint32
	instr uint32
	sh    uint64 // shift register (bit 0 goes out on TDO)
	shLen uint32
}

func (d *jtagDev) reset() {
	if d.asic {
		d.shLen, d.sh, d.instr = 32, uint64(d.id), 3
	} else {
		d.shLen, d.sh, d.instr = 1, 0, 15
	}
}

func (d *jtagDev) shift(tdi bool) bool {
	out := d.sh&1 != 0
	d.sh >>= 1
	if tdi {
		d.sh |= 1 << (d.shLen - 1)
	} else {
		d.sh &^= 1 << (d.shLen - 1)
	}
	return out
}

func (d *jtagDev) captureDR() {
	if !d.asic {
		d.shLen, d.sh = 1, 0
		return
	}
	switch d.instr {
	case 3: // IDCODE
		d.shLen, d.sh = 32, uint64(d.id)
	case 8: // MODEWORD
		d.shLen, d.sh = 32, uint64(d.mode)
	default: // BYPASS
		d.shLen, d.sh = 1, 0
	}
}

func (d *jtagDev) captureIR() {
	if d.asic {
		d.shLen, d.sh = 5, 1
	} else {
		d.shLen, d.sh = 4, 9
	}
}

func (d *jtagDev) updateDR(j *jtag) {
	if d.asic && d.instr == 8 {
		d.mode = uint32(d.sh)
		if j.onMode != nil && d.name != 0 {
			j.onMode(d.name, d.mode)
		}
	}
}

func (d *jtagDev) updateIR() { d.instr = uint32(d.sh) }

const (
	jsReset = iota
	jsIdle
	jsSelDR
	jsCapDR
	jsShiftDR
	jsExit1DR
	jsPauseDR
	jsExit2DR
	jsUpdDR
	jsSelIR
	jsCapIR
	jsShiftIR
	jsExit1IR
	jsPauseIR
	jsExit2IR
	jsUpdIR
)

var jtagNext = [16][2]int{
	{jsIdle, jsReset}, {jsIdle, jsSelDR}, {jsCapDR, jsSelIR}, {jsShiftDR, jsExit1DR},
	{jsShiftDR, jsExit1DR}, {jsPauseDR, jsUpdDR}, {jsPauseDR, jsExit2DR}, {jsShiftDR, jsUpdDR},
	{jsIdle, jsSelDR}, {jsCapIR, jsReset}, {jsShiftIR, jsExit1IR}, {jsShiftIR, jsExit1IR},
	{jsPauseIR, jsUpdIR}, {jsPauseIR, jsExit2IR}, {jsShiftIR, jsUpdIR}, {jsIdle, jsSelDR},
}

type jtag struct {
	devs    []*jtagDev
	state   int
	lastTck bool
	tdo     bool
	onMode  func(asic int, v uint32)
}

// newJTAG builds the chain of the video board of a stepping (0x10, 0x15,
// 0x20, 0x21).
func newJTAG(step int) *jtag {
	j := &jtag{}
	asic := func(name int, id uint32) *jtagDev { return &jtagDev{asic: true, name: name, id: id} }
	ram := func() *jtagDev { return &jtagDev{} }
	switch step {
	case 0x21, 0x20:
		earth, jup := uint32(0x516c5057), uint32(0x516c7057)
		if step == 0x20 {
			earth, jup = 0x416c5057, 0x416c7057
		}
		j.devs = []*jtagDev{
			asic(5, jup), asic(1, 0x416c3057), asic(2, 0x316c4057), asic(3, earth),
			ram(), asic(4, 0x316c6057), asic(0, 0x316c6057), ram(), ram(), ram(),
			asic(0, earth), ram(), asic(0, 0x316c6057), asic(0, 0x316c6057), ram(), ram(), ram(),
		}
	case 0x15:
		j.devs = []*jtagDev{asic(5, 0x316c7057), asic(1, 0x316c3057), asic(2, 0x216c4057), asic(3, 0x316c5057),
			ram(), asic(4, 0x216c6057), asic(0, 0x216c6057), ram(), ram(), ram()}
	default:
		j.devs = []*jtagDev{asic(5, 0x116c7057), asic(1, 0x216c3057), asic(2, 0x116c4057), asic(3, 0x216c5057),
			ram(), asic(4, 0x116c6057), asic(0, 0x116c6057), ram(), ram(), ram()}
	}
	j.reset()
	return j
}

func (j *jtag) reset() {
	j.state = jsReset
	for _, d := range j.devs {
		d.reset()
	}
}

func (j *jtag) read() bool { return j.tdo }

func (j *jtag) write(tck, tms, tdi, trst bool) {
	if !trst {
		for _, d := range j.devs {
			d.reset()
		}
		return
	}
	if j.lastTck == tck {
		return
	}
	j.lastTck = tck
	if tck {
		switch j.state {
		case jsReset:
			for _, d := range j.devs {
				d.reset()
			}
		case jsCapDR:
			for _, d := range j.devs {
				d.captureDR()
			}
		case jsCapIR:
			for _, d := range j.devs {
				d.captureIR()
			}
		case jsShiftDR, jsShiftIR:
			for _, d := range j.devs {
				tdi = d.shift(tdi)
			}
		}
		t := 0
		if tms {
			t = 1
		}
		j.state = jtagNext[j.state][t]
		return
	}
	switch j.state {
	case jsShiftDR, jsShiftIR:
		j.tdo = j.devs[len(j.devs)-1].sh&1 != 0
	case jsUpdDR:
		for _, d := range j.devs {
			d.updateDR(j)
		}
	case jsUpdIR:
		for _, d := range j.devs {
			d.updateIR()
		}
	}
}
