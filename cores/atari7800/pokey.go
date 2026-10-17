package atari7800

import pk "retropixels/cores/pokey"

// POKEY sound (4 channels), clocked at the processor rate (1.79 MHz): the
// shared chip in cores/pokey.

type pokey struct{ pk.Chip }

func newPOKEY() *pokey { return &pokey{*pk.New()} }

func (p *pokey) write(r int, v uint8)        { p.Write(r, v) }
func (p *pokey) read(r int, now int64) uint8 { return p.Read(r, now) }
func (p *pokey) run(n int) float64           { return p.Run(n) }
func (p *pokey) save(w *stateW)              { w.b = p.AppendState(w.b) }
func (p *pokey) load(r *stateR) {
	if r.err != nil {
		return
	}
	rest, err := p.LoadState(r.b)
	if err != nil {
		r.err = errBadState
		return
	}
	r.b = rest
}
