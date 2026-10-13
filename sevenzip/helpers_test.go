// Test-only helpers: a minimal 7z writer, a range encoder and a BCJ2
// encoder, plus ways to borrow independent compressors (Python's lzma
// module, py7zr, the xz tool) when they are installed.

package sevenzip

import (
	"bytes"
	"encoding/binary"
	"hash/crc32"
	"os"
	"os/exec"
	"path/filepath"
	"testing"
	"unicode/utf16"
)

// ---------------- minimal 7z writer ----------------

type tfolder struct {
	coders  []coderInfo
	bind    []bindPair
	packed  []int    // folder in-stream index of each pack stream
	packs   [][]byte // pack stream contents, same order as packed
	sizes   []uint64 // unpack size of every coder output
	streams [][]byte // the files stored in this folder, in order
}

type tentry struct {
	name  string
	dir   bool
	empty bool // empty file (no stream)
	mtime uint64
}

type hw struct{ bytes.Buffer }

func (w *hw) num(v uint64) {
	for i := 0; i < 8; i++ {
		if v < 1<<(7*uint(i+1)) {
			w.WriteByte(byte(0xFF<<(8-uint(i))) | byte(v>>(8*uint(i))))
			for j := 0; j < i; j++ {
				w.WriteByte(byte(v >> (8 * uint(j))))
			}
			return
		}
	}
	w.WriteByte(0xFF)
	binary.Write(w, binary.LittleEndian, v)
}

func (w *hw) u32(v uint32) { binary.Write(w, binary.LittleEndian, v) }

func (w *hw) bits(v []bool) {
	var cur byte
	for i, b := range v {
		if b {
			cur |= 0x80 >> uint(i%8)
		}
		if i%8 == 7 {
			w.WriteByte(cur)
			cur = 0
		}
	}
	if len(v)%8 != 0 {
		w.WriteByte(cur)
	}
}

func (w *hw) prop(id byte, data []byte) {
	w.WriteByte(id)
	w.num(uint64(len(data)))
	w.Write(data)
}

func writeFolders(w *hw, packPos uint64, folders []tfolder, withSub bool) {
	var packs [][]byte
	for _, f := range folders {
		packs = append(packs, f.packs...)
	}
	w.WriteByte(idPackInfo)
	w.num(packPos)
	w.num(uint64(len(packs)))
	w.WriteByte(idSize)
	for _, p := range packs {
		w.num(uint64(len(p)))
	}
	w.WriteByte(idEnd)

	w.WriteByte(idUnpackInfo)
	w.WriteByte(idFolder)
	w.num(uint64(len(folders)))
	w.WriteByte(0)
	for _, f := range folders {
		w.num(uint64(len(f.coders)))
		for _, c := range f.coders {
			flags := byte(len(c.id))
			if c.numIn != 1 || c.numOut != 1 {
				flags |= 0x10
			}
			if len(c.props) > 0 {
				flags |= 0x20
			}
			w.WriteByte(flags)
			w.Write(c.id)
			if flags&0x10 != 0 {
				w.num(uint64(c.numIn))
				w.num(uint64(c.numOut))
			}
			if len(c.props) > 0 {
				w.num(uint64(len(c.props)))
				w.Write(c.props)
			}
		}
		for _, bp := range f.bind {
			w.num(uint64(bp.in))
			w.num(uint64(bp.out))
		}
		if len(f.packed) > 1 {
			for _, p := range f.packed {
				w.num(uint64(p))
			}
		}
	}
	w.WriteByte(idCodersUnpackSize)
	for _, f := range folders {
		for _, s := range f.sizes {
			w.num(s)
		}
	}
	if !withSub {
		// Folder CRCs (used for an encoded header).
		w.WriteByte(idCRC)
		w.WriteByte(1)
		for _, f := range folders {
			w.u32(crc32.ChecksumIEEE(bytes.Join(f.streams, nil)))
		}
	}
	w.WriteByte(idEnd)

	if withSub {
		w.WriteByte(idSubStreamsInfo)
		w.WriteByte(idNumUnpackStream)
		for _, f := range folders {
			w.num(uint64(len(f.streams)))
		}
		w.WriteByte(idSize)
		for _, f := range folders {
			for i := 0; i+1 < len(f.streams); i++ {
				w.num(uint64(len(f.streams[i])))
			}
		}
		w.WriteByte(idCRC)
		w.WriteByte(1)
		for _, f := range folders {
			for _, s := range f.streams {
				w.u32(crc32.ChecksumIEEE(s))
			}
		}
		w.WriteByte(idEnd)
	}
	w.WriteByte(idEnd)
}

// build7z assembles a complete archive. With encode set, the header is
// stored as a Copy folder behind a kEncodedHeader record.
func build7z(folders []tfolder, entries []tentry, encode bool) []byte {
	var body bytes.Buffer
	for _, f := range folders {
		for _, p := range f.packs {
			body.Write(p)
		}
	}
	h := &hw{}
	h.WriteByte(idHeader)
	if len(folders) > 0 {
		h.WriteByte(idMainStreamsInfo)
		writeFolders(h, 0, folders, true)
	}
	h.WriteByte(idFilesInfo)
	h.num(uint64(len(entries)))
	var es, ef []bool
	anyEmpty := false
	for _, e := range entries {
		es = append(es, e.dir || e.empty)
		if e.dir || e.empty {
			anyEmpty = true
			ef = append(ef, e.empty)
		}
	}
	if anyEmpty {
		var b hw
		b.bits(es)
		h.prop(idEmptyStream, b.Bytes())
		var c hw
		c.bits(ef)
		h.prop(idEmptyFile, c.Bytes())
	}
	var names hw
	names.WriteByte(0)
	for _, e := range entries {
		for _, u := range utf16.Encode([]rune(e.name)) {
			binary.Write(&names, binary.LittleEndian, u)
		}
		names.Write([]byte{0, 0})
	}
	h.prop(idName, names.Bytes())
	var mt hw
	mt.WriteByte(1)
	mt.WriteByte(0)
	for _, e := range entries {
		binary.Write(&mt, binary.LittleEndian, e.mtime)
	}
	h.prop(idMTime, mt.Bytes())
	h.WriteByte(idEnd)
	h.WriteByte(idEnd)
	header := h.Bytes()

	if encode {
		pos := uint64(body.Len())
		body.Write(header)
		e := &hw{}
		e.WriteByte(idEncodedHeader)
		writeFolders(e, pos, []tfolder{{
			coders:  []coderInfo{{id: []byte{0}, numIn: 1, numOut: 1}},
			packed:  []int{0},
			packs:   [][]byte{header},
			sizes:   []uint64{uint64(len(header))},
			streams: [][]byte{header},
		}}, false)
		header = e.Bytes()
	}

	var out bytes.Buffer
	out.Write(signature)
	out.Write([]byte{0, 4})
	var tail [20]byte
	binary.LittleEndian.PutUint64(tail[0:], uint64(body.Len()))
	binary.LittleEndian.PutUint64(tail[8:], uint64(len(header)))
	binary.LittleEndian.PutUint32(tail[16:], crc32.ChecksumIEEE(header))
	binary.Write(&out, binary.LittleEndian, crc32.ChecksumIEEE(tail[:]))
	out.Write(tail[:])
	out.Write(body.Bytes())
	out.Write(header)
	return out.Bytes()
}

// copyFolder stores files uncompressed in one solid folder.
func copyFolder(files ...[]byte) tfolder {
	all := bytes.Join(files, nil)
	return tfolder{
		coders:  []coderInfo{{id: []byte{0}, numIn: 1, numOut: 1}},
		packed:  []int{0},
		packs:   [][]byte{all},
		sizes:   []uint64{uint64(len(all))},
		streams: files,
	}
}

// filterFolder is filter <- codec <- pack stream, the usual two-coder chain.
func filterFolder(filterID, filterProps, codecID, codecProps, packed []byte, files ...[]byte) tfolder {
	all := bytes.Join(files, nil)
	return tfolder{
		coders: []coderInfo{
			{id: filterID, numIn: 1, numOut: 1, props: filterProps},
			{id: codecID, numIn: 1, numOut: 1, props: codecProps},
		},
		bind:    []bindPair{{in: 0, out: 1}},
		packed:  []int{1},
		packs:   [][]byte{packed},
		sizes:   []uint64{uint64(len(all)), uint64(len(all))},
		streams: files,
	}
}

func writeArchive(t *testing.T, data []byte) string {
	t.Helper()
	p := filepath.Join(t.TempDir(), "test.7z")
	if err := os.WriteFile(p, data, 0o644); err != nil {
		t.Fatal(err)
	}
	return p
}

// ---------------- range encoder and BCJ2 encoder ----------------

// rangeEncoder is the encoding side of rangeDecoder, used to build BCJ2
// streams for tests.
type rangeEncoder struct {
	low       uint64
	rng       uint32
	cache     byte
	cacheSize int
	out       bytes.Buffer
}

func newRangeEncoder() *rangeEncoder { return &rangeEncoder{rng: 0xFFFFFFFF, cacheSize: 1} }

func (e *rangeEncoder) shiftLow() {
	if uint32(e.low) < 0xFF000000 || e.low >= 1<<32 {
		carry := byte(e.low >> 32)
		c := e.cache
		for ; e.cacheSize > 0; e.cacheSize-- {
			e.out.WriteByte(c + carry)
			c = 0xFF
		}
		e.cache = byte(e.low >> 24)
	}
	e.cacheSize++
	e.low = (e.low & 0x00FFFFFF) << 8
}

func (e *rangeEncoder) encode(p *uint16, bit bool) {
	v := uint32(*p)
	bound := (e.rng >> 11) * v
	if !bit {
		e.rng = bound
		*p = uint16(v + (2048-v)>>5)
	} else {
		e.low += uint64(bound)
		e.rng -= bound
		*p = uint16(v - v>>5)
	}
	for e.rng < 1<<24 {
		e.rng <<= 8
		e.shiftLow()
	}
}

func (e *rangeEncoder) flush() []byte {
	for i := 0; i < 5; i++ {
		e.shiftLow()
	}
	return e.out.Bytes()
}

// bcj2Encode splits x86 code into the four BCJ2 streams. It converts every
// CALL/JMP/Jcc whose operand looks like a near target, as 7-Zip's encoder
// would, and leaves the others.
func bcj2Encode(x []byte) (main, call, jump, rc []byte) {
	var m, c, j bytes.Buffer
	enc := newRangeEncoder()
	var probs [258]uint16
	fill(probs[:])
	var prev byte
	for i := 0; i < len(x); {
		b := x[i]
		m.WriteByte(b)
		i++
		if !(b&0xFE == 0xE8 || (prev == 0x0F && b&0xF0 == 0x80)) {
			prev = b
			continue
		}
		if i == len(x) {
			break
		}
		p := &probs[257]
		if b == 0xE8 {
			p = &probs[prev]
		} else if b == 0xE9 {
			p = &probs[256]
		}
		convert := i+4 <= len(x) && near(x[i+3])
		enc.encode(p, convert)
		if !convert {
			prev = b
			continue
		}
		abs := binary.LittleEndian.Uint32(x[i:]) + uint32(i+4)
		dst := &j
		if b == 0xE8 {
			dst = &c
		}
		binary.Write(dst, binary.BigEndian, abs)
		prev = x[i+3]
		i += 4
	}
	return m.Bytes(), c.Bytes(), j.Bytes(), enc.flush()
}

// ---------------- external tools ----------------

// python returns a Python 3 interpreter, or "" when there is none.
func python() string {
	for _, p := range []string{"python3", "python"} {
		if path, err := exec.LookPath(p); err == nil {
			if exec.Command(path, "-I", "-c", "import lzma").Run() == nil {
				return path
			}
		}
	}
	return ""
}

// pyRaw compresses data with Python's lzma module in raw mode using the given
// filter chain, written as a Python expression.
func pyRaw(t *testing.T, filters string, data []byte) []byte {
	t.Helper()
	py := python()
	if py == "" {
		t.Skip("python3 with lzma not found")
	}
	cmd := exec.Command(py, "-I", "-c",
		"import lzma,sys; sys.stdout.buffer.write(lzma.compress(sys.stdin.buffer.read(), format=lzma.FORMAT_RAW, filters="+filters+"))")
	cmd.Stdin = bytes.NewReader(data)
	var stderr bytes.Buffer
	cmd.Stderr = &stderr
	out, err := cmd.Output()
	if err != nil {
		t.Fatalf("python lzma: %v %s", err, stderr.String())
	}
	return out
}

// py7zrPython returns a Python that can import py7zr: $SEVENZIP_PYTHON, or
// python3 on the PATH.
func py7zrPython() string {
	cands := []string{os.Getenv("SEVENZIP_PYTHON"), "python3", "python"}
	for _, c := range cands {
		if c == "" {
			continue
		}
		if path, err := exec.LookPath(c); err == nil {
			if exec.Command(path, "-I", "-c", "import py7zr").Run() == nil {
				return path
			}
		}
	}
	return ""
}
