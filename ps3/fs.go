package ps3

// The file system the game sees, mapped onto the installed game's folder
// and RetroPixels' save folder:
//
//	/dev_hdd0/game/<title>/  -> the game folder (PARAM.SFO, USRDIR, ...)
//	/app_home/               -> the game folder's USRDIR
//	/dev_hdd0/, /dev_hdd1/   -> folders under the save folder
//
// PSN data files (.EDAT) are decrypted as they're read.

import (
	"bytes"
	"io"
	"os"
	"path/filepath"
	"sort"
	"strings"
)

type fsFile struct {
	path   string
	f      *os.File
	r      io.ReaderAt // EDAT
	size   int64
	pos    int64
	write  bool
	dir    []os.DirEntry
	dirPos int
	isDir  bool
}

type fsState struct {
	m      *Machine
	files  map[uint32]*fsFile
	nextFD uint32
}

func newFS(m *Machine) *fsState { return &fsState{m: m, files: map[uint32]*fsFile{}, nextFD: 3} }

func (m *Machine) titleID() string {
	if id := m.sfoString("TITLE_ID"); id != "" {
		return id
	}
	return "NPEB00149"
}

// hostPath maps a PS3 path to a host path ("" when it's nowhere).
func (m *Machine) hostPath(p string) string {
	p = strings.ReplaceAll(p, "\\", "/")
	clean := func(rest string) string {
		rest = filepath.Clean("/" + rest)
		return filepath.FromSlash(strings.TrimPrefix(rest, "/"))
	}
	lower := strings.ToLower(p)
	game := "/dev_hdd0/game/" + strings.ToLower(m.titleID())
	switch {
	case strings.HasPrefix(lower, game):
		return filepath.Join(m.GameDir, clean(p[len(game):]))
	case strings.HasPrefix(lower, "/app_home"):
		return filepath.Join(m.GameDir, "USRDIR", clean(p[len("/app_home"):]))
	case strings.HasPrefix(lower, "/host_root"):
		return filepath.Join(m.GameDir, "USRDIR", clean(p[len("/host_root"):]))
	case strings.HasPrefix(lower, "/dev_hdd0"):
		return filepath.Join(m.saveRoot(), "dev_hdd0", clean(p[len("/dev_hdd0"):]))
	case strings.HasPrefix(lower, "/dev_hdd1"):
		return filepath.Join(m.saveRoot(), "dev_hdd1", clean(p[len("/dev_hdd1"):]))
	}
	return ""
}

func (m *Machine) saveRoot() string {
	if m.SaveDir != "" {
		return m.SaveDir
	}
	return filepath.Join(m.GameDir, "..", "ps3-hdd")
}

// fsError turns a host error into an LV2 one.
func fsError(err error) uint32 {
	switch {
	case err == nil:
		return 0
	case os.IsNotExist(err):
		return eNOENT
	case os.IsExist(err):
		return eEXIST
	case os.IsPermission(err):
		return ePERM
	}
	return eIO
}

// findCase finds a path ignoring case (the PS3's file system is used by
// games as if it didn't care, and the game folder may be on a Linux disk).
func findCase(p string) string {
	if _, err := os.Stat(p); err == nil {
		return p
	}
	dir, base := filepath.Split(p)
	dir = filepath.Clean(dir)
	if dir != p && dir != "." && dir != string(filepath.Separator) {
		dir = findCase(dir)
	}
	ents, err := os.ReadDir(dir)
	if err != nil {
		return p
	}
	for _, e := range ents {
		if strings.EqualFold(e.Name(), base) {
			return filepath.Join(dir, e.Name())
		}
	}
	return filepath.Join(dir, base)
}

func (fs *fsState) open(path string, flags uint32) (uint32, uint32) {
	m := fs.m
	hp := m.hostPath(path)
	if hp == "" {
		m.Logf("open %s: not a place RetroPixels has", path)
		return 0, eNOENT
	}
	hp = findCase(hp)
	acc := flags & 3
	of := os.O_RDONLY
	switch acc {
	case 1:
		of = os.O_WRONLY
	case 2:
		of = os.O_RDWR
	}
	if flags&0x40 != 0 {
		of |= os.O_CREATE
		os.MkdirAll(filepath.Dir(hp), 0o755)
	}
	if flags&0x80 != 0 {
		of |= os.O_EXCL
	}
	if flags&0x200 != 0 {
		of |= os.O_TRUNC
	}
	if flags&0x400 != 0 {
		of |= os.O_APPEND
	}
	if st, err := os.Stat(hp); err == nil && st.IsDir() {
		return 0, eISDIR
	}
	f, err := os.OpenFile(hp, of, 0o644)
	if err != nil {
		m.Logf("open %s (%s): %v", path, hp, err)
		return 0, fsError(err)
	}
	st, _ := f.Stat()
	ff := &fsFile{path: path, f: f, size: st.Size(), write: acc != 0}
	// PSN data files: decrypt on the fly
	if acc == 0 && st.Size() >= 0x100 {
		h := make([]byte, 4)
		f.ReadAt(h, 0)
		if bytes.Equal(h, []byte("NPD\x00")) {
			if m.klic == nil && m.mainELF != nil {
				hdr := make([]byte, 0x80)
				f.ReadAt(hdr, 0)
				if k, ok := FindKlicensee(m.mainELF, hdr); ok {
					m.klic = k
				}
			}
			e, err := OpenEDAT(f, m.klic)
			if err != nil {
				m.Logf("open %s: %v", path, err)
				f.Close()
				return 0, eIO
			}
			ff.r = e
			ff.size = e.Size
		}
	}
	fd := fs.nextFD
	fs.nextFD++
	fs.files[fd] = ff
	m.Logf("open %s -> fd %d (%d bytes)", path, fd, ff.size)
	return fd, 0
}

func (fs *fsState) read(fd uint32, buf uint32, n uint64) (uint64, uint32) {
	f, ok := fs.files[fd]
	if !ok || f.isDir {
		return 0, eBADF
	}
	if f.pos >= f.size {
		return 0, 0
	}
	n = min(n, uint64(f.size-f.pos))
	data := make([]byte, n)
	var got int
	var err error
	if f.r != nil {
		got, err = f.r.ReadAt(data, f.pos)
	} else {
		got, err = f.f.ReadAt(data, f.pos)
	}
	if err != nil && err != io.EOF {
		return 0, eIO
	}
	fs.m.Mem.WriteBytes(buf, data[:got])
	f.pos += int64(got)
	return uint64(got), 0
}

func (fs *fsState) write(fd uint32, buf uint32, n uint64) (uint64, uint32) {
	f, ok := fs.files[fd]
	if !ok || f.isDir || !f.write {
		return 0, eBADF
	}
	data := make([]byte, n)
	fs.m.Mem.ReadBytes(buf, data)
	w, err := f.f.WriteAt(data, f.pos)
	if err != nil {
		return uint64(w), eIO
	}
	f.pos += int64(w)
	if f.pos > f.size {
		f.size = f.pos
	}
	return uint64(w), 0
}

func (fs *fsState) seek(fd uint32, off int64, whence uint32) (uint64, uint32) {
	f, ok := fs.files[fd]
	if !ok {
		return 0, eBADF
	}
	var p int64
	switch whence {
	case 0:
		p = off
	case 1:
		p = f.pos + off
	case 2:
		p = f.size + off
	default:
		return 0, eINVAL
	}
	if p < 0 {
		return 0, eINVAL
	}
	f.pos = p
	return uint64(p), 0
}

func (fs *fsState) close(fd uint32) uint32 {
	f, ok := fs.files[fd]
	if !ok {
		return eBADF
	}
	if f.f != nil {
		f.f.Close()
	}
	delete(fs.files, fd)
	return 0
}

// writeStat fills a CellFsStat.
func (m *Machine) writeStat(a uint32, isDir bool, size int64, mtime int64) {
	mode := uint32(0o100666)
	if isDir {
		mode = 0o040777
	}
	m.Mem.Write32(a, mode)
	m.Mem.Write32(a+4, 0)
	m.Mem.Write32(a+8, 0)
	m.Mem.Write64(a+0x0C, uint64(mtime))
	m.Mem.Write64(a+0x14, uint64(mtime))
	m.Mem.Write64(a+0x1C, uint64(mtime))
	m.Mem.Write64(a+0x24, uint64(size))
	m.Mem.Write64(a+0x2C, 512)
}

func (fs *fsState) stat(path string, a uint32) uint32 {
	m := fs.m
	hp := m.hostPath(path)
	if hp == "" {
		return eNOENT
	}
	hp = findCase(hp)
	st, err := os.Stat(hp)
	if err != nil {
		m.Logf("stat %s: not found", path)
		return eNOENT
	}
	size := st.Size()
	if !st.IsDir() && size >= 0x100 {
		if f, err := os.Open(hp); err == nil {
			h := make([]byte, 0x90)
			f.ReadAt(h, 0)
			f.Close()
			if bytes.Equal(h[:4], []byte("NPD\x00")) {
				size = int64(be.Uint64(h[0x88:]))
			}
		}
	}
	m.writeStat(a, st.IsDir(), size, st.ModTime().Unix())
	return 0
}

func init() {
	reg("cellFsOpen", func(m *Machine, t *Thread) { // (path, flags, fd*, arg, size)
		path := m.Mem.String(t.arg(0), 1024)
		fd, err := m.fs.open(path, t.arg(1))
		if err == 0 {
			m.Mem.Write32(t.arg(2), fd)
		}
		t.ret(err)
	})
	reg("cellFsRead", func(m *Machine, t *Thread) { // (fd, buf, n, nread*)
		n, err := m.fs.read(t.arg(0), t.arg(1), t.arg64(2))
		if t.arg(3) != 0 {
			m.Mem.Write64(t.arg(3), n)
		}
		t.ret(err)
	})
	reg("cellFsWrite", func(m *Machine, t *Thread) {
		n, err := m.fs.write(t.arg(0), t.arg(1), t.arg64(2))
		if t.arg(3) != 0 {
			m.Mem.Write64(t.arg(3), n)
		}
		t.ret(err)
	})
	reg("cellFsClose", func(m *Machine, t *Thread) { t.ret(m.fs.close(t.arg(0))) })
	reg("cellFsLseek", func(m *Machine, t *Thread) { // (fd, offset, whence, pos*)
		p, err := m.fs.seek(t.arg(0), int64(t.arg64(1)), t.arg(2))
		if err == 0 && t.arg(3) != 0 {
			m.Mem.Write64(t.arg(3), p)
		}
		t.ret(err)
	})
	reg("cellFsFstat", func(m *Machine, t *Thread) {
		f, ok := m.fs.files[t.arg(0)]
		if !ok {
			t.ret(eBADF)
			return
		}
		m.writeStat(t.arg(1), f.isDir, f.size, m.now().Unix())
		t.ret(0)
	})
	reg("cellFsStat", func(m *Machine, t *Thread) {
		t.ret(m.fs.stat(m.Mem.String(t.arg(0), 1024), t.arg(1)))
	})
	reg("cellFsOpendir", func(m *Machine, t *Thread) { // (path, fd*)
		path := m.Mem.String(t.arg(0), 1024)
		hp := m.hostPath(path)
		if hp == "" {
			t.ret(eNOENT)
			return
		}
		ents, err := os.ReadDir(findCase(hp))
		if err != nil {
			m.Logf("opendir %s: %v", path, err)
			t.ret(fsError(err))
			return
		}
		sort.Slice(ents, func(i, j int) bool { return ents[i].Name() < ents[j].Name() })
		fd := m.fs.nextFD
		m.fs.nextFD++
		m.fs.files[fd] = &fsFile{path: path, dir: ents, isDir: true}
		m.Mem.Write32(t.arg(1), fd)
		t.ret(0)
	})
	reg("cellFsReaddir", func(m *Machine, t *Thread) { // (fd, dirent*, nread*)
		f, ok := m.fs.files[t.arg(0)]
		if !ok || !f.isDir {
			t.ret(eBADF)
			return
		}
		// "." and ".." first, as the console lists them
		names := []string{".", ".."}
		types := []uint8{1, 1}
		for _, e := range f.dir {
			names = append(names, e.Name())
			if e.IsDir() {
				types = append(types, 1)
			} else {
				types = append(types, 2)
			}
		}
		if f.dirPos >= len(names) {
			m.Mem.Write64(t.arg(2), 0)
			t.ret(0)
			return
		}
		d := t.arg(1)
		name := names[f.dirPos]
		m.Mem.Write8(d, types[f.dirPos])
		m.Mem.Write8(d+1, uint8(len(name)))
		m.Mem.WriteString(d+2, name)
		f.dirPos++
		m.Mem.Write64(t.arg(2), 0x102)
		t.ret(0)
	})
	reg("cellFsClosedir", func(m *Machine, t *Thread) { t.ret(m.fs.close(t.arg(0))) })
	reg("cellFsMkdir", func(m *Machine, t *Thread) {
		hp := m.hostPath(m.Mem.String(t.arg(0), 1024))
		if hp == "" {
			t.ret(eNOENT)
			return
		}
		if _, err := os.Stat(hp); err == nil {
			t.ret(eEXIST)
			return
		}
		t.ret(fsError(os.MkdirAll(hp, 0o755)))
	})
	reg("cellFsRmdir", func(m *Machine, t *Thread) {
		hp := m.hostPath(m.Mem.String(t.arg(0), 1024))
		t.ret(fsError(os.Remove(hp)))
	})
	reg("cellFsUnlink", func(m *Machine, t *Thread) {
		hp := m.hostPath(m.Mem.String(t.arg(0), 1024))
		if hp == "" {
			t.ret(eNOENT)
			return
		}
		t.ret(fsError(os.Remove(findCase(hp))))
	})
	reg("cellFsRename", func(m *Machine, t *Thread) {
		a := m.hostPath(m.Mem.String(t.arg(0), 1024))
		b := m.hostPath(m.Mem.String(t.arg(1), 1024))
		t.ret(fsError(os.Rename(findCase(a), b)))
	})
	reg("cellFsChmod", func(m *Machine, t *Thread) { t.ret(0) })
	reg("cellFsStReadFinish", func(m *Machine, t *Thread) { t.ret(0) })

	// the same through system calls
	sysc(801, func(m *Machine, t *Thread) { // sys_fs_open(path, flags, fd*, mode, arg, size)
		fd, err := m.fs.open(m.Mem.String(t.arg(0), 1024), t.arg(1))
		if err == 0 {
			m.Mem.Write32(t.arg(2), fd)
		}
		t.ret(err)
	})
	sysc(802, func(m *Machine, t *Thread) {
		n, err := m.fs.read(t.arg(0), t.arg(1), t.arg64(2))
		if t.arg(3) != 0 {
			m.Mem.Write64(t.arg(3), n)
		}
		t.ret(err)
	})
	sysc(803, func(m *Machine, t *Thread) {
		n, err := m.fs.write(t.arg(0), t.arg(1), t.arg64(2))
		if t.arg(3) != 0 {
			m.Mem.Write64(t.arg(3), n)
		}
		t.ret(err)
	})
	sysc(804, func(m *Machine, t *Thread) { t.ret(m.fs.close(t.arg(0))) })
	sysc(818, func(m *Machine, t *Thread) {
		p, err := m.fs.seek(t.arg(0), int64(t.arg64(1)), t.arg(2))
		if err == 0 && t.arg(3) != 0 {
			m.Mem.Write64(t.arg(3), p)
		}
		t.ret(err)
	})
}

// ---- PARAM.SFO

func (m *Machine) sfo() map[string]any {
	if v, ok := m.misc["sfo"].(map[string]any); ok {
		return v
	}
	out := map[string]any{}
	b, err := os.ReadFile(filepath.Join(m.GameDir, "PARAM.SFO"))
	if err == nil {
		out = parseSFO(b)
	}
	m.misc["sfo"] = out
	return out
}

func parseSFO(b []byte) map[string]any {
	out := map[string]any{}
	if len(b) < 20 || string(b[:4]) != "\x00PSF" {
		return out
	}
	le := func(o int) uint32 { return uint32(b[o]) | uint32(b[o+1])<<8 | uint32(b[o+2])<<16 | uint32(b[o+3])<<24 }
	keyT, dataT, n := int(le(8)), int(le(12)), int(le(16))
	for i := 0; i < n; i++ {
		e := 20 + 16*i
		if e+16 > len(b) {
			break
		}
		ko := keyT + int(uint16(b[e])|uint16(b[e+1])<<8)
		fmtv := uint16(b[e+2]) | uint16(b[e+3])<<8
		ln := int(le(e + 4))
		do := dataT + int(le(e+12))
		if ko >= len(b) || do+ln > len(b) {
			break
		}
		k := ko
		for k < len(b) && b[k] != 0 {
			k++
		}
		key := string(b[ko:k])
		switch fmtv {
		case 0x0404:
			out[key] = le(do)
		default:
			s := b[do : do+ln]
			if i := bytes.IndexByte(s, 0); i >= 0 {
				s = s[:i]
			}
			out[key] = string(s)
		}
	}
	return out
}

func (m *Machine) sfoString(k string) string {
	s, _ := m.sfo()[k].(string)
	return s
}
