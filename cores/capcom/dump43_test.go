package capcom

func (b *b1943) loadDump(d *mameDump) {
	for a, v := range d.ranges {
		for i, x := range v {
			b.write(uint16(a+i), x)
		}
	}
	for a, v := range d.regs {
		b.write(uint16(a), v)
	}
	b.render()
}

func (b *b1943) peek(a uint16) uint8    { return b.read(a) }
func (b *b1943) setRenderHook(f func()) { b.onRender = f }
