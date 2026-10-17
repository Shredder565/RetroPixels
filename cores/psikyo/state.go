package psikyo

import "retropixels/cores/arcade"

const stateMagic = "PSKY\x01"

func (b *board) SaveState() ([]byte, error) {
	w := &arcade.StateW{}
	w.Bytes([]byte(stateMagic))
	w.B = b.cpu.AppendState(w.B)
	w.B = b.ymf.AppendState(w.B)
	v := b.vid
	w.Bytes(b.ram)
	w.Bytes(v.spr[:])
	w.Bytes(v.pal[:0x5000])
	w.Bytes(v.zoom[:])
	w.Bytes(v.regs[:])
	b.ee.save(w)
	w.Bool(b.irq)
	w.Int(int(b.ymfT))
	b.out.AppendState(w)
	return w.B, nil
}

func (b *board) LoadState(data []byte) (err error) {
	if len(data) < len(stateMagic) || string(data[:len(stateMagic)]) != stateMagic {
		return arcade.ErrBadState
	}
	r := &arcade.StateR{B: data[len(stateMagic):]}
	if r.B, err = b.cpu.LoadState(r.B); err != nil {
		return err
	}
	if r.B, err = b.ymf.LoadState(r.B); err != nil {
		return err
	}
	v := b.vid
	r.Bytes(b.ram)
	r.Bytes(v.spr[:])
	r.Bytes(v.pal[:0x5000])
	r.Bytes(v.zoom[:])
	r.Bytes(v.regs[:])
	b.ee.load(r)
	b.irq = r.Bool()
	b.ymfT = int64(r.Int())
	b.out.LoadState(r)
	if err := r.Done(); err != nil {
		return err
	}
	b.out.Clear()
	v.render()
	return nil
}

func (e *eeprom) save(w *arcade.StateW) {
	w.Bytes(e.mem[:])
	for _, x := range []bool{e.cs, e.clk, e.di, e.do, e.writeOK} {
		w.Bool(x)
	}
	w.Ints([]int{e.state, e.bits, int(e.sr), e.op, e.addr, int(e.out), e.outBits})
}

func (e *eeprom) load(r *arcade.StateR) {
	r.Bytes(e.mem[:])
	for _, x := range []*bool{&e.cs, &e.clk, &e.di, &e.do, &e.writeOK} {
		*x = r.Bool()
	}
	n := make([]int, 7)
	r.Ints(n)
	e.state, e.bits, e.sr, e.op, e.addr, e.out, e.outBits = n[0], n[1], uint32(n[2]), n[3], n[4], uint8(n[5]), n[6]
}
