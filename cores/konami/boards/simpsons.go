package boards

import (
	"math"

	"retropixels/cores/arcade"
	"retropixels/cores/konami"
	"retropixels/cores/konami2"
	"retropixels/cores/ym2151"
	"retropixels/cores/z80"
)

// ---- The Simpsons (1991): see arcade_simpsons.js ----

const (
	simLineCyc   = 192
	simLines     = 264
	simW         = 288
	simH         = 224
	simTop       = 16
	simLeft      = 112
	simNMIDelay  = 179
	simFIRQDelay = 256
)

var (
	simCPUHz   = float64(3000000)
	simZ80Hz   = float64(3579545)
	simYMHz    = float64(3579545)
	simPCMGain = 1.6
	simFPS     = simCPUHz / (float64(simLineCyc) * float64(simLines))
	simSndCyc  = float64(simLineCyc) * simZ80Hz / simCPUHz
	simShade   [256]uint8
)

func init() {
	for i := range simShade {
		simShade[i] = uint8(math.Floor(float64(i) * 0.6))
	}
}

type simpsons struct {
	game       *arcade.Game
	sampleRate int
	prg        []byte
	fixedBase  int
	sndRom     []byte
	ram        [0x2000]uint8
	palRam     [0x1000]uint8
	sprRam     [0x2000]uint8
	sndRam     [0x800]uint8
	palette    [2048]uint32
	screen     []uint32

	layerBase  [3]int
	spriteBase int
	k052109    *konami.K052109
	k053247    *konami.K053245
	k053251    *konami.K053251
	k053260    *konami.K053260
	ym         *ym2151.YM2151
	eeprom     *konami.Eeprom93C46
	cpu        *konami2.CPU
	z80        *z80.CPU

	layerPens         [3][]uint16
	sprPens           []uint16
	sprPri, sprShadow []uint8
	mixPens           []uint16
	mixShade          []uint8
	inputs            []arcade.Pad
	resampler         *arcade.Resampler
	pcmBuf            []float32

	bank, videoBank     int
	firqEnabled, vblank bool
	eepromOut, ctrl     int
	firqAt              int
	sndBank             int
	nmiAt, zCyc, ymFrac float64
	cyc, testHold       int
}

type simMain struct{ b *simpsons }
type simSound struct{ b *simpsons }

func newSimpsons(g *arcade.Game, files arcade.Files, opts arcade.Opts) (arcade.Board, error) {
	b := &simpsons{game: g, sampleRate: opts.SampleRate}
	if b.sampleRate == 0 {
		b.sampleRate = 48000
	}
	b.prg = arcade.Region(g, files, "cpu", 0x80000)
	b.fixedBase = len(b.prg) - 0x8000
	b.sndRom = arcade.Region(g, files, "sound", 0x20000)
	tileRom := interleaveRole(g, files, "tiles", 2, 4)
	spriteRom := interleaveRole(g, files, "sprites", 2, 8)
	pcm := arcade.Region(g, files, "pcm", 0x140000)
	b.screen = make([]uint32, simW*simH)

	b.k052109 = konami.NewK052109(konami.K052109Opts{
		ROM: tileRom,
		TileCallback: func(t *konami.Tile) {
			c := t.Color
			t.Code |= (c&0x3f)<<8 | t.Bank<<14
			t.Color = b.layerBase[t.Layer] + (c&0xc0)>>6
		},
	})
	b.k052109.DX = [3]int{0, -6, -6}
	b.k053247 = konami.NewK053245(konami.K053245Opts{
		Type: "053247", ROM: spriteRom, Layout: "packed", DX: -59, DY: -39, CodeCounters: true, ZoomStep: true, SubtractOffsetX: true,
		SpriteCallback: func(t *konami.Sprite2) {
			t.Pri = (t.Attr & 0x0f80) >> 6
			t.Color = b.spriteBase + t.Attr&0x1f
			t.Shadow = (t.Attr >> 10) & 3
		},
	})
	b.k053251 = &konami.K053251{}
	b.k053260 = konami.NewK053260(pcm, simYMHz)
	b.ym = ym2151.New(simYMHz)
	var nv []byte
	if len(opts.SRAM) >= 128 {
		nv = opts.SRAM
	}
	if nv == nil {
		if d := roleFileIfPresent(g, files, "eeprom"); len(d) >= 128 {
			nv = d
		}
	}
	b.eeprom = konami.NewEeprom93C46(nv, 8, 9, true)
	b.cpu = konami2.New(simMain{b})
	b.z80 = z80.New(simSound{b})
	n := simW * simH
	for i := range b.layerPens {
		b.layerPens[i] = make([]uint16, n)
	}
	b.sprPens, b.sprPri, b.sprShadow = make([]uint16, n), make([]uint8, n), make([]uint8, n)
	b.mixPens, b.mixShade = make([]uint16, n), make([]uint8, n)
	b.resampler = arcade.NewResampler(simYMHz/64, float64(b.sampleRate))
	b.pcmBuf = make([]float32, 64)
	b.Reset()
	return b, nil
}

func (b *simpsons) Reset() {
	b.ram, b.palRam, b.sprRam, b.sndRam = [0x2000]uint8{}, [0x1000]uint8{}, [0x2000]uint8{}, [0x800]uint8{}
	b.palette = [2048]uint32{}
	b.k052109.Reset()
	b.k053247.Reset()
	b.k053251.Reset()
	b.ym.Reset()
	b.k053260.Reset()
	b.bank, b.videoBank, b.firqEnabled, b.eepromOut, b.ctrl = 0, 0, false, 0, 0
	b.firqAt, b.vblank = -1, false
	b.cpu.IRQ, b.cpu.FIRQ = false, false
	b.cpu.Reset()
	b.z80.Reset()
	b.z80.IRQ = false
	b.sndBank, b.nmiAt = 0, -1
	b.cyc, b.zCyc, b.ymFrac = 0, 0, 0
	b.testHold = 0
	if allFF(b.eeprom.Bytes[:]) {
		b.testHold = 120
	}
	b.resampler.Clear()
}

func (b *simpsons) SetInput(pads []arcade.Pad) { b.inputs = pads }

func (b *simpsons) pad(p int) arcade.Pad {
	if p < len(b.inputs) {
		return b.inputs[p]
	}
	return nil
}

func (b *simpsons) playerByte(p int) int {
	i := b.pad(p)
	v := 0
	for k, n := range []string{"Left", "Right", "Up", "Down", "Button 1", "Button 2"} {
		if i[n] {
			v |= 1 << k
		}
	}
	if i["Start (1P)"] || (p == 1 && b.pad(0)["Start (2P)"]) {
		v |= 0x80
	}
	return ^v & 0xff
}

func (b *simpsons) coinByte() int {
	v := 0
	for p := 0; p < 4; p++ {
		i := b.pad(p)
		if i["Coin"] || (p == 1 && b.pad(0)["Coin 2"]) {
			v |= 1 << p
		}
		if i["Service"] {
			v |= 0x10 << p
		}
	}
	return ^v & 0xff
}

func (b *simpsons) testByte() int {
	i := b.pad(0)
	v := 0xce | b.eeprom.Read()<<4 | 0x20
	if !((i["Service"] && i["Start (1P)"]) || b.testHold > 0) {
		v |= 1
	}
	return v
}

func (m simMain) Read(addr uint16) uint8 {
	b, a := m.b, int(addr)
	switch {
	case a >= 0x8000:
		return b.prg[b.fixedBase+(a-0x8000)]
	case a >= 0x6000:
		return b.prg[(b.bank*0x2000+(a-0x6000))%len(b.prg)]
	case a >= 0x4000:
		return b.ram[a-0x4000]
	case a < 0x1000:
		if b.videoBank&1 != 0 {
			return b.palRam[a]
		}
		return b.k052109.Read(a)
	case a >= 0x2000:
		if b.videoBank&2 != 0 {
			return b.sprRam[a-0x2000]
		}
		return b.k052109.Read(a)
	case a >= 0x1f80:
		switch a {
		case 0x1f80:
			return uint8(b.coinByte())
		case 0x1f81:
			return uint8(b.testByte())
		case 0x1f90, 0x1f91, 0x1f92, 0x1f93:
			return uint8(b.playerByte(a - 0x1f90))
		case 0x1fc4:
			b.z80.IRQ = true
			return 0
		case 0x1fc6, 0x1fc7:
			return b.k053260.MainRead(a & 1)
		case 0x1fc8, 0x1fc9:
			return b.k053247.RegRead(a & 1)
		}
		return 0xff
	}
	return b.k052109.Read(a)
}

func (m simMain) Write(addr uint16, v uint8) {
	b, a := m.b, int(addr)
	switch {
	case a >= 0x6000:
		return
	case a >= 0x4000:
		b.ram[a-0x4000] = v
		return
	case a < 0x1000:
		if b.videoBank&1 != 0 {
			b.palRam[a] = v
			b.updateColour(a >> 1)
		} else {
			b.k052109.Write(a, v)
		}
		return
	case a >= 0x2000:
		if b.videoBank&2 != 0 {
			b.sprRam[a-0x2000] = v
		} else {
			b.k052109.Write(a, v)
		}
		return
	case a >= 0x1f80:
		if a >= 0x1fa0 && a < 0x1fa8 {
			b.k053247.RegWrite(a&7, v)
			return
		}
		if a >= 0x1fb0 && a < 0x1fc0 {
			b.k053251.Write(a&15, v)
			return
		}
		switch a {
		case 0x1fc0:
			b.ctrl = int(v)
			b.k052109.Rmrd = v&0x08 != 0
		case 0x1fc2:
			if v == 0xff {
				return
			}
			b.eepromOut = int(v)
			b.eeprom.Write(v&0x08 != 0, v&0x10 != 0, v&0x80 != 0)
			b.videoBank = int(v & 3)
			b.firqEnabled = v&4 != 0
		case 0x1fc6, 0x1fc7:
			b.k053260.MainWrite(a&1, v)
		}
		return
	}
	b.k052109.Write(a, v)
}

func (m simMain) SetLines(v uint8) { m.b.bank = int(v & 0x3f) }

func (m simMain) Ack(kind int) {
	switch kind {
	case konami2.AckIRQ:
		m.b.cpu.IRQ = false
	case konami2.AckFIRQ:
		m.b.cpu.FIRQ = false
	}
}

func (b *simpsons) updateColour(i int) {
	v := int(b.palRam[i*2])<<8 | int(b.palRam[i*2+1])
	c5 := func(x int) int { return x<<3 | x>>2 }
	b.palette[i] = arcade.RGB(c5(v&31), c5((v>>5)&31), c5((v>>10)&31))
}

// spriteDma copies the sprite list into the sprite chip (see arcade_simpsons.js).
func (b *simpsons) spriteDma() {
	src, dst := b.sprRam[:], b.k053247.Buffer
	o := 0
	for i := 0; i < 256; i++ {
		s := i * 16
		if src[s]&0x80 != 0 && src[s+1] != 0 {
			copy(dst[o:o+16], src[s:s+16])
			o += 16
		}
	}
	for ; o < 256*16; o += 16 {
		clear(dst[o : o+16])
	}
}

func (s simSound) Read(addr uint16) uint8 {
	b, a := s.b, int(addr)
	switch {
	case a < 0x8000:
		return b.sndRom[a]
	case a < 0xc000:
		return b.sndRom[(b.sndBank*0x4000+(a-0x8000))%len(b.sndRom)]
	case a >= 0xf000 && a < 0xf800:
		return b.sndRam[a-0xf000]
	case a == 0xf801:
		return b.ym.Read()
	case a >= 0xfc00 && a < 0xfc30:
		return b.k053260.Read(a - 0xfc00)
	}
	return 0xff
}

func (s simSound) Write(addr uint16, v uint8) {
	b, a := s.b, int(addr)
	switch {
	case a >= 0xf000 && a < 0xf800:
		b.sndRam[a-0xf000] = v
	case a == 0xf800 || a == 0xf801:
		b.ym.Write(a&1, v)
	case a == 0xfa00:
		b.nmiAt = b.zCyc + simNMIDelay
	case a >= 0xfc00 && a < 0xfc30:
		b.k053260.Write(a-0xfc00, v)
	case a == 0xfe00:
		b.sndBank = int(v & 7)
	}
}

func (s simSound) In(uint16) uint8   { return 0xff }
func (s simSound) Out(uint16, uint8) {}
func (s simSound) Ack()              { s.b.z80.IRQ = false }

func (b *simpsons) runSound(cycles float64) {
	end := b.zCyc + cycles
	for b.zCyc < end {
		if b.nmiAt >= 0 && b.zCyc >= b.nmiAt {
			b.nmiAt = -1
			b.z80.NMI()
		}
		b.zCyc += float64(b.z80.Step())
	}
	b.ymFrac += cycles * (simYMHz / 64) / simZ80Hz
	n := int(math.Floor(b.ymFrac))
	b.ymFrac -= float64(n)
	if n > len(b.pcmBuf) {
		b.pcmBuf = make([]float32, n*2)
	}
	pcm := b.pcmBuf[:n]
	clear(pcm)
	b.k053260.Mix(pcm, 0, n, simYMHz/64)
	for i := 0; i < n; i++ {
		b.ym.Tick()
		b.resampler.Add(float64(b.ym.Out[0]+b.ym.Out[1])/65536 + float64(pcm[i])*simPCMGain)
	}
}

func (b *simpsons) drawFrame() {
	k51, n := b.k053251, simW*simH
	b.spriteBase = k51.PaletteBase(1)
	b.layerBase[0], b.layerBase[1], b.layerBase[2] = k51.PaletteBase(2), k51.PaletteBase(3), k51.PaletteBase(4)
	for l := 0; l < 3; l++ {
		clear(b.layerPens[l])
		b.k052109.Render(l, b.layerPens[l], simW, simH, konami.RenderOpts{X0: simLeft, Y0: simTop})
	}
	clear(b.sprPens)
	clear(b.sprPri)
	clear(b.sprShadow)
	b.k053247.Render(b.sprPens, b.sprPri, b.sprShadow, simW, simH)
	b.mix()
	for i := 0; i < n; i++ {
		c := b.palette[b.mixPens[i]&2047]
		if b.mixShade[i] != 0 {
			c = uint32(simShade[(c>>16)&255])<<16 | uint32(simShade[(c>>8)&255])<<8 | uint32(simShade[c&255])
		}
		b.screen[i] = c
	}
}

func (b *simpsons) mix() {
	k51, n := b.k053251, simW*simH
	sp, spri, ssh := b.sprPens, b.sprPri, b.sprShadow
	l0, l1, l2 := b.layerPens[0], b.layerPens[1], b.layerPens[2]
	p2, p3, p4 := k51.Priority(2), k51.Priority(3), k51.Priority(4)
	bg := uint16(k51.PaletteBase(0) * 16)
	regs := &k51.Regs
	for i := 0; i < n; i++ {
		best, pen, spr := 255, bg, false
		if s := sp[i]; s&15 != 0 {
			best, pen, spr = int(spri[i]), s, true
		}
		if v := l2[i]; v&15 != 0 && p4 < best {
			best, pen, spr = p4, v, false
		}
		if v := l1[i]; v&15 != 0 && p3 < best {
			best, pen, spr = p3, v, false
		}
		if v := l0[i]; v&15 != 0 && p2 < best {
			best, pen, spr = p2, v, false
		}
		b.mixPens[i] = pen
		sh := int(ssh[i])
		b.mixShade[i] = 0
		if sh != 0 && !spr && int(regs[(5+sh)&15]&0x3f) <= best {
			b.mixShade[i] = 1
		}
	}
}

func (b *simpsons) RunFrame() {
	cpu := b.cpu
	if b.testHold > 0 {
		b.testHold--
	}
	for line := 0; line < simLines; line++ {
		if line == simTop+simH {
			b.vblank = true
			if b.k053247.DMAEnabled() {
				b.spriteDma()
				b.firqAt = b.cyc + simFIRQDelay
			}
			if b.k052109.IRQEnabled() {
				cpu.IRQ = true
			}
		} else if line == simTop {
			b.vblank = false
			b.drawFrame()
		}
		end := (line + 1) * simLineCyc
		for b.cyc < end {
			if b.firqAt >= 0 && b.cyc >= b.firqAt {
				b.firqAt = -1
				if b.firqEnabled {
					cpu.FIRQ = true
				}
			}
			b.cyc += cpu.Step()
		}
		b.runSound(simSndCyc)
	}
	b.cyc -= simLines * simLineCyc
	if b.firqAt >= 0 {
		b.firqAt -= simLines * simLineCyc
	}
}

func (b *simpsons) TakeAudio() []float32 { return b.resampler.Take() }
func (b *simpsons) Screen() []uint32     { return b.screen }
func (b *simpsons) SRAM() []byte         { return append([]byte(nil), b.eeprom.Bytes[:]...) }

const simStateMagic = "SIMPS\x01"

func (b *simpsons) SaveState() ([]byte, error) {
	w := &arcade.StateW{}
	w.Bytes([]byte(simStateMagic))
	w.B = b.cpu.AppendState(w.B)
	w.B = b.z80.AppendState(w.B)
	w.Bytes(b.ram[:])
	w.Bytes(b.palRam[:])
	w.Bytes(b.sprRam[:])
	w.Bytes(b.sndRam[:])
	b.k052109.AppendState(w)
	b.k053247.AppendState(w)
	b.k053251.AppendState(w)
	b.k053260.AppendState(w)
	w.B = b.ym.AppendState(w.B)
	b.eeprom.AppendState(w)
	w.Ints([]int{b.bank, b.videoBank, bit(b.firqEnabled), b.eepromOut, b.ctrl, b.firqAt, bit(b.vblank), b.sndBank, b.cyc, b.testHold})
	w.F64s([]float64{b.nmiAt, b.zCyc, b.ymFrac})
	b.resampler.AppendState(w)
	return w.B, nil
}

func (b *simpsons) LoadState(data []byte) (err error) {
	if len(data) < len(simStateMagic) || string(data[:len(simStateMagic)]) != simStateMagic {
		return arcade.ErrBadState
	}
	r := &arcade.StateR{B: data[len(simStateMagic):]}
	if r.B, err = b.cpu.LoadState(r.B); err != nil {
		return err
	}
	if r.B, err = b.z80.LoadState(r.B); err != nil {
		return err
	}
	r.Bytes(b.ram[:])
	r.Bytes(b.palRam[:])
	r.Bytes(b.sprRam[:])
	r.Bytes(b.sndRam[:])
	b.k052109.LoadState(r)
	b.k052109.DX = [3]int{0, -6, -6}
	b.k053247.LoadState(r)
	b.k053251.LoadState(r)
	b.k053260.LoadState(r)
	if r.Err == nil {
		if r.B, err = b.ym.LoadState(r.B); err != nil {
			return err
		}
	}
	b.eeprom.LoadState(r)
	m := make([]int, 10)
	r.Ints(m)
	f := make([]float64, 3)
	r.F64s(f)
	b.resampler.LoadState(r)
	if err := r.Done(); err != nil {
		return err
	}
	b.bank, b.videoBank, b.firqEnabled, b.eepromOut, b.ctrl = m[0], m[1], m[2] != 0, m[3], m[4]
	b.firqAt, b.vblank, b.sndBank, b.cyc, b.testHold = m[5], m[6] != 0, m[7], m[8], m[9]
	b.nmiAt, b.zCyc, b.ymFrac = f[0], f[1], f[2]
	b.k052109.Rmrd = b.ctrl&0x08 != 0
	for i := 0; i < 2048; i++ {
		b.updateColour(i)
	}
	b.resampler.Clear()
	b.drawFrame()
	return nil
}

func simpsonsGames() []arcade.Game {
	shared := []arcade.File{
		{Name: "072-e03.6g", Size: 0x20000, Role: "sound", Offset: arcade.Next},
		{Name: "072-b07.18h", Size: 0x80000, Role: "tiles", Lane: 0},
		{Name: "072-b06.16h", Size: 0x80000, Role: "tiles", Lane: 2},
		{Name: "072-b08.3n", Size: 0x100000, Role: "sprites", Lane: 0},
		{Name: "072-b09.8n", Size: 0x100000, Role: "sprites", Lane: 2},
		{Name: "072-b10.12n", Size: 0x100000, Role: "sprites", Lane: 4},
		{Name: "072-b11.16l", Size: 0x100000, Role: "sprites", Lane: 6},
		{Name: "072-d05.1f", Size: 0x100000, Role: "pcm", Offset: 0},
		{Name: "072-d04.1d", Size: 0x40000, Role: "pcm", Offset: 0x100000},
	}
	version := func(id, title string, players int, prg []string, nv string) arcade.Game {
		parent := "simpsons"
		if id == "simpsons" {
			parent = ""
		}
		var files []arcade.File
		for i, name := range prg {
			files = append(files, arcade.File{Name: name, Size: 0x20000, Role: "cpu", Offset: i * 0x20000})
		}
		files = append(files, shared...)
		files = append(files, arcade.File{Name: nv, Size: 0x80, Role: "eeprom", Offset: arcade.Next, Optional: true})
		return arcade.Game{ID: id, Title: title, Year: 1991, Maker: "Konami", Players: players, Loose: true, Parent: parent, Files: files}
	}
	return []arcade.Game{
		version("simpsons", "The Simpsons (4 players, World)", 4, []string{"072-g02.16c", "072-g01.17c", "072-j13.13c", "072-j12.15c"}, "simpsons.12c.nv"),
		version("simpsons4pe", "The Simpsons (4 players, older)", 4, []string{"072-g02.16c", "072-g01.17c", "072-m13.13c", "072-l12.15c"}, "simpsons4pe.12c.nv"),
		version("simpsons2p", "The Simpsons (2 players, World)", 2, []string{"072-g02.16c", "072-p01.17c", "072-013.13c", "072-012.15c"}, "simpsons2p.12c.nv"),
		version("simpsons2pa", "The Simpsons (2 players, Asia)", 2, []string{"072-g02.16c", "072-p01.17c", "072-013.13c", "072-012.15c"}, "simpsons2pa.12c.nv"),
		version("simpsons2pj", "The Simpsons (2 players, Japan)", 2, []string{"072-s02.16c", "072-t01.17c", "072-213.13c", "072-212.15c"}, "simpsons2pj.12c.nv"),
	}
}

func init() {
	arcade.Register(&arcade.BoardDef{
		ID: "simpsons", Title: "Konami The Simpsons", Width: simW, Height: simH, Rotate: 0, FPS: simFPS, Players: 4,
		Characters: []string{"Marge", "Homer", "Bart", "Lisa"},
		Buttons:    []string{"Up", "Down", "Left", "Right", "Button 1", "Button 2", "Coin", "Start (1P)", "Start (2P)", "Service"},
		Dips:       []arcade.Dip{}, Games: simpsonsGames(), New: newSimpsons,
	})
}
