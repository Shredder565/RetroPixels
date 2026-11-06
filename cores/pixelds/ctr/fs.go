package ctr

import (
	"encoding/binary"
	"os"
	"path/filepath"
	"strings"
	"unicode/utf16"
)

// FS (fs:USER): archives and files. The SD card is a folder on the
// computer, save data another; the game's RomFS is read from the game.

const (
	archRomFS    = 3
	archSaveData = 4
	archExtSave  = 6
	archSDMC     = 9
	archSDMCW    = 0xa

	rcFSNotFound = 0xc8804478
	rcFSExists   = 0xc82044be
	rcFSDenied   = 0xc8804464
)

type archive struct {
	id   uint32
	root string // host folder ("" for the RomFS)
}

type fsFile struct {
	path  string
	romfs []byte   // a RomFS file's data, or nil
	f     *os.File `snap:"-"` // reopened from host and mode after a save state loads
	host  string
	mode  int
}

func (f *fsFile) name() string { return "file " + f.path }

type fsDir struct {
	entries []os.DirEntry `snap:"-"` // read again from host after a save state loads
	host    string
	pos     int
}

func (d *fsDir) name() string { return "directory" }

type fsService struct {
	n        string
	m        *Machine
	archives map[uint64]*archive
	nextArch uint64
}

func newFS(m *Machine, name string) *fsService {
	return &fsService{n: name, m: m, archives: map[uint64]*archive{}, nextArch: 0x100000000}
}

func (v *fsService) name() string { return v.n }

// fsPath decodes an FS path: type 1 empty, 2 binary, 3 ASCII, 4 UTF-16.
func (m *Machine) fsPath(typ, addr, size uint32) (string, []byte) {
	b := m.mem.readBytes(addr, size)
	switch typ {
	case 3:
		return strings.TrimRight(string(b), "\x00"), b
	case 4:
		u := make([]uint16, 0, len(b)/2)
		for i := 0; i+1 < len(b); i += 2 {
			c := binary.LittleEndian.Uint16(b[i:])
			if c == 0 {
				break
			}
			u = append(u, c)
		}
		return string(utf16.Decode(u)), b
	}
	return "", b
}

func (v *fsService) openArchive(id uint32) (*archive, uint32) {
	m := v.m
	a := &archive{id: id}
	switch id {
	case archSDMC, archSDMCW:
		a.root = m.opts.SDCard
	case archSaveData:
		a.root = m.opts.SaveData
	case archExtSave:
		a.root = filepath.Join(m.opts.SaveData, "extdata")
	case archRomFS:
	default:
		return nil, rcFSNotFound
	}
	if a.root != "" {
		os.MkdirAll(a.root, 0o755)
	}
	return a, rcOK
}

func (a *archive) host(p string) string {
	p = strings.TrimLeft(filepath.FromSlash(p), `/\`)
	clean := filepath.Clean("/" + p)
	return filepath.Join(a.root, clean)
}

func (v *fsService) openFile(a *archive, path string, flags uint32) (*fsFile, uint32) {
	m := v.m
	if a.id == archRomFS {
		if m.romfs == nil {
			return nil, rcFSNotFound
		}
		return &fsFile{path: "romfs", romfs: m.romfs}, rcOK
	}
	if a.root == "" {
		return nil, rcFSNotFound
	}
	hp := a.host(path)
	mode := os.O_RDONLY
	if flags&2 != 0 {
		mode = os.O_RDWR
	}
	if flags&4 != 0 {
		mode |= os.O_CREATE
		os.MkdirAll(filepath.Dir(hp), 0o755)
	}
	f, err := os.OpenFile(hp, mode, 0o644)
	if err != nil {
		return nil, rcFSNotFound
	}
	return &fsFile{path: path, f: f, host: hp, mode: mode &^ os.O_CREATE}, rcOK
}

func (v *fsService) handle(c *ipc) {
	m := c.m
	k := m.kern
	switch c.id() {
	case 0x0801, 0x0861, 0x0862, 0x0863: // Initialize, InitializeWithSdkVersion, SetPriority, GetPriority
		if c.id() == 0x0863 {
			c.push(0)
		}
	case 0x080c: // OpenArchive(id, path type, path size, ..., path) -> archive handle
		a, rc := v.openArchive(c.arg(0))
		if rc != rcOK {
			c.result(rc)
			c.push(0, 0)
			return
		}
		v.nextArch++
		v.archives[v.nextArch] = a
		c.push64(v.nextArch)
	case 0x080e: // CloseArchive
		delete(v.archives, uint64(c.arg(0))|uint64(c.arg(1))<<32)
	case 0x0802: // OpenFile(transaction, archive, path type, path size, flags, attr, ..., path)
		a := v.archives[uint64(c.arg(1))|uint64(c.arg(2))<<32]
		if a == nil {
			c.result(rcInvalidHandle)
			return
		}
		p, _ := m.fsPath(c.arg(3), c.arg(8), c.arg(4))
		f, rc := v.openFile(a, p, c.arg(5))
		if rc != rcOK {
			c.result(rc)
			c.handles(0)
			return
		}
		c.handles(k.add(&session{svc: &fileService{m: m, f: f}}))
	case 0x0803: // OpenFileDirectly(transaction, archive id, apath type, apath size, fpath type, fpath size, flags, attr, ..., apath, ..., fpath)
		a, rc := v.openArchive(c.arg(1))
		if rc != rcOK {
			c.result(rc)
			c.handles(0)
			return
		}
		p, _ := m.fsPath(c.arg(4), c.arg(11), c.arg(5))
		f, rc := v.openFile(a, p, c.arg(6))
		if rc != rcOK {
			c.result(rc)
			c.handles(0)
			return
		}
		c.handles(k.add(&session{svc: &fileService{m: m, f: f}}))
	case 0x0804, 0x0805, 0x0806, 0x0807, 0x0808, 0x0809, 0x080a: // DeleteFile, RenameFile, DeleteDirectory(Recursively), CreateFile, CreateDirectory, RenameDirectory
		a := v.archives[uint64(c.arg(1))|uint64(c.arg(2))<<32]
		if a == nil || a.root == "" {
			c.result(rcFSDenied)
			return
		}
		p, _ := m.fsPath(c.arg(3), c.arg(len3(c.id())), c.arg(4))
		hp := a.host(p)
		var err error
		switch c.id() {
		case 0x0804:
			err = os.Remove(hp)
		case 0x0806, 0x0807:
			err = os.RemoveAll(hp)
		case 0x0808: // CreateFile(…, attributes, size)
			if _, e := os.Stat(hp); e == nil {
				c.result(rcFSExists)
				return
			}
			os.MkdirAll(filepath.Dir(hp), 0o755)
			var f *os.File
			if f, err = os.Create(hp); err == nil {
				f.Truncate(int64(uint64(c.arg(6)) | uint64(c.arg(7))<<32))
				f.Close()
			}
		case 0x0809:
			if _, e := os.Stat(hp); e == nil {
				c.result(rcFSExists)
				return
			}
			err = os.MkdirAll(hp, 0o755)
		}
		if err != nil {
			c.result(rcFSNotFound)
		}
	case 0x080b: // OpenDirectory(archive, path type, path size, ..., path)
		a := v.archives[uint64(c.arg(0))|uint64(c.arg(1))<<32]
		if a == nil || a.root == "" {
			c.result(rcFSNotFound)
			c.handles(0)
			return
		}
		p, _ := m.fsPath(c.arg(2), c.arg(5), c.arg(3))
		es, err := os.ReadDir(a.host(p))
		if err != nil {
			c.result(rcFSNotFound)
			c.handles(0)
			return
		}
		c.handles(k.add(&session{svc: &dirService{m: m, d: &fsDir{entries: es, host: a.host(p)}}}))
	case 0x0814: // GetSdmcArchiveResource -> sector size, cluster size, total, free clusters
		c.push(512, 0x8000, 0x100000, 0x80000)
	case 0x0817, 0x0818: // IsSdmcDetected, IsSdmcWritable
		c.push(1)
	case 0x0821: // CardSlotIsInserted
		c.push(1)
	case 0x0845: // GetFormatInfo -> total size, directories, files, duplicate data
		c.push(0x100000, 0x10, 0x10, 0)
	case 0x084c: // FormatSaveData
	default:
		c.m.logf("fs: command %04x", c.id())
	}
}

// where the path pointer is for the delete / create commands
func len3(id uint32) int {
	switch id {
	case 0x0808:
		return 9
	case 0x0805, 0x080a:
		return 13
	}
	return 6
}

// fileService is a file session.
type fileService struct {
	m *Machine
	f *fsFile
}

func (*fileService) name() string { return "file" }

func (v *fileService) handle(c *ipc) {
	m := c.m
	f := v.f
	switch c.id() {
	case 0x0802: // Read(offset, size, ..., buffer) -> bytes read
		off := int64(uint64(c.arg(0)) | uint64(c.arg(1))<<32)
		n, buf := c.arg(2), c.arg(4)
		var data []byte
		if f.romfs != nil {
			if off < int64(len(f.romfs)) {
				end := off + int64(n)
				if end > int64(len(f.romfs)) {
					end = int64(len(f.romfs))
				}
				data = f.romfs[off:end]
			}
		} else if f.f != nil {
			data = make([]byte, n)
			k, _ := f.f.ReadAt(data, off)
			data = data[:k]
		}
		m.mem.writeBytes(buf, data)
		c.push(uint32(len(data)))
	case 0x0803: // Write(offset, size, flags, ..., buffer) -> bytes written
		off := int64(uint64(c.arg(0)) | uint64(c.arg(1))<<32)
		n, buf := c.arg(2), c.arg(5)
		if f.f == nil {
			c.result(rcFSDenied)
			c.push(0)
			return
		}
		k, _ := f.f.WriteAt(m.mem.readBytes(buf, n), off)
		c.push(uint32(k))
	case 0x0804: // GetSize
		var size int64
		if f.romfs != nil {
			size = int64(len(f.romfs))
		} else if f.f != nil {
			if st, err := f.f.Stat(); err == nil {
				size = st.Size()
			}
		}
		c.push64(uint64(size))
	case 0x0805: // SetSize
		if f.f != nil {
			f.f.Truncate(int64(uint64(c.arg(0)) | uint64(c.arg(1))<<32))
		}
	case 0x0808: // Close
		if f.f != nil {
			f.f.Close()
			f.f = nil
			f.host = "" // closed: not reopened by a save state
		}
	case 0x0809: // Flush
		if f.f != nil {
			f.f.Sync()
		}
	case 0x080c: // OpenLinkFile -> a new session to the same file
		c.handles(m.kern.add(&session{svc: v}))
	}
}

// dirService is a directory session.
type dirService struct {
	m *Machine
	d *fsDir
}

func (*dirService) name() string { return "directory" }

func (v *dirService) handle(c *ipc) {
	m := c.m
	switch c.id() {
	case 0x0801: // Read(count, ..., buffer) -> entries read (0x228 bytes each)
		n, buf := c.arg(0), c.arg(2)
		var got uint32
		for got < n && v.d.pos < len(v.d.entries) {
			e := v.d.entries[v.d.pos]
			v.d.pos++
			ent := make([]byte, 0x228)
			name := utf16.Encode([]rune(e.Name()))
			for i, u := range name {
				if i >= 0x105 {
					break
				}
				binary.LittleEndian.PutUint16(ent[i*2:], u)
			}
			short := strings.ToUpper(e.Name())
			if len(short) > 8 {
				short = short[:8]
			}
			copy(ent[0x20c:], short)
			if e.IsDir() {
				ent[0x21c] = 1
			} else if info, err := e.Info(); err == nil {
				binary.LittleEndian.PutUint64(ent[0x220:], uint64(info.Size()))
			}
			m.mem.writeBytes(buf+got*0x228, ent)
			got++
		}
		c.push(got)
	case 0x0802: // Close
	}
}
