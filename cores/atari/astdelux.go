package atari

import (
	"retropixels/cores/arcade"
	"retropixels/cores/cpu6502"
	"retropixels/cores/pokey"
)

// Asteroids Deluxe (Atari, 1980).
//
// One 6502 at 1.512 MHz with 8 KB of program ROM at $6000-$7FFF (the vectors
// at $FFFA read its top: the top address line isn't connected) and 1 KB of
// RAM whose pages 2 and 3 swap for the second player. The picture is drawn
// by the Digital Vector Generator from 2 KB of vector RAM at $4000 and 4 KB
// of vector ROM at $4800 (some of which is also program code). The NMI comes
// every 6144 cycles (the 3 kHz clock / 12); the game draws one picture every
// four. The sound is a POKEY (which also reads the second switch bank) and
// two discrete circuits, the thrust and the explosions, made here with
// RetroPixels' own synthesis. An ER2055 EAROM keeps the high scores.
//
// The vector picture is drawn into a 640x480 raster.

const (
	adCPUHz    = 1512000
	adNMICyc   = 6144
	adFrameCyc = 4 * adNMICyc // 24576: 61.5 frames a second
	adW        = 640
	adH        = 480
	adSampleN  = 32 // POKEY clocks per sample (47.25 kHz)
	adRate     = adCPUHz / adSampleN
)

const adFPS = float64(adCPUHz) / adFrameCyc

type astdelux struct {
	dips map[string]int
	rom  []byte // $6000-$7FFF
	vrom []byte // $4800-$57FF

	ram   [0x400]uint8
	vram  [0x800]uint8
	latch [8]uint8 // $3C00-$3C07 (bit 7 of each write)
	er    earom
	snd   *pokey.Chip
	out   pokeyOut
	boom  explosion
	thr   thrust

	cpu      *cpu6502.CPU
	frameCyc int64 // the CPU's cycle count at the start of this frame
	busyTill int64 // the DVG draws until this cycle
	potRead  bool  // the POKEY has scanned its pots since its last reset
	sndCyc   int
	watchdog int

	lines []dvgLine // the last picture the DVG drew
	ras   *vecRaster
	pads  []arcade.Pad
	res   *arcade.Resampler
}

type adBus struct{ b *astdelux }

func (x adBus) Read(a uint16) uint8     { return x.b.read(int(a)) }
func (x adBus) Write(a uint16, v uint8) { x.b.write(int(a), v) }

func newAstdelux(g *arcade.Game, files arcade.Files, opts arcade.Opts) (arcade.Board, error) {
	b := &astdelux{dips: opts.Dips}
	b.rom = arcade.Region(g, files, "cpu", 0x2000)
	b.vrom = arcade.Region(g, files, "vector", 0x1000)
	if len(opts.SRAM) == len(b.er.mem) {
		copy(b.er.mem[:], opts.SRAM)
	}
	b.snd = newPOKEY()
	b.cpu = cpu6502.New(adBus{b}, cpu6502.Options{Decimal: true})
	// the playfield is the DVG's x 0-1023 and y 128-895, and the scores and the
	// copyright line reach a little past it: show x -21 to 1045, y 116 to 916 (4:3)
	b.ras = newVecRaster(adW, adH, -21, 116, 1066, 800)
	b.res = arcade.NewResampler(adRate, float64(opts.SampleRate))
	b.pads = []arcade.Pad{{}, {}}
	b.Reset()
	return b, nil
}

func (b *astdelux) Reset() {
	b.ram, b.vram, b.latch = [0x400]uint8{}, [0x800]uint8{}, [8]uint8{}
	mem := b.er.mem
	b.er = earom{mem: mem}
	*b.snd = *newPOKEY()
	b.out = pokeyOut{}
	b.boom, b.thr = explosion{}, thrust{}
	b.cpu.NMIPending = false
	b.cpu.Reset()
	b.frameCyc, b.busyTill = b.cpu.Cycles, 0
	b.sndCyc, b.watchdog = 0, 0
	b.lines = b.lines[:0]
	b.res.Clear()
}

// ramAt: pages 2 and 3 trade places while latch bit 4 is on (player 2).
func (b *astdelux) ramAt(a int) int {
	if a >= 0x200 && b.latch[4] != 0 {
		return a ^ 0x100
	}
	return a
}

// switches $2000-$2007: bit 7 is the switch (on = 1).
func (b *astdelux) in0(n int) bool {
	p := b.pads[0]
	switch n {
	case 1: // the 3 kHz clock
		return b.cpu.Cycles&256 != 0
	case 2: // the DVG is drawing
		return b.cpu.Cycles < b.busyTill
	case 3:
		return p["Down"] // shields
	case 4:
		return p["Fire"]
	case 6: // slam switch
		return false
	case 7:
		return b.dips["test"] != 0
	}
	return false
}

// switches $2400-$2407
func (b *astdelux) in1(n int) bool {
	p := b.pads[0]
	switch n {
	case 0:
		return p["Coin"]
	case 1:
		return p["Coin 2"]
	case 2:
		return false // the third (right) coin slot
	case 3:
		return p["Start (1P)"]
	case 4:
		return p["Start (2P)"]
	case 5:
		return p["Up"] // thrust
	case 6:
		return p["Right"]
	case 7:
		return p["Left"]
	}
	return false
}

func bit7(on bool) uint8 {
	if on {
		return 0x80
	}
	return 0
}

func (b *astdelux) read(a int) uint8 {
	a &= 0x7fff
	switch {
	case a < 0x400:
		return b.ram[b.ramAt(a)]
	case a >= 0x6000:
		return b.rom[a-0x6000]
	case a >= 0x4000 && a < 0x4800:
		return b.vram[a-0x4000]
	case a >= 0x4800 && a < 0x5800:
		return b.vrom[a-0x4800]
	case a >= 0x2000 && a < 0x2008:
		return bit7(b.in0(a & 7))
	case a >= 0x2400 && a < 0x2408:
		return bit7(b.in1(a & 7))
	case a >= 0x2800 && a < 0x2804: // the first switch bank, two at a time
		return uint8(b.dsw1()>>(6-2*(a&3))) & 3
	case a >= 0x2c00 && a < 0x2c10:
		b.snd.Pot[8] = b.allpot()
		return b.snd.Read(a, b.cpu.Cycles)
	case a >= 0x2c40 && a < 0x2c80:
		return b.er.read()
	}
	return 0
}

func (b *astdelux) write(a int, v uint8) {
	a &= 0x7fff
	switch {
	case a < 0x400:
		b.ram[b.ramAt(a)] = v
	case a >= 0x4000 && a < 0x4800:
		b.vram[a-0x4000] = v
	case a >= 0x2c00 && a < 0x2c10:
		b.snd.Write(a, v)
		switch {
		case a&15 == 11: // POTGO
			b.potRead = true
		case a&15 == 15 && v&3 == 0: // SKCTL: the chip held in reset
			b.potRead = false
		}
	case a == 0x3000: // start the DVG
		b.runDVG()
	case a >= 0x3200 && a < 0x3240:
		b.er.latch(a, v)
	case a == 0x3400:
		b.watchdog = 0
	case a == 0x3600:
		b.boom.set(v)
	case a == 0x3a00:
		b.er.control(v)
	case a >= 0x3c00 && a < 0x3c08:
		b.latch[a&7] = v >> 7
		b.thr.on = b.latch[3] != 0
	case a == 0x3e00:
		b.boom.noise = 0 // the noise generator's reset
	}
}

// dvgWord reads a word of the DVG's address space.
func (b *astdelux) dvgWord(w int) uint16 {
	a := (w & 0xfff) * 2
	var lo, hi uint8
	switch {
	case a < 0x800:
		lo, hi = b.vram[a], b.vram[a+1]
	case a < 0x1800:
		lo, hi = b.vrom[a-0x800], b.vrom[a-0x7ff]
	}
	return uint16(hi)<<8 | uint16(lo)
}

// runDVG draws the whole list at once (the game doesn't touch the list
// being drawn) and keeps the busy flag up for as long as the drawing takes.
func (b *astdelux) runDVG() {
	var n int
	b.lines, n = dvgRun(b.dvgWord, b.lines)
	b.busyTill = b.cpu.Cycles + int64(n)
}

// allpot: the second switch bank is wired to the POKEY's pot lines. A
// switch that is on keeps its line from charging, so once the pots have
// been scanned its bit reads 1; the rest read 0, and all of them read 0
// before the first scan after a reset of the chip.
func (b *astdelux) allpot() uint8 {
	if b.potRead {
		return b.dsw2()
	}
	return 0
}

func (b *astdelux) dsw1() int {
	d := b.dips
	return d["language"] | d["lives"] | d["minimum"] | d["difficulty"] | d["bonus"]
}

func (b *astdelux) dsw2() uint8 {
	d := b.dips
	return uint8(d["coinage"] | d["rightcoin"] | d["centercoin"] | d["bonuscoins"])
}

func (b *astdelux) SetInput(p []arcade.Pad) { b.pads = p }

func (b *astdelux) Screen() []uint32 { return b.ras.pix }

func (b *astdelux) RunFrame() {
	for i := 1; i <= 4; i++ {
		end := b.frameCyc + int64(i*adNMICyc)
		for b.cpu.Cycles < end {
			b.sound(b.cpu.Step())
		}
		b.cpu.NMIPending = true
	}
	b.ras.draw(b.lines)
	b.frameCyc += adFrameCyc
	b.watchdog++
	if b.watchdog > 16 { // the game stopped: the watchdog resets it
		b.watchdog = 0
		b.cpu.Reset()
	}
}

func (b *astdelux) sound(n int) {
	b.sndCyc += n
	for b.sndCyc >= adSampleN {
		b.sndCyc -= adSampleN
		v := b.out.next(b.snd.Run(adSampleN)) + b.boom.next() + b.thr.next()
		b.res.Add(v)
	}
}

func (b *astdelux) TakeAudio() []float32 { return b.res.Take() }

func (b *astdelux) SRAM() []byte { return append([]byte(nil), b.er.mem[:]...) }

func (b *astdelux) SaveState() ([]byte, error) {
	w := &arcade.StateW{}
	w.Bytes(b.cpu.SaveState())
	w.Bytes(b.ram[:])
	w.Bytes(b.vram[:])
	w.Bytes(b.latch[:])
	b.er.save(w)
	w.B = b.snd.AppendState(w.B)
	b.boom.save(w)
	b.thr.save(w)
	w.Int(int(b.busyTill - b.cpu.Cycles)) // how long the DVG still draws
	w.Bool(b.potRead)
	w.F64(b.out.lp)
	w.Int(int(b.cpu.Cycles - b.frameCyc))
	w.Int(b.sndCyc)
	w.Int(b.watchdog)
	b.res.AppendState(w)
	return w.B, nil
}

func (b *astdelux) LoadState(st []byte) error {
	if len(st) < cpu6502.StateSize {
		return arcade.ErrBadState
	}
	cpu := *b.cpu
	if err := cpu.LoadState(st[:cpu6502.StateSize]); err != nil {
		return err
	}
	n := *b
	snd := *b.snd
	res := *b.res
	r := &arcade.StateR{B: st[cpu6502.StateSize:]}
	r.Bytes(n.ram[:])
	r.Bytes(n.vram[:])
	r.Bytes(n.latch[:])
	n.er.load(r)
	if r.Err == nil {
		rest, err := snd.LoadState(r.B)
		if err != nil {
			return arcade.ErrBadState
		}
		r.B = rest
	}
	n.boom.load(r)
	n.thr.load(r)
	busy := int64(r.Int())
	n.potRead = r.Bool()
	n.out.lp = r.F64()
	into := int64(r.Int())
	n.sndCyc = r.Int()
	n.watchdog = r.Int()
	res.LoadState(r)
	if err := r.Done(); err != nil {
		return err
	}
	// keep the board's own objects (the framework holds b)
	cpu.SetBus(adBus{b})
	*b.cpu, *b.snd, *b.res = cpu, snd, res
	n.cpu, n.snd, n.res, n.ras, n.lines = b.cpu, b.snd, b.res, b.ras, b.lines
	*b = n
	b.busyTill = b.cpu.Cycles + busy
	b.frameCyc = b.cpu.Cycles - into
	// redraw the picture from the vector list in the state
	b.runDVG()
	b.busyTill = b.cpu.Cycles + busy
	b.ras.draw(b.lines)
	return nil
}

var adDips = []arcade.Dip{
	{ID: "language", Label: "Language", Default: 0, Options: []arcade.DipOption{dopt("English", 0), dopt("German", 1), dopt("French", 2), dopt("Spanish", 3)}},
	{ID: "lives", Label: "Ships", Default: 0, Options: []arcade.DipOption{dopt("2-4", 0), dopt("3-5", 4), dopt("4-6", 8), dopt("5-7", 0xc)}},
	{ID: "minimum", Label: "Minimum plays", Default: 0, Options: []arcade.DipOption{dopt("1", 0), dopt("2", 0x10)}},
	{ID: "difficulty", Label: "Difficulty", Default: 0, Options: []arcade.DipOption{dopt("Easy", 0), dopt("Hard", 0x20)}},
	{ID: "bonus", Label: "Bonus ship", Default: 0, Options: []arcade.DipOption{dopt("10,000", 0), dopt("12,000", 0x40), dopt("15,000", 0x80), dopt("None", 0xc0)}},
	{ID: "coinage", Label: "Coinage", Default: 1, Options: []arcade.DipOption{dopt("2 coins, 1 credit", 0), dopt("1 coin, 1 credit", 1), dopt("1 coin, 2 credits", 2), dopt("Free play", 3)}},
	{ID: "rightcoin", Label: "Right coin slot", Default: 0xc, Options: []arcade.DipOption{dopt("x1", 0xc), dopt("x4", 8), dopt("x5", 4), dopt("x6", 0)}},
	{ID: "centercoin", Label: "Centre coin slot", Default: 0x10, Options: []arcade.DipOption{dopt("x1", 0x10), dopt("x2", 0)}},
	{ID: "bonuscoins", Label: "Bonus coins", Default: 0xe0, Options: []arcade.DipOption{dopt("None", 0xe0), dopt("1 each 2 coins", 0xc0), dopt("1 each 4 coins", 0xa0), dopt("2 each 4 coins", 0x80), dopt("1 each 5 coins", 0x60)}},
	{ID: "test", Label: "Service mode (test screen)", Default: 0, Options: []arcade.DipOption{dopt("Off", 0), dopt("On", 1)}},
}

func init() {
	files := []arcade.File{
		{Name: "036430.02", Alt: []string{"036430-02.d1"}, Size: 0x800, Role: "cpu", Offset: 0x0000},
		{Name: "036431.02", Alt: []string{"036431-02.ef1"}, Size: 0x800, Role: "cpu", Offset: 0x0800},
		{Name: "036432.02", Alt: []string{"036432-02.fh1"}, Size: 0x800, Role: "cpu", Offset: 0x1000},
		{Name: "036433.03", Alt: []string{"036433-03.j1"}, Size: 0x800, Role: "cpu", Offset: 0x1800},
		{Name: "036800.02", Alt: []string{"036800-02.r2"}, Size: 0x800, Role: "vector", Offset: 0x0000},
		{Name: "036799.01", Alt: []string{"036799-01.np2"}, Size: 0x800, Role: "vector", Offset: 0x0800},
		// the DVG's state machine: the DVG is emulated from its documented behaviour
		{Name: "034602-01.c8", Size: 0x100, Role: "prom", Offset: 0, Optional: true},
	}
	arcade.Register(&arcade.BoardDef{
		ID: "astdelux", Title: "Atari Asteroids Deluxe", Width: adW, Height: adH, FPS: adFPS,
		// Up is thrust and Down the shields
		Buttons: []string{"Left", "Right", "Up", "Down", "Fire", "Coin", "Start (1P)", "Start (2P)", "Coin 2"},
		Dips:    adDips,
		Games: []arcade.Game{
			{ID: "astdelux", Title: "Asteroids Deluxe (revision 3)", Year: 1980, Maker: "Atari", Files: files},
		},
		New: newAstdelux,
	})
}
