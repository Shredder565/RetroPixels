package mario

import "retropixels/cores/arcade"

func (b *board) SaveState() ([]byte, error) {
	w := &arcade.StateW{}
	w.B = b.cpu.AppendState(w.B)
	w.B = b.snd.AppendState(w.B)
	w.Bytes(b.ram[:])
	w.Bytes(b.spriteRAM[:])
	w.Bytes(b.vram[:])
	w.Bytes(b.ctrl[:])
	w.Bytes([]uint8{b.scroll, b.latch, b.p1Latch, b.p2Latch, b.tLatch})
	w.Bool(b.sndIRQ)
	w.Bool(b.nmiLine)
	w.Bool(b.skid)
	w.Ints([]int{b.stall, b.cyc, b.frame, b.dac, int(b.noise)})
	w.F64s([]float64{b.sndCyc, b.lp, b.hp, b.hpIn, b.walk[0], b.walk[1], b.walkPh[0], b.walkPh[1], b.skidEnv, b.skidPh})
	d := &b.dma
	w.Ints(d.wr[:])
	w.Ints([]int{d.addrA, d.addrB, d.length, d.stepA, d.stepB, d.curA, d.curB, len(d.follow)})
	w.Ints(d.follow)
	w.Bool(d.enabled)
	w.Bool(d.srcA)
	b.out.AppendState(w)
	return w.B, nil
}

func (b *board) LoadState(st []byte) error {
	rest, err := b.cpu.LoadState(st)
	if err != nil {
		return err
	}
	if rest, err = b.snd.LoadState(rest); err != nil {
		return err
	}
	r := &arcade.StateR{B: rest}
	r.Bytes(b.ram[:])
	r.Bytes(b.spriteRAM[:])
	r.Bytes(b.vram[:])
	r.Bytes(b.ctrl[:])
	v := make([]uint8, 5)
	r.Bytes(v)
	b.scroll, b.latch, b.p1Latch, b.p2Latch, b.tLatch = v[0], v[1], v[2], v[3], v[4]
	b.sndIRQ = r.Bool()
	b.nmiLine = r.Bool()
	b.skid = r.Bool()
	m := make([]int, 5)
	r.Ints(m)
	b.stall, b.cyc, b.frame, b.dac, b.noise = m[0], m[1], m[2], m[3], uint32(m[4])
	f := make([]float64, 10)
	r.F64s(f)
	b.sndCyc, b.lp, b.hp, b.hpIn = f[0], f[1], f[2], f[3]
	b.walk, b.walkPh, b.skidEnv, b.skidPh = [2]float64{f[4], f[5]}, [2]float64{f[6], f[7]}, f[8], f[9]
	d := &b.dma
	r.Ints(d.wr[:])
	m = make([]int, 8)
	r.Ints(m)
	d.addrA, d.addrB, d.length, d.stepA, d.stepB, d.curA, d.curB = m[0], m[1], m[2], m[3], m[4], m[5], m[6]
	if m[7] < 0 || m[7] > 16 {
		return arcade.ErrBadState
	}
	d.follow = make([]int, m[7])
	r.Ints(d.follow)
	d.enabled = r.Bool()
	d.srcA = r.Bool()
	b.out.LoadState(r)
	b.out.Clear()
	if err := r.Done(); err != nil {
		return err
	}
	b.drawFrame()
	return nil
}

func opt(label string, v int) arcade.DipOption { return arcade.DipOption{Label: label, Value: v} }

func init() {
	arcade.Register(&arcade.BoardDef{
		ID: "mario", Title: "Nintendo Mario Bros.", Width: scrW, Height: scrH, FPS: FPS,
		Buttons: []string{"Left", "Right", "Jump", "Coin", "Start (1P)", "Start (2P)", "Coin 2", "Service"},
		Dips: []arcade.Dip{
			{ID: "lives", Label: "Lives", Default: 0, Options: []arcade.DipOption{opt("3", 0), opt("4", 1), opt("5", 2), opt("6", 3)}},
			{ID: "coins", Label: "Coins", Default: 0, Options: []arcade.DipOption{
				opt("1 coin, 1 game", 0), opt("2 coins, 1 game", 1), opt("1 coin, 2 games", 2), opt("1 coin, 3 games", 3)}},
			{ID: "bonus", Label: "Extra life at", Default: 0, Options: []arcade.DipOption{
				opt("20,000 points", 0), opt("30,000 points", 1), opt("40,000 points", 2), opt("No extra life", 3)}},
			{ID: "difficulty", Label: "Difficulty", Default: 0, Options: []arcade.DipOption{
				opt("Easy", 0), opt("Medium", 2), opt("Hard", 1), opt("Hardest", 3)}},
		},
		Games: []arcade.Game{
			{ID: "mario", Title: "Mario Bros.", Year: 1983, Maker: "Nintendo of America", Files: []arcade.File{
				{Name: "tma1-c-7f_g.7f", Alt: []string{"mario.7f", "tma1-c-7f_e-1.7f", "tma1-c-7f_f.7f"}, Size: 0x2000, Role: "cpu", Offset: 0x0000},
				{Name: "tma1-c-7e_g.7e", Alt: []string{"mario.7e", "tma1-c-7e_e-3.7e", "tma1-c-7e_f.7e"}, Size: 0x2000, Role: "cpu", Offset: 0x2000},
				{Name: "tma1-c-7d_g.7d", Alt: []string{"mario.7d", "tma1-c-7d_e-1.7d", "tma1-c-7d_f.7d"}, Size: 0x2000, Role: "cpu", Offset: 0x4000},
				{Name: "tma1-c-7c_g.7c", Alt: []string{"mario.7c", "tma1-c-7c_e-3.7c", "tma1-c-7c_f.7c"}, Size: 0x1000, Role: "cpu", Offset: 0x6000},
				{Name: "tma1-c-6k_e.6k", Alt: []string{"tma1c-a.6k"}, Size: 0x1000, Role: "sound", Offset: 0},
				{Name: "tma1-v-3f.3f", Alt: []string{"mario.3f"}, Size: 0x1000, Role: "tiles", Offset: 0x0000},
				{Name: "tma1-v-3j.3j", Alt: []string{"mario.3j"}, Size: 0x1000, Role: "tiles", Offset: 0x1000},
				{Name: "tma1-v-7m.7m", Alt: []string{"mario.7m", "tma1-v.7m.7m"}, Size: 0x1000, Role: "sprites", Offset: 0x0000},
				{Name: "tma1-v-7n.7n", Alt: []string{"mario.7n"}, Size: 0x1000, Role: "sprites", Offset: 0x1000},
				{Name: "tma1-v-7p.7p", Alt: []string{"mario.7p"}, Size: 0x1000, Role: "sprites", Offset: 0x2000},
				{Name: "tma1-v-7s.7s", Alt: []string{"mario.7s"}, Size: 0x1000, Role: "sprites", Offset: 0x3000},
				{Name: "tma1-v-7t.7t", Alt: []string{"mario.7t"}, Size: 0x1000, Role: "sprites", Offset: 0x4000},
				{Name: "tma1-v-7u.7u", Alt: []string{"mario.7u"}, Size: 0x1000, Role: "sprites", Offset: 0x5000},
				{Name: "tma1-c-4p.4p", Alt: []string{"mario.4p", "tma1-c-4p_1.4p"}, Size: 0x200, Role: "palette", Offset: 0},
				// the address decoder (the memory map is built in)
				{Name: "tma1-c-5p.5p", Size: 0x20, Role: "decoder", Offset: 0, Optional: true},
			}},
		},
		New: newBoard,
	})
}
