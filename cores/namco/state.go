package namco

import "retropixels/cores/arcade"

func (b *board) SaveState() ([]byte, error) {
	w := &arcade.StateW{}
	for _, c := range b.cpu {
		w.B = c.AppendState(w.B)
	}
	w.Bytes(b.vram[:])
	for i := range b.ram {
		w.Bytes(b.ram[i][:])
	}
	w.Ints(b.cyc[:])
	w.Bool(b.irq[0])
	w.Bool(b.irq[1])
	w.Bytes(b.latch[:])
	w.Bytes(b.vlat[:])
	w.Ints([]int{b.wdog, b.t, b.io.nextNMI, b.eaAddr})
	w.U8(b.io.ctl)
	w.Ints(b.io.idx[:])
	b.n51.save(w)
	b.n54.save(w)
	b.snd.save(w)
	w.Bytes(b.earom[:])
	w.U8(b.eaData)
	w.U8(b.eaCtl)
	w.Int(b.vid.stars.scroll)
	b.out.AppendState(w)
	return w.B, nil
}

func (b *board) LoadState(s []byte) error {
	rest := s
	var err error
	for _, c := range b.cpu {
		if rest, err = c.LoadState(rest); err != nil {
			return arcade.ErrBadState
		}
	}
	r := &arcade.StateR{B: rest}
	r.Bytes(b.vram[:])
	for i := range b.ram {
		r.Bytes(b.ram[i][:])
	}
	r.Ints(b.cyc[:])
	b.irq[0], b.irq[1] = r.Bool(), r.Bool()
	r.Bytes(b.latch[:])
	r.Bytes(b.vlat[:])
	v := make([]int, 4)
	r.Ints(v)
	b.wdog, b.t, b.io.nextNMI, b.eaAddr = v[0], v[1], v[2], v[3]
	b.io.ctl = r.U8()
	r.Ints(b.io.idx[:])
	b.n51.load(r)
	b.n54.load(r)
	b.snd.load(r)
	r.Bytes(b.earom[:])
	b.eaData = r.U8()
	b.eaCtl = r.U8()
	b.vid.stars.scroll = r.Int()
	b.out.LoadState(r)
	b.out.Clear()
	if err := r.Done(); err != nil {
		return err
	}
	b.vid.render(b)
	return nil
}
