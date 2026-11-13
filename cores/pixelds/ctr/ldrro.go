package ctr

// ldr:ro, the service that loads code modules. Many 3DS games keep part
// of their code in CRO files in RomFS and load them while running; the
// game's own code (a CRS file describing the main program's symbols)
// is registered first. Loading a module maps the file the game read into
// a buffer at the address the game picked, moves the file's offsets to
// that address, applies its relocations and links its imports with the
// exports of the modules already loaded, in both directions.
//
// A module file is a 0x80-byte hash, then a header of 46 words: the
// magic ("CRO0", or "FIXD" once the module's linking data has been cut
// off), the list links that chain loaded modules together (kept in the
// game's memory, where the game's own code walks them), then an offset
// and a count for each table. Places in a module are "segment tags":
// segment index in the low 4 bits, offset in that segment above them.

const (
	croHashSize   = 0x80
	croHeaderSize = 0x138

	croMagic = 0x304f5243 // "CRO0"
	croFixed = 0x44584946 // "FIXD"

	rcROAlreadyInit  = 0xd9612ff9
	rcRONotInit      = 0xd9612ff8
	rcROTooSmall     = 0xe0e12c1f
	rcROMisaligned   = 0xd9012ff1
	rcROMisalignSize = 0xd9012ff2
	rcRONotLoaded    = 0xd8a12c0d
	rcROFormat       = 0xd9012c11
)

// header words
const (
	hMagic = iota
	hNameOff
	hNext
	hPrev
	hFileSize
	hBssSize
	hFixedSize
	_
	_
	hOnLoad
	hOnExit
	hOnUnresolved
	hCode // the first of the (offset, count) pairs
)

// the (offset, count) pairs, as header word indexes
const (
	tCode        = 12
	tData        = 14
	tModName     = 16
	tSegments    = 18
	tExpNamed    = 20
	tExpIndexed  = 22
	tExpStrings  = 24
	tExpTree     = 26
	tImpModules  = 28
	tExtRelocs   = 30
	tImpNamed    = 32
	tImpIndexed  = 34
	tImpAnon     = 36
	tImpStrings  = 38
	tStaticAnon  = 40
	tIntRelocs   = 42
	tStaticReloc = 44
	tEnd         = 46
)

// the size of one entry of each table, in pair order from tCode
var croEntrySize = [17]uint32{1, 1, 1, 12, 8, 4, 1, 8, 20, 12, 8, 8, 8, 1, 8, 12, 12}

// where each fix level cuts the tables off
var croFixFrom = [4]int{tEnd, tStaticAnon, tImpModules, tExpNamed}

const (
	segCode = iota
	segROData
	segData
	segBSS
)

type croErr uint32

// cro is a module (or the CRS) in the game's memory at base.
type cro struct {
	m    *Machine
	base uint32
}

func (c cro) word(i int) uint32       { return c.m.mem.Read32(c.base + croHashSize + 4*uint32(i)) }
func (c cro) setWord(i int, v uint32) { c.m.mem.Write32(c.base+croHashSize+4*uint32(i), v) }
func (c cro) next() cro               { return cro{c.m, c.word(hNext)} }
func (c cro) prev() cro               { return cro{c.m, c.word(hPrev)} }

// entry is the address of entry i of table t.
func (c cro) entry(t int, i uint32) uint32 {
	return c.word(t) + i*croEntrySize[(t-tCode)/2]
}

func (c cro) count(t int) uint32 { return c.word(t + 1) }

func (c cro) str(a, max uint32) string { return c.m.mem.readString(a, max) }

func (c cro) moduleName() string {
	return c.str(c.word(tModName), c.count(tModName))
}

// addr turns a segment tag into an address (0 when it's outside the
// module's segments).
func (c cro) addr(tag uint32) uint32 {
	idx, off := tag&15, tag>>4
	if idx >= c.count(tSegments) {
		return 0
	}
	e := c.entry(tSegments, idx)
	if off >= c.m.mem.Read32(e+4) {
		return 0
	}
	return c.m.mem.Read32(e) + off
}

func (c cro) segType(idx uint32) uint32 { return c.m.mem.Read32(c.entry(tSegments, idx) + 8) }

// relocate writes one relocation at target: the symbol's address plus the
// addend, or that less where the word will end up (future) for relative
// ones.
func (c cro) relocate(target uint32, typ uint8, addend, sym, future uint32) {
	mem := c.m.mem
	v := sym + addend
	switch typ {
	case 0:
	case 2, 38: // absolute
		mem.Write32(target, v)
	case 3: // relative
		mem.Write32(target, v-future)
	case 28, 29: // ARM B/BL: a word offset from 8 bytes on
		off := (v - future - 8) >> 2
		mem.Write32(target, mem.Read32(target)&0xff000000|off&0xffffff)
	case 42: // relative, a multiple of 4
		mem.Write32(target, (v-future)&^3)
	default:
		c.m.warnOnce("cro reloc", "code modules: relocation type %d isn't handled", typ)
	}
}

// clearRelocation zeroes a relocated word (on unloading).
func (c cro) clearRelocation(target uint32, typ uint8) {
	switch typ {
	case 2, 3, 38, 42:
		c.m.mem.Write32(target, 0)
	}
}

// A relocation entry (12 bytes): segment tag of the place to patch, type,
// end-of-batch flag, resolved flag (on a batch's first entry), addend.
type croReloc struct {
	tag              uint32
	typ, end, solved uint8
	addend           uint32
}

func (c cro) reloc(a uint32) croReloc {
	mem := c.m.mem
	return croReloc{mem.Read32(a), uint8(mem.Read8(a + 4)), uint8(mem.Read8(a + 5)), uint8(mem.Read8(a + 6)), mem.Read32(a + 8)}
}

// applyBatch points a batch of relocations (in this module) at sym, and
// marks it resolved (or not, when reset).
func (c cro) applyBatch(batch, sym uint32, reset bool) error {
	if sym == 0 && !reset {
		return croErr(rcROFormat)
	}
	for a := batch; ; a += 12 {
		r := c.reloc(a)
		t := c.addr(r.tag)
		if t == 0 {
			return croErr(rcROFormat)
		}
		c.relocate(t, r.typ, r.addend, sym, t)
		if r.end != 0 {
			break
		}
	}
	solved := uint32(1)
	if reset {
		solved = 0
	}
	c.m.mem.Write8(batch+6, solved)
	return nil
}

func (e croErr) Error() string { return "code module error" }

// findExport looks a named symbol up in the module's export tree: a
// binary tree over the names' bits, each node naming the bit to test and
// its two children (the top bit of a child marks a leaf, which gives the
// export's index).
func (c cro) findExport(name string) uint32 {
	n := c.count(tExpTree)
	if n == 0 {
		return 0
	}
	mem := c.m.mem
	node := func(i uint32) uint32 { return c.word(tExpTree) + 8*(i&0x7fff) }
	child := mem.Read16(node(0) + 2)
	for steps := uint32(0); ; steps++ {
		if steps > n+1 {
			return 0 // a loop: a broken tree
		}
		e := node(child)
		if child&0x8000 != 0 {
			idx := mem.Read16(e + 6)
			if idx >= c.count(tExpNamed) {
				return 0
			}
			ex := c.entry(tExpNamed, idx)
			if c.str(mem.Read32(ex), c.count(tExpStrings)) != name {
				return 0
			}
			return c.addr(mem.Read32(ex + 4))
		}
		bit := mem.Read16(e)
		if by := int(bit >> 3); by < len(name) && name[by]>>(bit&7)&1 != 0 {
			child = mem.Read16(e + 4)
		} else {
			child = mem.Read16(e + 2)
		}
	}
}

// eachLinked calls f for the CRS and then each auto-link module, until f
// returns false.
func eachLinked(crs cro, f func(cro) bool) {
	seen := 0
	for c := crs; c.base != 0 && seen < 4096; c = c.next() {
		if !f(c) {
			return
		}
		seen++
	}
}

// rebase moves the module's offsets to its address and places .data and
// .bss in the game's buffers; then it applies the module's own
// relocations. For the CRS only the header and tables are moved.
func (c cro) rebase(crs cro, size, dataAddr, dataSize, bssAddr, bssSize uint32, isCRS bool) error {
	mem := c.m.mem
	bad := croErr(rcROFormat)
	if c.word(hMagic) != croMagic || c.word(hNext) != 0 || c.word(hPrev) != 0 || c.word(hFixedSize) != 0 {
		return bad
	}
	if c.word(hFileSize) > 0x10000000 || c.word(hBssSize) > 0x10000000 || c.word(tCode) < croHeaderSize {
		return bad
	}
	// the tables come in this order in the file
	order := []int{tCode, tModName, tSegments, tExpNamed, tExpTree, tExpIndexed, tExpStrings,
		tImpModules, tExtRelocs, tImpNamed, tImpIndexed, tImpAnon, tImpStrings,
		tStaticAnon, tIntRelocs, tStaticReloc, tData, hFileSize}
	for i := 1; i < len(order); i++ {
		if c.word(order[i]) < c.word(order[i-1]) {
			return bad
		}
	}
	move := func(a uint32) uint32 {
		if v := mem.Read32(a); v != 0 {
			v += c.base
			mem.Write32(a, v)
			return v
		}
		return 0
	}
	move(c.base + croHashSize + 4*hNameOff)
	for t := tCode; t < tEnd; t += 2 {
		move(c.base + croHashSize + 4*uint32(t))
	}
	end := c.base + size
	for t := tCode; t < tEnd; t += 2 {
		if c.word(t)+c.count(t)*croEntrySize[(t-tCode)/2] > end {
			return bad
		}
	}
	strOK := func(t int) bool { return c.count(t) == 0 || mem.Read8(c.word(t)+c.count(t)-1) == 0 }
	if !strOK(tModName) {
		return bad
	}

	oldData := uint32(0)
	if !isCRS {
		for i := uint32(0); i < c.count(tSegments); i++ {
			e := c.entry(tSegments, i)
			off, sz, typ := mem.Read32(e), mem.Read32(e+4), mem.Read32(e+8)
			switch {
			case typ == segData && sz != 0:
				if sz > dataSize {
					return croErr(rcROTooSmall)
				}
				oldData = off
				mem.Write32(e, dataAddr)
			case typ == segBSS && sz != 0:
				if sz > bssSize {
					return croErr(rcROTooSmall)
				}
				mem.Write32(e, bssAddr)
			case typ != segData && typ != segBSS && off != 0:
				if off+c.base > end {
					return bad
				}
				mem.Write32(e, off+c.base)
			}
		}
		oldData += c.base
	}

	inside := func(a uint32, t int) bool {
		return a >= c.word(t) && a <= c.word(t)+c.count(t)*croEntrySize[(t-tCode)/2]
	}
	for i := uint32(0); i < c.count(tExpNamed); i++ {
		if a := move(c.entry(tExpNamed, i)); a != 0 && !inside(a, tExpStrings) {
			return bad
		}
	}
	for i := uint32(0); i < c.count(tExpTree); i++ {
		e := c.entry(tExpTree, i)
		if mem.Read16(e+2)&0x7fff >= c.count(tExpTree) || mem.Read16(e+4)&0x7fff >= c.count(tExpTree) {
			return bad
		}
	}
	if !strOK(tExpStrings) {
		return bad
	}
	for i := uint32(0); i < c.count(tImpModules); i++ {
		e := c.entry(tImpModules, i)
		if a := move(e); a != 0 && !inside(a, tImpStrings) {
			return bad
		}
		if a := move(e + 4); a != 0 && !inside(a, tImpIndexed) {
			return bad
		}
		if a := move(e + 12); a != 0 && !inside(a, tImpAnon) {
			return bad
		}
	}
	// every external relocation starts unresolved
	if n := c.count(tExtRelocs); n > 0 {
		if c.reloc(c.entry(tExtRelocs, n-1)).end == 0 {
			return bad
		}
		unresolved := c.addr(c.word(hOnUnresolved))
		first := true
		for i := uint32(0); i < n; i++ {
			a := c.entry(tExtRelocs, i)
			r := c.reloc(a)
			t := c.addr(r.tag)
			if t == 0 {
				return bad
			}
			c.relocate(t, r.typ, r.addend, unresolved, t)
			if first {
				mem.Write8(a+6, 0)
			}
			first = r.end != 0
		}
	}
	for i := uint32(0); i < c.count(tImpNamed); i++ {
		e := c.entry(tImpNamed, i)
		if a := move(e); a != 0 && !inside(a, tImpStrings) {
			return bad
		}
		if a := move(e + 4); a != 0 && !inside(a, tExtRelocs) {
			return bad
		}
	}
	for _, t := range []int{tImpIndexed, tImpAnon} {
		for i := uint32(0); i < c.count(t); i++ {
			if a := move(c.entry(t, i) + 4); a != 0 && !inside(a, tExtRelocs) {
				return bad
			}
		}
	}
	if !strOK(tImpStrings) {
		return bad
	}

	// symbols this module gives the CRS by position
	if !isCRS {
		for i := uint32(0); i < c.count(tStaticAnon); i++ {
			e := c.entry(tStaticAnon, i)
			batch := mem.Read32(e+4) + c.base
			if !inside(batch, tStaticReloc) {
				return bad
			}
			if err := crs.applyBatch(batch, c.addr(mem.Read32(e)), false); err != nil {
				return err
			}
		}
	}

	// the module's own relocations; ones into .data are made in the
	// file's copy of it, which the game then copies to its buffer
	for i := uint32(0); i < c.count(tIntRelocs); i++ {
		e := c.entry(tIntRelocs, i)
		tag, typ, symSeg, addend := mem.Read32(e), uint8(mem.Read8(e+4)), mem.Read8(e+5), mem.Read32(e+8)
		future := c.addr(tag)
		if future == 0 || symSeg >= c.count(tSegments) {
			return bad
		}
		target := future
		if c.segType(tag&15) == segData {
			target = oldData + tag>>4
		}
		c.relocate(target, typ, addend, mem.Read32(c.entry(tSegments, symSeg)), future)
	}

	// __aeabi_atexit goes to the RO library's own version
	if !isCRS {
		for i := uint32(0); i < c.count(tImpNamed); i++ {
			e := c.entry(tImpNamed, i)
			if c.str(mem.Read32(e), c.count(tImpStrings)) != "__aeabi_atexit" {
				continue
			}
			var err error
			eachLinked(crs, func(src cro) bool {
				if a := src.findExport("nnroAeabiAtexit_"); a != 0 {
					err = c.applyBatch(mem.Read32(e+4), a, false)
					return false
				}
				return true
			})
			if err != nil {
				return err
			}
		}
	}
	return nil
}

// importFrom gives this module the indexed and positional symbols it
// takes from src, when it names src among its imported modules.
func (c cro) importFrom(src cro) error {
	mem := c.m.mem
	name := src.moduleName()
	for i := uint32(0); i < c.count(tImpModules); i++ {
		e := c.entry(tImpModules, i)
		if c.str(mem.Read32(e), c.count(tImpStrings)) != name {
			continue
		}
		idxTab, idxN, anonTab, anonN := mem.Read32(e+4), mem.Read32(e+8), mem.Read32(e+12), mem.Read32(e+16)
		for j := uint32(0); j < idxN; j++ {
			im := idxTab + 8*j
			ex := src.entry(tExpIndexed, mem.Read32(im))
			if err := c.applyBatch(mem.Read32(im+4), src.addr(mem.Read32(ex)), false); err != nil {
				return err
			}
		}
		for j := uint32(0); j < anonN; j++ {
			im := anonTab + 8*j
			if err := c.applyBatch(mem.Read32(im+4), src.addr(mem.Read32(im)), false); err != nil {
				return err
			}
		}
	}
	return nil
}

// namedFrom resolves this module's unresolved named imports that src
// exports.
func (c cro) namedFrom(src cro) error {
	mem := c.m.mem
	for i := uint32(0); i < c.count(tImpNamed); i++ {
		e := c.entry(tImpNamed, i)
		batch := mem.Read32(e + 4)
		if c.reloc(batch).solved != 0 {
			continue
		}
		if a := src.findExport(c.str(mem.Read32(e), c.count(tImpStrings))); a != 0 {
			if err := c.applyBatch(batch, a, false); err != nil {
				return err
			}
		}
	}
	return nil
}

// link resolves the module's imports from the loaded modules, then gives
// its exports to them. fixData: the data segment is still in the file
// (linking while loading), so imports into .data are made there.
func (c cro) link(crs cro, fixData bool) error {
	mem := c.m.mem
	var err error
	var dataEntry, dataAddr uint32
	if fixData && c.count(tSegments) > segData && c.segType(segData) == segData {
		dataEntry = c.entry(tSegments, segData)
		dataAddr = mem.Read32(dataEntry)
		mem.Write32(dataEntry, c.word(tData))
	}
	// named imports: from the first module that has them
	for i := uint32(0); i < c.count(tImpNamed) && err == nil; i++ {
		e := c.entry(tImpNamed, i)
		batch := mem.Read32(e + 4)
		if c.reloc(batch).solved != 0 {
			continue
		}
		name := c.str(mem.Read32(e), c.count(tImpStrings))
		eachLinked(crs, func(src cro) bool {
			if a := src.findExport(name); a != 0 {
				err = c.applyBatch(batch, a, false)
				return false
			}
			return true
		})
	}
	// indexed and positional imports, from modules by name
	for i := uint32(0); i < c.count(tImpModules) && err == nil; i++ {
		want := c.str(mem.Read32(c.entry(tImpModules, i)), c.count(tImpStrings))
		eachLinked(crs, func(src cro) bool {
			if src.moduleName() == want {
				err = c.importFromEntry(src, i)
				return false
			}
			return true
		})
	}
	if dataEntry != 0 {
		mem.Write32(dataEntry, dataAddr)
	}
	if err != nil {
		return err
	}
	// exports to the loaded modules
	eachLinked(crs, func(t cro) bool {
		if err = t.namedFrom(c); err == nil {
			err = t.importFrom(c)
		}
		return err == nil
	})
	return err
}

// importFromEntry is importFrom for import module entry i only.
func (c cro) importFromEntry(src cro, i uint32) error {
	mem := c.m.mem
	e := c.entry(tImpModules, i)
	idxTab, idxN, anonTab, anonN := mem.Read32(e+4), mem.Read32(e+8), mem.Read32(e+12), mem.Read32(e+16)
	for j := uint32(0); j < idxN; j++ {
		im := idxTab + 8*j
		ex := src.entry(tExpIndexed, mem.Read32(im))
		if err := c.applyBatch(mem.Read32(im+4), src.addr(mem.Read32(ex)), false); err != nil {
			return err
		}
	}
	for j := uint32(0); j < anonN; j++ {
		im := anonTab + 8*j
		if err := c.applyBatch(mem.Read32(im+4), src.addr(mem.Read32(im)), false); err != nil {
			return err
		}
	}
	return nil
}

// unlink points the module's imports back at its "unresolved" handler,
// and other modules' imports of its symbols at theirs.
func (c cro) unlink(crs cro) error {
	mem := c.m.mem
	unresolved := c.addr(c.word(hOnUnresolved))
	for _, t := range []int{tImpNamed, tImpIndexed, tImpAnon} {
		for i := uint32(0); i < c.count(t); i++ {
			if err := c.applyBatch(mem.Read32(c.entry(t, i)+4), unresolved, true); err != nil {
				return err
			}
		}
	}
	name := c.moduleName()
	var err error
	eachLinked(crs, func(t cro) bool {
		u := t.addr(t.word(hOnUnresolved))
		for i := uint32(0); i < t.count(tImpNamed) && err == nil; i++ {
			e := t.entry(tImpNamed, i)
			batch := mem.Read32(e + 4)
			if t.reloc(batch).solved != 0 && c.findExport(t.str(mem.Read32(e), t.count(tImpStrings))) != 0 {
				err = t.applyBatch(batch, u, true)
			}
		}
		for i := uint32(0); i < t.count(tImpModules) && err == nil; i++ {
			e := t.entry(tImpModules, i)
			if t.str(mem.Read32(e), t.count(tImpStrings)) != name {
				continue
			}
			for _, tab := range [][2]uint32{{mem.Read32(e + 4), mem.Read32(e + 8)}, {mem.Read32(e + 12), mem.Read32(e + 16)}} {
				for j := uint32(0); j < tab[1] && err == nil; j++ {
					err = t.applyBatch(mem.Read32(tab[0]+8*j+4), u, true)
				}
			}
		}
		return err == nil
	})
	return err
}

// clearRelocations zeroes every relocated word (unloading a module that
// wasn't fixed).
func (c cro) clearRelocations() {
	mem := c.m.mem
	first := true
	for i := uint32(0); i < c.count(tExtRelocs); i++ {
		a := c.entry(tExtRelocs, i)
		r := c.reloc(a)
		if t := c.addr(r.tag); t != 0 {
			c.clearRelocation(t, r.typ)
		}
		if first {
			mem.Write8(a+6, 0)
		}
		first = r.end != 0
	}
	for i := uint32(0); i < c.count(tIntRelocs); i++ {
		e := c.entry(tIntRelocs, i)
		if t := c.addr(mem.Read32(e)); t != 0 {
			c.clearRelocation(t, uint8(mem.Read8(e+4)))
		}
	}
}

// register adds the module at the end of a list: the CRS's next link
// heads the auto-link list, its previous link the other; a head's
// previous link points to its list's tail.
func (c cro) register(crs cro, autoLink bool) {
	headField := hPrev
	if autoLink {
		headField = hNext
	}
	head := cro{c.m, crs.word(headField)}
	if head.base != 0 {
		tail := head.prev()
		c.setWord(hPrev, tail.base)
		tail.setWord(hNext, c.base)
		head.setWord(hPrev, c.base)
	} else {
		c.setWord(hPrev, c.base)
		crs.setWord(headField, c.base)
	}
	c.setWord(hNext, 0)
}

func (c cro) unregister(crs cro) {
	next, prev := c.next(), c.prev()
	switch {
	case crs.word(hNext) == c.base || crs.word(hPrev) == c.base: // the head
		if next.base != 0 {
			next.setWord(hPrev, prev.base)
		}
		if crs.word(hPrev) == c.base {
			crs.setWord(hPrev, next.base)
		} else {
			crs.setWord(hNext, next.base)
		}
	case next.base != 0: // the middle
		prev.setWord(hNext, next.base)
		next.setWord(hPrev, prev.base)
	default: // the tail
		prev.setWord(hNext, 0)
		for _, f := range []int{hNext, hPrev} {
			if h := (cro{c.m, crs.word(f)}); h.base != 0 && h.word(hPrev) == c.base {
				h.setWord(hPrev, prev.base)
				break
			}
		}
	}
	c.setWord(hNext, 0)
	c.setWord(hPrev, 0)
}

// fix cuts the module's linking data off at a level (0 keeps it all),
// returning the size the module keeps.
func (c cro) fix(level uint32) uint32 {
	if level > 3 {
		level = 3
	}
	end := max(c.base+croHeaderSize, c.word(tCode)+c.count(tCode))
	for t := tModName; t < croFixFrom[level]; t += 2 {
		end = max(end, c.word(t)+c.count(t)*croEntrySize[(t-tCode)/2])
	}
	if level != 0 {
		c.setWord(hMagic, croFixed)
		for t := croFixFrom[level]; t < tEnd; t += 2 {
			c.setWord(t, end)
			c.setWord(t+1, 0)
		}
	}
	size := (end+pageSize-1)&^(pageSize-1) - c.base
	c.setWord(hFixedSize, size)
	return size
}

// unrebase turns the addresses back into file offsets.
func (c cro) unrebase(isCRS bool) {
	mem := c.m.mem
	back := func(a uint32) {
		if v := mem.Read32(a); v != 0 {
			mem.Write32(a, v-c.base)
		}
	}
	for i := uint32(0); i < c.count(tImpAnon); i++ {
		back(c.entry(tImpAnon, i) + 4)
	}
	for i := uint32(0); i < c.count(tImpIndexed); i++ {
		back(c.entry(tImpIndexed, i) + 4)
	}
	for i := uint32(0); i < c.count(tImpNamed); i++ {
		back(c.entry(tImpNamed, i))
		back(c.entry(tImpNamed, i) + 4)
	}
	for i := uint32(0); i < c.count(tImpModules); i++ {
		e := c.entry(tImpModules, i)
		back(e)
		back(e + 4)
		back(e + 12)
	}
	for i := uint32(0); i < c.count(tExpNamed); i++ {
		back(c.entry(tExpNamed, i))
	}
	if !isCRS {
		for i := uint32(0); i < c.count(tSegments); i++ {
			e := c.entry(tSegments, i)
			switch mem.Read32(e + 8) {
			case segBSS:
				mem.Write32(e, 0)
			case segData:
				if mem.Read32(e+4) != 0 {
					mem.Write32(e, c.word(tData)-c.base)
				}
			default:
				back(e)
			}
		}
	}
	c.setWord(hNext, 0)
	c.setWord(hPrev, 0)
	c.setWord(hFixedSize, 0)
	back(c.base + croHashSize + 4*hNameOff)
	for t := tCode; t < tEnd; t += 2 {
		back(c.base + croHashSize + 4*uint32(t))
	}
}

func (c cro) loaded() bool {
	m := c.word(hMagic)
	return m == croMagic || m == croFixed
}

// roService is ldr:ro.
type roService struct {
	m   *Machine
	crs uint32 // the CRS's address (0 before Initialize)
}

func (*roService) name() string { return "ldr:ro" }

// mapAlias maps size bytes of the game's buffer at addr too, the way the
// RO service shows a module at the address the game asked for.
func (v *roService) mapAlias(addr, buf, size uint32) bool {
	mem := v.m.mem
	if !mem.mapped(buf, size) {
		return false
	}
	for off := uint32(0); off < size; off += pageSize {
		if err := mem.mapPhys(addr+off, mem.virtToPhys(buf+off), pageSize); err != nil {
			return false
		}
	}
	return true
}

func (v *roService) handle(c *ipc) {
	mem := v.m.mem
	aligned := func(a uint32) bool { return a&(pageSize-1) == 0 }
	inImage := func(a, size uint32) bool { return a >= codeBase && a+size <= 0x04000000 && a+size >= a }
	switch c.id() {
	case 0x0001: // Initialize(crs buffer, size, address, process)
		buf, size, addr := c.arg(0), c.arg(1), c.arg(2)
		switch {
		case v.crs != 0:
			c.result(rcROAlreadyInit)
		case size < croHeaderSize:
			c.result(rcROTooSmall)
		case !aligned(buf) || !aligned(addr):
			c.result(rcROMisaligned)
		case !aligned(size):
			c.result(rcROMisalignSize)
		case !inImage(addr, size) || !v.mapAlias(addr, buf, size):
			c.result(rcInvalidAddr)
		default:
			crs := cro{v.m, addr}
			crs.setWord(hNext, 0)
			crs.setWord(hPrev, 0)
			if err := crs.rebase(cro{}, size, 0, 0, 0, 0, true); err != nil {
				c.result(uint32(err.(croErr)))
				return
			}
			v.crs = addr
			v.m.logf("ldr:ro: the program's symbols (CRS) at %08x", addr)
		}
	case 0x0002, 0x0003: // LoadCRR, UnloadCRR: the list of allowed modules' hashes
	case 0x0004, 0x0009: // LoadCRO, LoadCRO_New
		buf, addr, size := c.arg(0), c.arg(1), c.arg(2)
		dataAddr, zero, dataSize := c.arg(3), c.arg(4), c.arg(5)
		bssAddr, bssSize := c.arg(6), c.arg(7)
		autoLink, fixLevel := c.arg(8)&0xff != 0, c.arg(9)
		fail := func(r uint32) { c.result(r); c.push(0) }
		switch {
		case v.crs == 0:
			fail(rcRONotInit)
			return
		case size < croHeaderSize:
			fail(rcROTooSmall)
			return
		case !aligned(buf) || !aligned(addr):
			fail(rcROMisaligned)
			return
		case !aligned(size):
			fail(rcROMisalignSize)
			return
		case zero != 0 || !inImage(addr, size) || !v.mapAlias(addr, buf, size):
			fail(rcInvalidAddr)
			return
		}
		m := cro{v.m, addr}
		crs := cro{v.m, v.crs}
		err := m.rebase(crs, size, dataAddr, dataSize, bssAddr, bssSize, false)
		if err == nil {
			err = m.link(crs, c.id() == 0x0009)
		}
		if err != nil {
			mem.unmap(addr, size)
			v.m.logf("ldr:ro: the code module at %08x couldn't be loaded", addr)
			fail(uint32(err.(croErr)))
			return
		}
		m.register(crs, autoLink)
		fixed := m.fix(fixLevel)
		if fixed < size {
			mem.unmap(addr+fixed, size-fixed)
		}
		v.m.logf("ldr:ro: code module %q at %08x (%#x bytes)", m.moduleName(), addr, fixed)
		c.push(fixed)
	case 0x0005: // UnloadCRO(address, zero, buffer)
		addr := c.arg(0)
		m := cro{v.m, addr}
		switch {
		case v.crs == 0:
			c.result(rcRONotInit)
		case !aligned(addr):
			c.result(rcROMisaligned)
		case !mem.mapped(addr, pageSize) || !m.loaded():
			c.result(rcRONotLoaded)
		default:
			crs := cro{v.m, v.crs}
			size := m.word(hFixedSize)
			m.unregister(crs)
			if err := m.unlink(crs); err != nil {
				c.result(uint32(err.(croErr)))
				return
			}
			if m.word(hMagic) != croFixed {
				m.clearRelocations()
			}
			m.unrebase(false)
			mem.unmap(addr, size)
		}
	case 0x0006, 0x0007: // LinkCRO, UnlinkCRO(address)
		addr := c.arg(0)
		m := cro{v.m, addr}
		switch {
		case v.crs == 0:
			c.result(rcRONotInit)
		case !aligned(addr):
			c.result(rcROMisaligned)
		case !mem.mapped(addr, pageSize) || !m.loaded():
			c.result(rcRONotLoaded)
		default:
			var err error
			if c.id() == 0x0006 {
				err = m.link(cro{v.m, v.crs}, false)
			} else {
				err = m.unlink(cro{v.m, v.crs})
			}
			if err != nil {
				c.result(uint32(err.(croErr)))
			}
		}
	case 0x0008: // Shutdown(crs buffer)
		if v.crs == 0 {
			c.result(rcRONotInit)
			return
		}
		crs := cro{v.m, v.crs}
		size := crs.word(hFileSize)
		crs.unrebase(true)
		mem.unmap(v.crs, (size+pageSize-1)&^(pageSize-1))
		v.crs = 0
	default:
		v.m.logf("ldr:ro: command %04x", c.id())
	}
}

// warnOnce logs a message the first time its key comes up.
func (m *Machine) warnOnce(key, format string, a ...any) {
	if m.svcs.log[key] {
		return
	}
	m.svcs.log[key] = true
	m.logf(format, a...)
}
