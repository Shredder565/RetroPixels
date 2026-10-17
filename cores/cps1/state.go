package cps1

import "retropixels/cores/arcade"

const stateMagic = "CPS1\x01"

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
	w.Bytes(v.gfxram[:])
	w.U16s(v.cpsa[:])
	w.U16s(v.cpsb[:])
	w.U16s(v.palRaw[:])
	w.Bytes(v.obj[:])
	w.U8(b.latch)
	w.U8(b.fade)
	w.Int(b.zbank)
	w.F64(b.zcyc)
	w.Int(b.ymClk)
	w.Int(b.okiAcc)
	w.F64(b.okiOut)
	w.Bool(b.z80.IRQ)
	w.Bool(v.palPending)
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
	r.Bytes(v.gfxram[:])
	r.U16s(v.cpsa[:])
	r.U16s(v.cpsb[:])
	r.U16s(v.palRaw[:])
	r.Bytes(v.obj[:])
	b.latch = r.U8()
	b.fade = r.U8()
	b.zbank = r.Int()
	b.zcyc = r.F64()
	b.ymClk = r.Int()
	b.okiAcc = r.Int()
	b.okiOut = r.F64()
	b.z80.IRQ = r.Bool()
	v.palPending = r.Bool()
	b.frame = int64(r.U32())
	b.out.LoadState(r)
	if err := r.Done(); err != nil {
		return err
	}
	for i, c := range v.palRaw {
		v.pal[i] = colour(c)
	}
	b.out.Clear()
	v.render()
	return nil
}
