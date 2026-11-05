package ctr

import (
	"encoding/binary"
	"testing"
)

// A small module builder for the tests: a code segment, a data segment
// and a .bss, named exports, named imports, imports from a module by
// index, and internal relocations.
type testReloc struct {
	seg, off uint32 // where to patch (segment index, offset)
	typ      uint8
	addend   uint32
}

type testModule struct {
	name      string
	code      []byte
	data      []byte
	bss       uint32
	exports   map[string]uint32 // name → code offset
	indexed   []uint32          // indexed exports: code offsets
	imports   map[string]testReloc
	modImport struct {
		module  string
		indexed []struct {
			index uint32
			at    testReloc
		}
	}
	internal []struct {
		at     testReloc
		symSeg uint8
	}
	crs           bool // segment offsets absolute (at codeAddr), as in a CRS
	codeAddr      uint32
	unresolvedOff uint32 // in code
}

func (t *testModule) build() []byte {
	le := binary.LittleEndian
	out := make([]byte, croHeaderSize)
	field := func(i int, v uint32) { le.PutUint32(out[croHashSize+4*i:], v) }
	pad := func() {
		for len(out)%4 != 0 {
			out = append(out, 0)
		}
	}
	table := func(t int, b []byte, n int) int {
		pad()
		pos := len(out)
		field(t, uint32(pos))
		field(t+1, uint32(n))
		out = append(out, b...)
		return pos
	}
	copy(out[croHashSize:], "CRO0")

	codeOff := uint32(table(tCode, t.code, len(t.code)))
	table(tModName, append([]byte(t.name), 0), len(t.name)+1)
	// segments: code, rodata (empty), data, bss
	seg := make([]byte, 48)
	putSeg := func(i int, off, size, typ uint32) {
		le.PutUint32(seg[12*i:], off)
		le.PutUint32(seg[12*i+4:], size)
		le.PutUint32(seg[12*i+8:], typ)
	}
	if t.crs {
		putSeg(0, t.codeAddr, uint32(len(t.code)), segCode)
	} else {
		putSeg(0, codeOff, uint32(len(t.code)), segCode)
	}
	putSeg(1, 0, 0, segROData)
	putSeg(3, 0, t.bss, segBSS)
	segPos := table(tSegments, seg, 4)

	// exports: names sorted for a two-leaf tree at most
	var names []string
	for n := range t.exports {
		names = append(names, n)
	}
	if len(names) == 2 && names[0] > names[1] {
		names[0], names[1] = names[1], names[0]
	}
	expNamed := make([]byte, 8*len(names))
	var strs []byte
	nameOffs := make([]int, len(names))
	for i, n := range names {
		nameOffs[i] = len(strs)
		strs = append(strs, n...)
		strs = append(strs, 0)
		le.PutUint32(expNamed[8*i+4:], t.exports[n]<<4|0)
	}
	expNamedPos := table(tExpNamed, expNamed, len(names))
	tree := make([]byte, 24)
	switch len(names) {
	case 1:
		le.PutUint16(tree[2:], 0x8000)
	case 2:
		a, b := names[0], names[1]
		bit := 0
		for ; ; bit++ {
			var x, y byte
			if bit>>3 < len(a) {
				x = a[bit>>3] >> (bit & 7) & 1
			}
			if bit>>3 < len(b) {
				y = b[bit>>3] >> (bit & 7) & 1
			}
			if x != y {
				break
			}
		}
		left, right := uint16(0x8001), uint16(0x8002)
		if a[bit>>3]>>(bit&7)&1 == 1 {
			left, right = right, left
		}
		le.PutUint16(tree[2:], 1)
		le.PutUint16(tree[8:], uint16(bit))
		le.PutUint16(tree[10:], left)
		le.PutUint16(tree[12:], right)
		le.PutUint16(tree[14:], 0)
		le.PutUint16(tree[22:], 1)
	}
	nTree := 0
	if len(names) == 1 {
		nTree = 1
	} else if len(names) == 2 {
		nTree = 3
	}
	table(tExpTree, tree[:8*nTree], nTree)
	idx := make([]byte, 4*len(t.indexed))
	for i, o := range t.indexed {
		le.PutUint32(idx[4*i:], o<<4)
	}
	table(tExpIndexed, idx, len(t.indexed))
	strPos := table(tExpStrings, strs, len(strs))
	for i := range names {
		le.PutUint32(out[expNamedPos+8*i:], uint32(strPos+nameOffs[i]))
	}

	// imports
	var impStrs []byte
	var relocs []byte
	reloc := func(r testReloc) uint32 {
		off := uint32(len(relocs))
		e := make([]byte, 12)
		le.PutUint32(e, r.off<<4|r.seg)
		e[4], e[5] = r.typ, 1
		le.PutUint32(e[8:], r.addend)
		relocs = append(relocs, e...)
		return off
	}
	type named struct{ name, batch uint32 }
	var impNamed []named
	var impNames []string
	for n := range t.imports {
		impNames = append(impNames, n)
	}
	for _, n := range impNames {
		impNamed = append(impNamed, named{uint32(len(impStrs)), reloc(t.imports[n])})
		impStrs = append(impStrs, n...)
		impStrs = append(impStrs, 0)
	}
	var impIdx [][2]uint32
	modName := uint32(len(impStrs))
	for _, im := range t.modImport.indexed {
		impIdx = append(impIdx, [2]uint32{im.index, reloc(im.at)})
	}
	if t.modImport.module != "" {
		impStrs = append(impStrs, t.modImport.module...)
		impStrs = append(impStrs, 0)
	}
	nMods := 0
	if t.modImport.module != "" {
		nMods = 1
	}
	modPos := table(tImpModules, make([]byte, 20*nMods), nMods)
	relPos := table(tExtRelocs, relocs, len(relocs)/12)
	inPos := table(tImpNamed, make([]byte, 8*len(impNamed)), len(impNamed))
	ixPos := table(tImpIndexed, make([]byte, 8*len(impIdx)), len(impIdx))
	table(tImpAnon, nil, 0)
	isPos := table(tImpStrings, impStrs, len(impStrs))
	for i, e := range impNamed {
		le.PutUint32(out[inPos+8*i:], uint32(isPos)+e.name)
		le.PutUint32(out[inPos+8*i+4:], uint32(relPos)+e.batch)
	}
	for i, e := range impIdx {
		le.PutUint32(out[ixPos+8*i:], e[0])
		le.PutUint32(out[ixPos+8*i+4:], uint32(relPos)+e[1])
	}
	if nMods == 1 {
		le.PutUint32(out[modPos:], uint32(isPos)+modName)
		le.PutUint32(out[modPos+4:], uint32(ixPos))
		le.PutUint32(out[modPos+8:], uint32(len(impIdx)))
	}
	table(tStaticAnon, nil, 0)
	ir := make([]byte, 12*len(t.internal))
	for i, r := range t.internal {
		le.PutUint32(ir[12*i:], r.at.off<<4|r.at.seg)
		ir[12*i+4], ir[12*i+5] = r.at.typ, r.symSeg
		le.PutUint32(ir[12*i+8:], r.at.addend)
	}
	table(tIntRelocs, ir, len(t.internal))
	table(tStaticReloc, nil, 0)
	dataOff := uint32(table(tData, t.data, len(t.data)))
	if !t.crs {
		le.PutUint32(out[segPos+24:], dataOff)
	}
	le.PutUint32(out[segPos+28:], uint32(len(t.data)))
	le.PutUint32(out[segPos+32:], segData)
	field(hOnUnresolved, t.unresolvedOff<<4)
	for len(out)%pageSize != 0 {
		out = append(out, 0)
	}
	field(hFileSize, uint32(len(out)))
	return out
}

func TestCodeModules(t *testing.T) {
	m := newMachine(Options{})
	mem := m.mem
	le := binary.LittleEndian
	// the game's heap for the files and the .data/.bss buffers
	pages, err := mem.allocPages(64)
	if err != nil {
		t.Fatal(err)
	}
	for i, p := range pages {
		mem.mapPhys(0x08000000+uint32(i)*pageSize, p, pageSize)
	}
	ro := &roService{m: m}
	cmd := uint32(0x08030000)
	call := func(id uint32, args ...uint32) []uint32 {
		mem.Write32(cmd, id<<16|uint32(len(args))<<6)
		for i, a := range args {
			mem.Write32(cmd+4+4*uint32(i), a)
		}
		c := &ipc{m: m, buf: cmd, header: id << 16}
		ro.handle(c)
		return append([]uint32{c.res}, c.out...)
	}
	put := func(addr uint32, b []byte) { mem.writeBytes(addr, b) }

	// the CRS: the main program's code at 0x00100000 exports crsFunc at +0x10
	code := make([]byte, 0x100)
	mem.mapPhys(0x00100000, pages[60], pageSize)
	crs := (&testModule{name: "static", code: code, crs: true, codeAddr: 0x00100000,
		exports: map[string]uint32{"crsFunc": 0x10},
		imports: map[string]testReloc{"modFunc": {0, 0x40, 2, 0}}}).build()
	put(0x08000000, crs)
	if r := call(1, 0x08000000, uint32(len(crs)), 0x00200000); r[0] != 0 {
		t.Fatalf("Initialize: %08x", r[0])
	}

	// modA: imports crsFunc (absolute, +4) into code+0x30; its own pointer
	// at data+0 to code+0x20 and at code+0x34 to its data; exports modFunc
	// (code+0x20) by name and code+0x24 by index 0
	modA := (&testModule{name: "modA", code: make([]byte, 0x80), data: make([]byte, 0x10), bss: 0x20,
		exports: map[string]uint32{"modFunc": 0x20, "other": 0x28},
		indexed: []uint32{0x24},
		imports: map[string]testReloc{"crsFunc": {0, 0x30, 2, 4}},
		internal: []struct {
			at     testReloc
			symSeg uint8
		}{{testReloc{2, 0, 2, 0x20}, 0}, {testReloc{0, 0x34, 2, 0}, 2}, {testReloc{0, 0x38, 3, 8}, 0}},
	}).build()
	put(0x08010000, modA)
	const aAddr, aData, aBss = 0x00400000, 0x08020000, 0x08021000
	r := call(9, 0x08010000, aAddr, uint32(len(modA)), aData, 0, 0x10, aBss, 0x20, 1, 0, 0)
	if r[0] != 0 {
		t.Fatalf("LoadCRO_New modA: %08x", r[0])
	}
	a := cro{m, aAddr}
	codeA := a.word(tCode)
	if codeA != aAddr+croHeaderSize {
		t.Fatalf("modA code at %08x", codeA)
	}
	if got := mem.Read32(codeA + 0x30); got != 0x00100014 {
		t.Errorf("modA's import of crsFunc: %08x, want 00100014", got)
	}
	if got := mem.Read32(codeA + 0x34); got != aData {
		t.Errorf("modA's pointer to its data: %08x, want %08x", got, aData)
	}
	if got, want := mem.Read32(codeA+0x38), codeA+8-(codeA+0x38); got != want {
		t.Errorf("relative relocation: %08x, want %08x", got, want)
	}
	// the data pointer is made in the file's copy of .data
	if got := mem.Read32(a.word(tData)); got != codeA+0x20 {
		t.Errorf("modA's data pointer: %08x, want %08x", got, codeA+0x20)
	}
	// the CRS's import of modFunc was resolved when modA arrived
	if got := mem.Read32(0x00100040); got != codeA+0x20 {
		t.Errorf("the CRS's import of modFunc: %08x, want %08x", got, codeA+0x20)
	}
	if a.findExport("other") != codeA+0x28 || a.findExport("modFunc") != codeA+0x20 || a.findExport("nope") != 0 {
		t.Errorf("export lookup")
	}
	if cro(a).word(hFixedSize) != r[1] || r[1] == 0 {
		t.Errorf("fixed size %x", r[1])
	}
	// modA is the auto-link list's head and tail
	if le := (cro{m, 0x00200000}); le.word(hNext) != aAddr || a.word(hPrev) != aAddr {
		t.Errorf("module list: CRS next %08x, modA previous %08x", le.word(hNext), a.word(hPrev))
	}

	// modB imports modFunc by name and modA's indexed export 0
	modB := (&testModule{name: "modB", code: make([]byte, 0x40),
		imports: map[string]testReloc{"modFunc": {0, 0x10, 2, 0}},
	})
	modB.modImport.module = "modA"
	modB.modImport.indexed = append(modB.modImport.indexed, struct {
		index uint32
		at    testReloc
	}{0, testReloc{0, 0x14, 2, 0}})
	bFile := modB.build()
	put(0x08028000, bFile)
	const bAddr = 0x00500000
	if r := call(4, 0x08028000, bAddr, uint32(len(bFile)), 0, 0, 0, 0, 0, 1, 0, 0); r[0] != 0 {
		t.Fatalf("LoadCRO modB: %08x", r[0])
	}
	b := cro{m, bAddr}
	codeB := b.word(tCode)
	if got := mem.Read32(codeB + 0x10); got != codeA+0x20 {
		t.Errorf("modB's import of modFunc: %08x, want %08x", got, codeA+0x20)
	}
	if got := mem.Read32(codeB + 0x14); got != codeA+0x24 {
		t.Errorf("modB's indexed import: %08x, want %08x", got, codeA+0x24)
	}
	if a.word(hNext) != bAddr || b.word(hPrev) != aAddr || a.word(hPrev) != bAddr {
		t.Errorf("module list after modB: modA next %08x prev %08x, modB prev %08x", a.word(hNext), a.word(hPrev), b.word(hPrev))
	}

	// unloading modA points modB's imports and the CRS's back at their
	// unresolved handlers (code+0 in each)
	if r := call(5, aAddr, 0, 0x08010000); r[0] != 0 {
		t.Fatalf("UnloadCRO: %08x", r[0])
	}
	if got := mem.Read32(codeB + 0x10); got != codeB {
		t.Errorf("modB's import after unloading: %08x, want %08x", got, codeB)
	}
	if got := mem.Read32(0x00100040); got != 0x00100000 {
		t.Errorf("the CRS's import after unloading: %08x", got)
	}
	if (cro{m, 0x00200000}).word(hNext) != bAddr || b.word(hPrev) != bAddr {
		t.Errorf("module list after unloading")
	}
	if mem.mapped(aAddr, pageSize) {
		t.Errorf("modA still mapped")
	}
	_ = le
}
