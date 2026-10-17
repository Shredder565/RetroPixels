package wiiu

// The hook between Latte and a host GPU backend. Guest memory stays the
// truth: the backend keeps copies of surfaces on the host, writes them back
// when anything else reads that memory, and drops its copies when anything
// else writes it.

// hwRenderer is a host GPU backend.
type hwRenderer interface {
	// draw draws on the host; ids are the vertex ids after the index
	// offset (^0 for restarts). False leaves the draw to the software
	// renderer.
	draw(st *drawState, prim uint32, ids []uint32, insts uint32) bool
	// flush writes host copies of memory in [lo, hi) back to the guest.
	flush(lo, hi uint32)
	// invalidate drops host copies of memory in [lo, hi): something else
	// wrote it.
	invalidate(lo, hi uint32)
	// flushAll writes every host copy back.
	flushAll()
	// clearColor and clearDepth clear a surface the backend holds, and
	// report whether it did.
	clearColor(cb *cbDesc, c [4]float32) bool
	clearDepth(db *dbDesc, z float32, stencil, flags uint32) bool
	close()
}

// hwFlush and hwInvalidate are no-ops without a backend.
func (g *Latte) hwFlush(lo, hi uint32) {
	if g.hw != nil {
		g.hw.flush(lo, hi)
	}
}

func (g *Latte) hwInvalidate(lo, hi uint32) {
	if g.hw != nil {
		g.hw.invalidate(lo, hi)
	}
}

// surfRange is the memory one slice of a surface covers.
func surfRange(lay *surfLayout, slice uint32) (uint32, uint32) {
	n := lay.sliceBytes()
	return lay.Base + n*slice, lay.Base + n*(slice+1)
}

// hwDraw offers a draw to the backend; it reports whether the backend
// drew it.
func (g *Latte) hwDraw(st *drawState, count uint32, index func(uint32) uint32) bool {
	prim := g.reg(regVGT_PRIMITIVE_TYPE)
	restartOn := g.reg(regVGT_MULTI_PRIM_IB_RESET_EN)&1 != 0
	restart := g.reg(regVGT_MULTI_PRIM_IB_RESET_INDX)
	idxOff := g.reg(regVGT_INDX_OFFSET)
	ids := make([]uint32, count)
	for i := range ids {
		id := index(uint32(i))
		if restartOn && id == restart {
			ids[i] = ^uint32(0)
			continue
		}
		ids[i] = id + idxOff
	}
	if !g.hw.draw(st, prim, ids, max(g.numInstances, 1)) {
		return false
	}
	// what the draw wrote isn't in the software texture cache any more
	for t := 0; t < 8; t++ {
		if st.cbOn[t] {
			g.tex.invalidate(surfRange(&st.cbs[t].lay, st.cbs[t].slice))
		}
	}
	if st.db.ok && st.dbCtl&5 != 0 {
		g.tex.invalidate(surfRange(&st.db.lay, st.db.slice))
	}
	return true
}
