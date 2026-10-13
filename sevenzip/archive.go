// Reading the 7z container: the signature header, the (possibly compressed)
// header database, and turning a folder's coder graph into one output stream.
//
// Layout, from the 7z format description: a 32-byte signature header points
// at the "next header" at the end of the file. That header lists the packed
// streams, the folders (each a small graph of coders such as BCJ2 feeding
// from three LZMA decoders), how each folder's output is split into files
// (substreams), and the file names, times and attributes. The header itself
// is often compressed and stored as a folder of its own (kEncodedHeader).

package sevenzip

import (
	"bytes"
	"compress/bzip2"
	"compress/flate"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"fmt"
	"hash/crc32"
	"io"
	"os"
	"time"
	"unicode/utf16"
)

// Property IDs used in the header.
const (
	idEnd = iota
	idHeader
	idArchiveProperties
	idAdditionalStreamsInfo
	idMainStreamsInfo
	idFilesInfo
	idPackInfo
	idUnpackInfo
	idSubStreamsInfo
	idSize
	idCRC
	idFolder
	idCodersUnpackSize
	idNumUnpackStream
	idEmptyStream
	idEmptyFile
	idAnti
	idName
	idCTime
	idATime
	idMTime
	idWinAttributes
	idComment
	idEncodedHeader
	idStartPos
	idDummy
)

var signature = []byte{'7', 'z', 0xBC, 0xAF, 0x27, 0x1C}

// Limits that keep a damaged header from asking for absurd amounts of memory.
const (
	maxHeaderSize = 256 << 20
	maxCount      = 1 << 24
)

type coderInfo struct {
	id     []byte
	numIn  int
	numOut int
	props  []byte
}

type bindPair struct{ in, out int }

type folder struct {
	coders      []coderInfo
	bindPairs   []bindPair
	packed      []int // folder in-stream indexes that read pack streams
	unpackSizes []uint64
	crc         uint32
	hasCRC      bool
	firstPack   int // index of this folder's first pack stream
	numStreams  int // number of file streams (substreams) inside
}

// size is the size of the folder's final output.
func (f *folder) size() uint64 {
	o, err := f.mainOut()
	if err != nil {
		return 0
	}
	return f.unpackSizes[o]
}

// mainOut finds the out-stream that no bind pair consumes.
func (f *folder) mainOut() (int, error) {
	total := 0
	for _, c := range f.coders {
		total += c.numOut
	}
	used := make([]bool, total)
	for _, bp := range f.bindPairs {
		if bp.out >= total {
			return 0, errBadHeader
		}
		used[bp.out] = true
	}
	for i, u := range used {
		if !u {
			return i, nil
		}
	}
	return 0, errBadHeader
}

type streamsInfo struct {
	packPos   uint64
	packSizes []uint64
	folders   []*folder
	// one entry per substream, in folder order
	subSizes  []uint64
	subCRC    []uint32
	subHasCRC []bool
}

// File describes one entry of an archive.
type File struct {
	Name    string // path inside the archive, with '/' separators
	Size    uint64
	IsDir   bool
	ModTime time.Time // zero if the archive doesn't record it
	Attrib  uint32    // Windows attributes, 0 if not recorded

	hasStream bool
	anti      bool
	crc       uint32
	hasCRC    bool
	folder    int // folder holding the data, for files with a stream
}

type archive struct {
	f       *os.File
	size    int64
	streams *streamsInfo
	files   []File
}

var errBadHeader = errors.New("this .7z file is damaged or not a 7z archive (bad header)")

// ---------------- low-level header reader ----------------

// headerError carries a parse failure out through panic/recover so the
// parsing code can stay linear.
type headerError struct{ err error }

type hreader struct {
	b []byte
	p int
}

func (h *hreader) fail() { panic(headerError{errBadHeader}) }

func (h *hreader) left() int { return len(h.b) - h.p }

func (h *hreader) byte() byte {
	if h.p >= len(h.b) {
		h.fail()
	}
	b := h.b[h.p]
	h.p++
	return b
}

func (h *hreader) bytes(n uint64) []byte {
	if n > uint64(h.left()) {
		h.fail()
	}
	b := h.b[h.p : h.p+int(n)]
	h.p += int(n)
	return b
}

func (h *hreader) uint32() uint32 { return binary.LittleEndian.Uint32(h.bytes(4)) }
func (h *hreader) uint64() uint64 { return binary.LittleEndian.Uint64(h.bytes(8)) }

// number reads the 7z variable-length integer: the count of leading one bits
// in the first byte says how many little-endian bytes follow, and the first
// byte's remaining bits are the top part.
func (h *hreader) number() uint64 {
	first := h.byte()
	var v uint64
	mask := byte(0x80)
	for i := 0; i < 8; i++ {
		if first&mask == 0 {
			high := uint64(first & (mask - 1))
			return v | high<<(8*uint(i))
		}
		v |= uint64(h.byte()) << (8 * uint(i))
		mask >>= 1
	}
	return v
}

// count reads a number used as an item count and sanity-checks it.
func (h *hreader) count() int {
	n := h.number()
	if n > maxCount || n > 8*uint64(len(h.b))+8 {
		h.fail()
	}
	return int(n)
}

func (h *hreader) bits(n int) []bool {
	v := make([]bool, n)
	var cur byte
	for i := 0; i < n; i++ {
		if i%8 == 0 {
			cur = h.byte()
		}
		v[i] = cur&(0x80>>uint(i%8)) != 0
	}
	return v
}

// defined reads the "all defined" byte and, if zero, the bit vector after it.
func (h *hreader) defined(n int) []bool {
	if h.byte() != 0 {
		v := make([]bool, n)
		for i := range v {
			v[i] = true
		}
		return v
	}
	return h.bits(n)
}

func (h *hreader) digests(n int) ([]bool, []uint32) {
	def := h.defined(n)
	crcs := make([]uint32, n)
	for i := range crcs {
		if def[i] {
			crcs[i] = h.uint32()
		}
	}
	return def, crcs
}

// skipProps skips a property list of (type, size, data) items ending in 0.
func (h *hreader) skipProps() {
	for h.byte() != idEnd {
		h.bytes(h.number())
	}
}

// ---------------- opening ----------------

func openArchive(path string) (a *archive, err error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer func() {
		if err != nil {
			f.Close()
		}
	}()
	st, err := f.Stat()
	if err != nil {
		return nil, err
	}
	a = &archive{f: f, size: st.Size()}

	var sh [32]byte
	if _, err := io.ReadFull(f, sh[:]); err != nil {
		return nil, errors.New("this file is too short to be a .7z archive")
	}
	if !bytes.Equal(sh[:6], signature) {
		return nil, errors.New("this file is not a .7z archive")
	}
	if sh[6] != 0 {
		return nil, fmt.Errorf("this .7z uses format version %d.%d, which RetroPixels can't read", sh[6], sh[7])
	}
	if crc32.ChecksumIEEE(sh[12:32]) != binary.LittleEndian.Uint32(sh[8:]) {
		return nil, errors.New("this .7z file is damaged (start header checksum mismatch)")
	}
	off := binary.LittleEndian.Uint64(sh[12:])
	size := binary.LittleEndian.Uint64(sh[20:])
	want := binary.LittleEndian.Uint32(sh[28:])
	if size == 0 {
		return a, nil // empty archive
	}
	if size > maxHeaderSize || off > uint64(a.size) || 32+off+size > uint64(a.size) {
		return nil, errors.New("this .7z file is incomplete or damaged (header is outside the file)")
	}
	buf := make([]byte, size)
	if _, err := f.ReadAt(buf, int64(32+off)); err != nil {
		return nil, err
	}
	if crc32.ChecksumIEEE(buf) != want {
		return nil, errors.New("this .7z file is damaged (header checksum mismatch)")
	}

	// Unwrap compressed headers until we reach the plain one.
	for round := 0; ; round++ {
		if len(buf) == 0 || round > 4 {
			return nil, errBadHeader
		}
		if buf[0] == idHeader {
			break
		}
		if buf[0] != idEncodedHeader {
			return nil, errBadHeader
		}
		var si *streamsInfo
		if err := parse(func() { h := &hreader{b: buf, p: 1}; si = h.streamsInfo() }); err != nil {
			return nil, err
		}
		if buf, err = a.readHeaderFolders(si); err != nil {
			return nil, err
		}
	}
	if err := parse(func() { a.header(&hreader{b: buf, p: 1}) }); err != nil {
		return nil, err
	}
	return a, nil
}

// parse runs fn and turns a headerError panic into an error.
func parse(fn func()) (err error) {
	defer func() {
		if r := recover(); r != nil {
			he, ok := r.(headerError)
			if !ok {
				panic(r)
			}
			err = he.err
		}
	}()
	fn()
	return nil
}

// readHeaderFolders decodes every folder of an encoded header into memory.
func (a *archive) readHeaderFolders(si *streamsInfo) ([]byte, error) {
	var out []byte
	for i, f := range si.folders {
		sz := f.size()
		if sz > maxHeaderSize || uint64(len(out))+sz > maxHeaderSize {
			return nil, errBadHeader
		}
		r, err := a.folderReader(si, i)
		if err != nil {
			return nil, err
		}
		b := make([]byte, sz)
		if _, err := io.ReadFull(r, b); err != nil {
			return nil, fmt.Errorf("can't read the archive's header: %w", err)
		}
		if f.hasCRC && crc32.ChecksumIEEE(b) != f.crc {
			return nil, errors.New("this .7z file is damaged (header checksum mismatch)")
		}
		out = append(out, b...)
	}
	return out, nil
}

// ---------------- header structure ----------------

func (a *archive) header(h *hreader) {
	t := h.byte()
	if t == idArchiveProperties {
		h.skipProps()
		t = h.byte()
	}
	if t == idAdditionalStreamsInfo {
		h.streamsInfo() // not used by normal archives
		t = h.byte()
	}
	if t == idMainStreamsInfo {
		a.streams = h.streamsInfo()
		t = h.byte()
	}
	if t == idFilesInfo {
		a.files = h.filesInfo()
		t = h.byte()
	}
	if t != idEnd {
		h.fail()
	}
	a.assignStreams(h)
}

func (h *hreader) streamsInfo() *streamsInfo {
	si := &streamsInfo{}
	t := h.byte()
	if t == idPackInfo {
		si.packPos = h.number()
		n := h.count()
		si.packSizes = make([]uint64, n)
		for {
			t = h.byte()
			if t == idEnd {
				break
			}
			switch t {
			case idSize:
				for i := range si.packSizes {
					si.packSizes[i] = h.number()
				}
			case idCRC:
				h.digests(n) // pack stream CRCs are not needed
			default:
				h.fail()
			}
		}
		t = h.byte()
	}
	if t == idUnpackInfo {
		si.folders = h.unpackInfo()
		t = h.byte()
	}
	// Default: one stream per folder covering the whole folder.
	for _, f := range si.folders {
		f.numStreams = 1
	}
	if t == idSubStreamsInfo {
		h.subStreamsInfo(si)
		t = h.byte()
	} else {
		for _, f := range si.folders {
			si.subSizes = append(si.subSizes, f.size())
			si.subCRC = append(si.subCRC, f.crc)
			si.subHasCRC = append(si.subHasCRC, f.hasCRC)
		}
	}
	if t != idEnd {
		h.fail()
	}
	// Give each folder its range of pack streams.
	next := 0
	for _, f := range si.folders {
		f.firstPack = next
		next += len(f.packed)
	}
	if next > len(si.packSizes) {
		h.fail()
	}
	return si
}

func (h *hreader) unpackInfo() []*folder {
	if h.byte() != idFolder {
		h.fail()
	}
	n := h.count()
	if h.byte() != 0 {
		panic(headerError{errors.New("this .7z stores its folder list outside the header, which RetroPixels can't read")})
	}
	folders := make([]*folder, n)
	for i := range folders {
		folders[i] = h.folder()
	}
	if h.byte() != idCodersUnpackSize {
		h.fail()
	}
	for _, f := range folders {
		total := 0
		for _, c := range f.coders {
			total += c.numOut
		}
		f.unpackSizes = make([]uint64, total)
		for j := range f.unpackSizes {
			f.unpackSizes[j] = h.number()
		}
	}
	for {
		t := h.byte()
		if t == idEnd {
			break
		}
		if t != idCRC {
			h.fail()
		}
		def, crcs := h.digests(n)
		for i, f := range folders {
			f.hasCRC, f.crc = def[i], crcs[i]
		}
	}
	return folders
}

func (h *hreader) folder() *folder {
	f := &folder{}
	nc := h.count()
	if nc == 0 || nc > 64 {
		h.fail()
	}
	totalIn, totalOut := 0, 0
	for i := 0; i < nc; i++ {
		flags := h.byte()
		if flags&0x80 != 0 {
			panic(headerError{errors.New("this .7z uses alternative coder lists, which RetroPixels can't read")})
		}
		c := coderInfo{id: append([]byte(nil), h.bytes(uint64(flags&0x0F))...), numIn: 1, numOut: 1}
		if flags&0x10 != 0 {
			c.numIn = h.count()
			c.numOut = h.count()
			if c.numIn > 64 || c.numOut > 64 {
				h.fail()
			}
		}
		if flags&0x20 != 0 {
			c.props = append([]byte(nil), h.bytes(h.number())...)
		}
		totalIn += c.numIn
		totalOut += c.numOut
		f.coders = append(f.coders, c)
	}
	if totalOut == 0 || totalIn < totalOut-1 {
		h.fail()
	}
	for i := 0; i < totalOut-1; i++ {
		bp := bindPair{in: h.count(), out: h.count()}
		if bp.in >= totalIn || bp.out >= totalOut {
			h.fail()
		}
		f.bindPairs = append(f.bindPairs, bp)
	}
	np := totalIn - (totalOut - 1)
	if np == 1 {
		// The single packed stream is the in-stream no bind pair feeds.
		for i := 0; i < totalIn; i++ {
			if f.bindIn(i) < 0 {
				f.packed = []int{i}
				break
			}
		}
		if len(f.packed) == 0 {
			h.fail()
		}
	} else {
		for i := 0; i < np; i++ {
			k := h.count()
			if k >= totalIn {
				h.fail()
			}
			f.packed = append(f.packed, k)
		}
	}
	return f
}

// bindIn returns the bind pair index feeding in-stream i, or -1.
func (f *folder) bindIn(i int) int {
	for j, bp := range f.bindPairs {
		if bp.in == i {
			return j
		}
	}
	return -1
}

func (h *hreader) subStreamsInfo(si *streamsInfo) {
	t := h.byte()
	if t == idNumUnpackStream {
		for _, f := range si.folders {
			f.numStreams = h.count()
		}
		t = h.byte()
	}
	total := 0
	for _, f := range si.folders {
		total += f.numStreams
		if total > maxCount {
			h.fail()
		}
	}
	hasSizes := t == idSize
	for _, f := range si.folders {
		if f.numStreams == 0 {
			continue
		}
		var sum uint64
		for j := 1; j < f.numStreams; j++ {
			if !hasSizes {
				h.fail()
			}
			s := h.number()
			sum += s
			si.subSizes = append(si.subSizes, s)
		}
		fs := f.size()
		if sum > fs {
			h.fail()
		}
		si.subSizes = append(si.subSizes, fs-sum)
	}
	if hasSizes {
		t = h.byte()
	}

	// CRCs: a folder with one stream and a folder CRC already has its digest;
	// every other stream may get one from the kCRC list.
	si.subCRC = make([]uint32, total)
	si.subHasCRC = make([]bool, total)
	need := 0
	k := 0
	for _, f := range si.folders {
		if f.numStreams == 1 && f.hasCRC {
			si.subCRC[k], si.subHasCRC[k] = f.crc, true
		} else {
			need += f.numStreams
		}
		k += f.numStreams
	}
	for t != idEnd {
		if t == idCRC {
			def, crcs := h.digests(need)
			k, d := 0, 0
			for _, f := range si.folders {
				if f.numStreams == 1 && f.hasCRC {
					k++
					continue
				}
				for j := 0; j < f.numStreams; j++ {
					si.subHasCRC[k], si.subCRC[k] = def[d], crcs[d]
					k++
					d++
				}
			}
		} else {
			h.bytes(h.number())
		}
		t = h.byte()
	}
}

func (h *hreader) filesInfo() []File {
	n := h.count()
	if n > h.left() {
		h.fail()
	}
	files := make([]File, n)
	var emptyStream, emptyFile, anti []bool
	numEmpty := 0
	for {
		t := h.byte()
		if t == idEnd {
			break
		}
		size := h.number()
		p := &hreader{b: h.bytes(size)}
		switch t {
		case idEmptyStream:
			emptyStream = p.bits(n)
			numEmpty = 0
			for _, e := range emptyStream {
				if e {
					numEmpty++
				}
			}
		case idEmptyFile:
			emptyFile = p.bits(numEmpty)
		case idAnti:
			anti = p.bits(numEmpty)
		case idName:
			if p.byte() != 0 {
				panic(headerError{errors.New("this .7z stores file names outside the header, which RetroPixels can't read")})
			}
			rest := p.bytes(uint64(p.left()))
			if len(rest)%2 != 0 {
				p.fail()
			}
			u := make([]uint16, len(rest)/2)
			for i := range u {
				u[i] = binary.LittleEndian.Uint16(rest[2*i:])
			}
			start, k := 0, 0
			for i, c := range u {
				if c != 0 {
					continue
				}
				if k >= n {
					p.fail()
				}
				files[k].Name = string(utf16.Decode(u[start:i]))
				k++
				start = i + 1
			}
			if k != n {
				p.fail()
			}
		case idMTime:
			def := p.defined(n)
			if p.byte() != 0 {
				p.fail()
			}
			for i := range files {
				if def[i] {
					files[i].ModTime = filetime(p.uint64())
				}
			}
		case idWinAttributes:
			def := p.defined(n)
			if p.byte() != 0 {
				p.fail()
			}
			for i := range files {
				if def[i] {
					files[i].Attrib = p.uint32()
				}
			}
		default:
			// Creation/access times, comments, start positions and
			// padding are not needed.
		}
	}
	e := 0
	for i := range files {
		f := &files[i]
		f.hasStream = true
		if emptyStream != nil && emptyStream[i] {
			f.hasStream = false
			isFile := emptyFile != nil && emptyFile[e]
			f.IsDir = !isFile
			f.anti = anti != nil && anti[e]
			e++
		}
		if f.Attrib&0x10 != 0 && !f.hasStream {
			f.IsDir = true
		}
	}
	return files
}

// filetime converts a Windows FILETIME (100 ns ticks since 1601) to time.Time.
func filetime(ft uint64) time.Time {
	const epochDiff = 116444736000000000 // 1601 to 1970 in 100 ns ticks
	if ft < epochDiff/2 {
		return time.Time{}
	}
	t := int64(ft) - epochDiff
	return time.Unix(t/1e7, t%1e7*100)
}

// assignStreams gives every file with data its folder, size and CRC.
func (a *archive) assignStreams(h *hreader) {
	si := a.streams
	if si == nil {
		si = &streamsInfo{}
		a.streams = si
	}
	fi, sub, left := 0, 0, 0 // left: streams not yet used in folder fi
	if len(si.folders) > 0 {
		left = si.folders[0].numStreams
	}
	for i := range a.files {
		f := &a.files[i]
		if !f.hasStream {
			continue
		}
		for left == 0 && fi < len(si.folders) {
			fi++
			if fi < len(si.folders) {
				left = si.folders[fi].numStreams
			}
		}
		if fi >= len(si.folders) || sub >= len(si.subSizes) {
			h.fail()
		}
		f.folder = fi
		f.Size = si.subSizes[sub]
		f.crc, f.hasCRC = si.subCRC[sub], si.subHasCRC[sub]
		sub++
		left--
	}
	if sub != len(si.subSizes) {
		h.fail()
	}
}

// ---------------- folder decoding ----------------

// Method IDs.
var methodNames = map[string]string{
	"00":       "Copy",
	"03":       "Delta",
	"0a":       "ARM64",
	"0b":       "RISC-V",
	"21":       "LZMA2",
	"030101":   "LZMA",
	"03030103": "BCJ",
	"0303011b": "BCJ2",
	"03030205": "PPC",
	"03030401": "IA64",
	"03030501": "ARM",
	"03030701": "ARMT",
	"03030805": "SPARC",
	"030401":   "PPMd",
	"040108":   "Deflate",
	"040109":   "Deflate64",
	"040202":   "BZip2",
	"04f71101": "Zstandard",
	"04f71102": "Brotli",
	"04f71104": "LZ4",
	"06f10701": "AES-256",
}

type unsupportedMethodError struct{ id string }

func (e unsupportedMethodError) Error() string {
	name := e.id
	if n, ok := methodNames[e.id]; ok {
		name = n + " (" + e.id + ")"
	}
	return "this .7z uses a compression method RetroPixels can't unpack: " + name
}

// ErrEncrypted is returned for password-protected archives.
var ErrEncrypted = errors.New("this .7z is password-protected (encrypted); RetroPixels can't unpack encrypted archives")

// folderReader returns a reader of folder fi's final output.
func (a *archive) folderReader(si *streamsInfo, fi int) (io.Reader, error) {
	f := si.folders[fi]
	// Offsets of every pack stream in the file.
	off := int64(32) + int64(si.packPos)
	for i := 0; i < f.firstPack; i++ {
		off += int64(si.packSizes[i])
	}
	packOff := make([]int64, len(f.packed))
	for i := range f.packed {
		packOff[i] = off
		off += int64(si.packSizes[f.firstPack+i])
	}
	if off > a.size || off < 0 {
		return nil, errors.New("this .7z file is incomplete (data is missing from the end)")
	}

	// Where each coder's streams start in the folder-wide numbering.
	inBase := make([]int, len(f.coders))
	outBase := make([]int, len(f.coders))
	ni, no := 0, 0
	for i, c := range f.coders {
		inBase[i], outBase[i] = ni, no
		ni += c.numIn
		no += c.numOut
	}
	for _, c := range f.coders {
		if bytes.Equal(c.id, []byte{0x06, 0xF1, 0x07, 0x01}) {
			return nil, ErrEncrypted
		}
	}

	building := make([]bool, len(f.coders))
	var outStream func(o int) (io.Reader, error)
	inStream := func(i int) (io.Reader, error) {
		for k, p := range f.packed {
			if p == i {
				return io.NewSectionReader(a.f, packOff[k], int64(si.packSizes[f.firstPack+k])), nil
			}
		}
		if j := f.bindIn(i); j >= 0 {
			return outStream(f.bindPairs[j].out)
		}
		return nil, errBadHeader
	}
	outStream = func(o int) (io.Reader, error) {
		ci := -1
		for i, c := range f.coders {
			if o >= outBase[i] && o < outBase[i]+c.numOut {
				ci = i
			}
		}
		if ci < 0 || building[ci] {
			return nil, errBadHeader
		}
		c := f.coders[ci]
		if c.numOut != 1 {
			return nil, unsupportedMethodError{hex.EncodeToString(c.id)}
		}
		building[ci] = true
		ins := make([]io.Reader, c.numIn)
		for k := range ins {
			r, err := inStream(inBase[ci] + k)
			if err != nil {
				return nil, err
			}
			ins[k] = r
		}
		size := f.unpackSizes[o]
		r, err := newCoder(c, ins, size)
		if err != nil {
			return nil, err
		}
		return &exactReader{r: r, left: int64(size)}, nil
	}
	o, err := f.mainOut()
	if err != nil {
		return nil, err
	}
	return outStream(o)
}

// newCoder builds the decoder for one coder.
func newCoder(c coderInfo, in []io.Reader, size uint64) (io.Reader, error) {
	id := hex.EncodeToString(c.id)
	if size > 1<<50 {
		return nil, errBadHeader
	}
	if id != "0303011b" && len(in) != 1 {
		return nil, unsupportedMethodError{id}
	}
	switch id {
	case "00":
		return in[0], nil
	case "030101":
		return newLZMAReader(in[0], c.props, int64(size))
	case "21":
		return newLZMA2Reader(in[0], c.props, int64(size))
	case "03030103":
		x := &x86Converter{lastSeen: -5}
		if len(c.props) == 4 {
			x.pos = binary.LittleEndian.Uint32(c.props)
		}
		return newFilterReader(in[0], x), nil
	case "0a":
		a := &arm64Converter{}
		if len(c.props) == 4 {
			a.pos = binary.LittleEndian.Uint32(c.props)
		}
		return newFilterReader(in[0], a), nil
	case "03":
		d := &deltaConverter{dist: 1}
		if len(c.props) >= 1 {
			d.dist = int(c.props[0]) + 1
		}
		return newFilterReader(in[0], d), nil
	case "0303011b":
		return newBCJ2Reader(in, int64(size))
	case "040108":
		return flate.NewReader(in[0]), nil
	case "040202":
		return bzip2.NewReader(in[0]), nil
	case "06f10701":
		return nil, ErrEncrypted
	}
	return nil, unsupportedMethodError{id}
}

// exactReader passes through exactly left bytes and reports a short stream.
type exactReader struct {
	r    io.Reader
	left int64
}

func (e *exactReader) Read(p []byte) (int, error) {
	if e.left <= 0 {
		return 0, io.EOF
	}
	if int64(len(p)) > e.left {
		p = p[:e.left]
	}
	n, err := e.r.Read(p)
	e.left -= int64(n)
	if err == io.EOF {
		if e.left > 0 {
			return n, errors.New("compressed data ends early")
		}
		err = nil
	}
	return n, err
}
