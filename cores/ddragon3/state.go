package ddragon3

import "retropixels/cores/arcade"

const stateMagic = "DD3\x01"

func (b *board) SaveState() ([]byte, error) {
	w := &arcade.StateW{}
	w.Bytes([]byte(stateMagic))
	w.B = b.cpu.AppendState(w.B)
	w.B = b.z80.AppendState(w.B)
	w.B = b.ym.AppendState(w.B)
	w.B = b.oki.AppendState(w.B)
	v := b.vid
	w.Bytes(b.wram[:])
	w.Bytes(b.zram[:])
	w.Bytes(v.fgram[:])
	w.Bytes(v.bgram[:])
	w.Bytes(v.palram[:])
	w.Bytes(v.spram[:])
	w.Bytes(v.spbuf[:])
	w.Bytes(v.regs[:])
	w.Bytes(v.prio[:])
	w.Int(b.line)
	w.Bool(b.irq5)
	w.Bool(b.irq6)
	w.U8(b.latch)
	w.Int(b.okiBank)
	w.F64(b.zcyc)
	w.Int(b.ymClk)
	w.Int(b.okiAcc)
	w.F64(b.okiOut)
	w.U32(uint32(b.frame))
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
	if r.B, err = b.z80.LoadState(r.B); err != nil {
		return err
	}
	if r.B, err = b.ym.LoadState(r.B); err != nil {
		return err
	}
	if r.B, err = b.oki.LoadState(r.B); err != nil {
		return err
	}
	v := b.vid
	r.Bytes(b.wram[:])
	r.Bytes(b.zram[:])
	r.Bytes(v.fgram[:])
	r.Bytes(v.bgram[:])
	r.Bytes(v.palram[:])
	r.Bytes(v.spram[:])
	r.Bytes(v.spbuf[:])
	r.Bytes(v.regs[:])
	r.Bytes(v.prio[:])
	b.line = r.Int()
	b.irq5 = r.Bool()
	b.irq6 = r.Bool()
	b.latch = r.U8()
	bank := r.Int()
	b.zcyc = r.F64()
	b.ymClk = r.Int()
	b.okiAcc = r.Int()
	b.okiOut = r.F64()
	b.frame = int64(r.U32())
	b.out.LoadState(r)
	if err := r.Done(); err != nil {
		return err
	}
	b.line %= lines
	b.setOkiBank(bank)
	b.setIRQ()
	for i := range v.pal {
		v.updateColour(i)
	}
	b.out.Clear()
	v.render()
	return nil
}
