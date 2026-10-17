package wiiu

// coreinit's file functions: the FS* API (FSOpenFile, FSReadFile, ... and
// their Async versions) and the newer FSA* API, over the title's FileSystem.
//
// Every operation finishes at once. An Async call then reports to the
// program the way Cafe OS does: a message on the queue the program gave, or
// its callback run by the system.

import (
	"io/fs"
	"path"
	"strings"
)

// FS status codes (FS API)
const (
	fsOK          = 0
	fsEnd         = 0xfffffffe // -2
	fsExists      = 0xfffffffb // -5
	fsNotFound    = 0xfffffffa // -6
	fsNotFile     = 0xfffffff9 // -7
	fsNotDir      = 0xfffffff8 // -8
	fsAccessError = 0xfffffff7 // -9
	fsPermission  = 0xfffffff6 // -10
	fsStorageFull = 0xfffffff4 // -12
	fsMediaError  = 0xffffffef // -17
)

// FSA error codes
const (
	fsaEndOfDir   = 0xfffcfffc // -0x30004
	fsaEndOfFile  = 0xfffcfffb // -0x30005
	fsaExists     = 0xfffcffea // -0x30016
	fsaNotFound   = 0xfffcffe9 // -0x30017
	fsaPermission = 0xfffcffe6 // -0x3001a
	fsaNotFile    = 0xfffcffd8 // -0x30028
	fsaNotDir     = 0xfffcffd7 // -0x30029
)

type openFile struct {
	Path  string
	Data  []byte
	Pos   uint32
	Write bool
	Dirty bool
}

type openDir struct {
	Path    string
	Entries []DirEntry
	Next    int
}

// fsState is the open files and folders.
type fsState struct {
	Files map[uint32]*openFile
	Dirs  map[uint32]*openDir
	Next  uint32
	Cwd   map[uint32]string // by client
}

func (m *Machine) fsInit() *fsState {
	if m.fs == nil {
		m.fs = &fsState{Files: map[uint32]*openFile{}, Dirs: map[uint32]*openDir{}, Next: 1, Cwd: map[uint32]string{}}
	}
	return m.fs
}

func (s *fsState) abs(client uint32, p string) string {
	if strings.HasPrefix(p, "/") {
		return cleanPath(p)
	}
	cwd := s.Cwd[client]
	if cwd == "" {
		cwd = "/"
	}
	return cleanPath(path.Join(cwd, p))
}

// fsOpen opens a file; mode is a C fopen mode ("r", "wb", "a+" ...).
func (m *Machine) fsOpen(p, mode string) (uint32, uint32) {
	s := m.fsInit()
	mode = strings.ReplaceAll(mode, "b", "")
	f := &openFile{Path: p}
	switch {
	case strings.HasPrefix(mode, "r"):
		b, err := m.Files.ReadFile(p)
		if err != nil {
			if _, dir, err2 := m.Files.Stat(p); err2 == nil && dir {
				return 0, fsNotFile
			}
			return 0, fsNotFound
		}
		f.Data = append([]byte(nil), b...)
		f.Write = strings.Contains(mode, "+")
	case strings.HasPrefix(mode, "w"):
		f.Write, f.Dirty = true, true
	case strings.HasPrefix(mode, "a"):
		if b, err := m.Files.ReadFile(p); err == nil {
			f.Data = append([]byte(nil), b...)
		}
		f.Pos = uint32(len(f.Data))
		f.Write = true
	default:
		return 0, fsAccessError
	}
	h := s.Next
	s.Next++
	s.Files[h] = f
	return h, fsOK
}

func (m *Machine) fsClose(h uint32) uint32 {
	s := m.fsInit()
	f := s.Files[h]
	if f == nil {
		return fsNotFound
	}
	delete(s.Files, h)
	if f.Dirty {
		if err := m.Files.WriteFile(f.Path, f.Data); err != nil {
			return fsAccessError
		}
	}
	return fsOK
}

func (m *Machine) fsRead(h, buf, n uint32, pos int64) (uint32, uint32) {
	f := m.fsInit().Files[h]
	if f == nil {
		return 0, fsNotFound
	}
	if pos >= 0 {
		f.Pos = uint32(pos)
	}
	if f.Pos >= uint32(len(f.Data)) {
		return 0, fsOK
	}
	k := min(n, uint32(len(f.Data))-f.Pos)
	m.Mem.WriteBytes(buf, f.Data[f.Pos:f.Pos+k])
	f.Pos += k
	return k, fsOK
}

func (m *Machine) fsWrite(h, buf, n uint32, pos int64) (uint32, uint32) {
	f := m.fsInit().Files[h]
	if f == nil {
		return 0, fsNotFound
	}
	if !f.Write {
		return 0, fsPermission
	}
	if pos >= 0 {
		f.Pos = uint32(pos)
	}
	end := f.Pos + n
	if end > uint32(len(f.Data)) {
		f.Data = append(f.Data, make([]byte, end-uint32(len(f.Data)))...)
	}
	m.Mem.ReadBytes(buf, f.Data[f.Pos:end])
	f.Pos = end
	f.Dirty = true
	return n, fsOK
}

// FSStat (0x64 bytes)
func (m *Machine) writeStat(a uint32, size int64, dir bool) {
	mem := m.Mem
	mem.Fill(a, 0, 0x64)
	flags := uint32(0x0c000000) // quota, regular
	if dir {
		flags = 0x80000000 | 0x0c000000
	}
	mem.Write32(a+0x00, flags)
	mem.Write32(a+0x04, 0x666) // read / write for everyone
	mem.Write32(a+0x10, uint32(size))
	mem.Write32(a+0x14, uint32(size+0xfff)&^0xfff)
	t := m.bootTime / (TimerHz / 1000000) // microseconds
	mem.Write64(a+0x24, t)
	mem.Write64(a+0x2c, t)
}

func (m *Machine) fsStat(p string, out uint32) uint32 {
	size, dir, err := m.Files.Stat(p)
	if err != nil {
		return fsNotFound
	}
	m.writeStat(out, size, dir)
	return fsOK
}

func (m *Machine) fsOpenDir(p string) (uint32, uint32) {
	s := m.fsInit()
	ents, err := m.Files.ReadDir(p)
	if err != nil {
		if _, dir, err2 := m.Files.Stat(p); err2 == nil && !dir {
			return 0, fsNotDir
		}
		return 0, fsNotFound
	}
	h := s.Next
	s.Next++
	s.Dirs[h] = &openDir{Path: p, Entries: ents}
	return h, fsOK
}

// fsReadDir fills an FSDirectoryEntry (FSStat + 256-byte name)
func (m *Machine) fsReadDir(h, out uint32) uint32 {
	d := m.fsInit().Dirs[h]
	if d == nil {
		return fsNotFound
	}
	if d.Next >= len(d.Entries) {
		return fsEnd
	}
	e := d.Entries[d.Next]
	d.Next++
	m.writeStat(out, e.Size, e.Dir)
	name := e.Name
	if len(name) > 255 {
		name = name[:255]
	}
	m.Mem.Fill(out+0x64, 0, 256)
	m.Mem.WriteString(out+0x64, name)
	return fsOK
}

func errFS(err error) uint32 {
	if err == nil {
		return fsOK
	}
	if err == fs.ErrNotExist {
		return fsNotFound
	}
	return fsAccessError
}

// fsaCode turns an FS status into the FSA error with the same meaning.
func fsaCode(st uint32) uint32 {
	switch st {
	case fsOK:
		return 0
	case fsEnd:
		return fsaEndOfDir
	case fsNotFound:
		return fsaNotFound
	case fsNotFile:
		return fsaNotFile
	case fsNotDir:
		return fsaNotDir
	case fsExists:
		return fsaExists
	}
	return fsaPermission
}

// fsDone finishes an FS call: the status in r3, or for an Async call, a
// message or callback with it. asyncArg is the argument number of the
// FSAsyncData (-1 for the plain call).
func (m *Machine) fsDone(cpu *CPU, client, block uint32, st uint32, asyncArg int) {
	if asyncArg < 0 {
		ret(cpu, st)
		return
	}
	var ad uint32
	if asyncArg < 8 {
		ad = arg(cpu, asyncArg)
	} else {
		ad = m.Mem.Read32(cpu.GPR[1] + 8 + uint32(asyncArg-8)*4)
	}
	ret(cpu, fsOK)
	if ad == 0 {
		return
	}
	mem := m.Mem
	cb, param, queue := mem.Read32(ad), mem.Read32(ad+4), mem.Read32(ad+8)
	// FSAsyncResult: asyncData, ioMsg, client, block, status
	res := m.fsAsyncResult(block)
	mem.Write32(res+0x00, cb)
	mem.Write32(res+0x04, param)
	mem.Write32(res+0x08, queue)
	mem.Write32(res+0x0c, res)
	mem.Write32(res+0x10, 0)
	mem.Write32(res+0x14, 0)
	mem.Write32(res+0x18, 8) // OS_FUNCTION_TYPE_FS_CMD_ASYNC
	mem.Write32(res+0x1c, client)
	mem.Write32(res+0x20, block)
	mem.Write32(res+0x24, st)
	if queue != 0 {
		m.postMessage(queue, res, 0, 0, 8)
		return
	}
	m.queueCallback(cpu.Core, cb, client, block, st, param)
}

// each command block's FSAsyncResult
func (m *Machine) fsAsyncResult(block uint32) uint32 {
	key := "fsres:" + itoa(block)
	if a, ok := m.perThread[key]; ok {
		return a
	}
	a := m.sysAlloc(0x28, 8)
	if m.perThread == nil {
		m.perThread = map[string]uint32{}
	}
	m.perThread[key] = a
	return a
}

// postMessage puts a message on a queue (from the system).
func (m *Machine) postMessage(q, msg, a0, a1, a2 uint32) bool {
	mem := m.Mem
	size, used, first := mem.Read32(q+mqSize), mem.Read32(q+mqUsed), mem.Read32(q+mqFirst)
	if size == 0 || used >= size {
		return false
	}
	slot := mem.Read32(q+mqMsgs) + (first+used)%size*16
	mem.Write32(slot, msg)
	mem.Write32(slot+4, a0)
	mem.Write32(slot+8, a1)
	mem.Write32(slot+12, a2)
	mem.Write32(q+mqUsed, used+1)
	m.wake(keyMsgRecv | uint64(q))
	return true
}

func init() {
	c := "coreinit"
	ok := func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0) }
	for _, n := range []string{"FSInit", "FSShutdown", "FSSetStateChangeNotification", "FSSetCmdPriority",
		"FSCancelCommand", "FSCancelAllCommands", "FSSetUserData", "FSFlushQuota", "FSFlushQuotaAsync"} {
		reg(c, n, ok)
	}
	reg(c, "FSAddClient", func(m *Machine, cpu *CPU, t *Thread) { m.fsInit(); ret(cpu, 0) })
	reg(c, "FSAddClientEx", func(m *Machine, cpu *CPU, t *Thread) { m.fsInit(); ret(cpu, 0) })
	reg(c, "FSDelClient", ok)
	reg(c, "FSInitCmdBlock", func(m *Machine, cpu *CPU, t *Thread) { m.Mem.Fill(arg(cpu, 0), 0, 0xa80) })
	reg(c, "FSGetCmdPriority", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 16) })
	reg(c, "FSGetUserData", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0) })
	reg(c, "FSGetVolumeState", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 1) }) // ready
	reg(c, "FSGetLastError", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0) })
	reg(c, "FSGetLastErrorCodeForViewer", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0) })
	reg(c, "FSGetErrorCodeForViewer", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0) })
	reg(c, "FSGetAsyncResult", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, m.Mem.Read32(arg(cpu, 0))) })

	// each FS call, with its Async twin (the FSAsyncData comes after the
	// error mask)
	both := func(name string, nargs int, fn func(m *Machine, cpu *CPU) uint32) {
		reg(c, name, func(m *Machine, cpu *CPU, t *Thread) {
			m.fsDone(cpu, arg(cpu, 0), arg(cpu, 1), fn(m, cpu), -1)
		})
		reg(c, name+"Async", func(m *Machine, cpu *CPU, t *Thread) {
			m.fsDone(cpu, arg(cpu, 0), arg(cpu, 1), fn(m, cpu), nargs+1)
		})
	}
	str := func(m *Machine, cpu *CPU, i int) string {
		return m.fsInit().abs(arg(cpu, 0), m.Mem.String(arg(cpu, i), 1024))
	}
	both("FSGetMountSource", 4, func(m *Machine, cpu *CPU) uint32 {
		// (client, block, type, source, errMask): type 0 = the SD card
		if arg(cpu, 2) != 0 || !m.Files.Mounted("/vol/external01") {
			return fsNotFound
		}
		src := arg(cpu, 3)
		m.Mem.Write32(src, 0)
		m.Mem.WriteString(src+4, "/sd")
		return fsOK
	})
	both("FSMount", 5, func(m *Machine, cpu *CPU) uint32 {
		// (client, block, source, target, bytes, errMask)
		target, n := arg(cpu, 3), arg(cpu, 4)
		p := "/vol/external01"
		if uint32(len(p)+1) <= n {
			m.Mem.WriteString(target, p)
		}
		return fsOK
	})
	both("FSUnmount", 3, func(m *Machine, cpu *CPU) uint32 { return fsOK })
	both("FSBindMount", 4, func(m *Machine, cpu *CPU) uint32 { return fsOK })
	both("FSBindUnmount", 3, func(m *Machine, cpu *CPU) uint32 { return fsOK })
	both("FSOpenFile", 5, func(m *Machine, cpu *CPU) uint32 {
		// (client, block, path, mode, handle*, errMask)
		h, st := m.fsOpen(str(m, cpu, 2), m.Mem.String(arg(cpu, 3), 16))
		if st == fsOK {
			m.Mem.Write32(arg(cpu, 4), h)
		}
		return st
	})
	both("FSOpenFileEx", 8, func(m *Machine, cpu *CPU) uint32 {
		// (client, block, path, mode, createMode, openFlag, preallocSize, handle*, errMask)
		h, st := m.fsOpen(str(m, cpu, 2), m.Mem.String(arg(cpu, 3), 16))
		if st == fsOK {
			m.Mem.Write32(arg(cpu, 7), h)
		}
		return st
	})
	both("FSCloseFile", 3, func(m *Machine, cpu *CPU) uint32 { return m.fsClose(arg(cpu, 2)) })
	readOrWrite := func(write, withPos bool) func(m *Machine, cpu *CPU) uint32 {
		return func(m *Machine, cpu *CPU) uint32 {
			// (client, block, buf, size, count, [pos,] handle, flag, errMask)
			buf, size, count := arg(cpu, 2), arg(cpu, 3), arg(cpu, 4)
			pos := int64(-1)
			hi := 5
			if withPos {
				pos = int64(arg(cpu, 5))
				hi = 6
			}
			h := arg(cpu, hi)
			if size == 0 {
				return 0
			}
			var n, st uint32
			if write {
				n, st = m.fsWrite(h, buf, size*count, pos)
			} else {
				n, st = m.fsRead(h, buf, size*count, pos)
			}
			if st != fsOK {
				return st
			}
			return n / size
		}
	}
	both("FSReadFile", 7, readOrWrite(false, false))
	both("FSReadFileWithPos", 8, readOrWrite(false, true))
	both("FSWriteFile", 7, readOrWrite(true, false))
	both("FSWriteFileWithPos", 8, readOrWrite(true, true))
	both("FSGetPosFile", 4, func(m *Machine, cpu *CPU) uint32 {
		f := m.fsInit().Files[arg(cpu, 2)]
		if f == nil {
			return fsNotFound
		}
		m.Mem.Write32(arg(cpu, 3), f.Pos)
		return fsOK
	})
	both("FSSetPosFile", 4, func(m *Machine, cpu *CPU) uint32 {
		f := m.fsInit().Files[arg(cpu, 2)]
		if f == nil {
			return fsNotFound
		}
		f.Pos = arg(cpu, 3)
		return fsOK
	})
	both("FSIsEof", 3, func(m *Machine, cpu *CPU) uint32 {
		f := m.fsInit().Files[arg(cpu, 2)]
		if f == nil {
			return fsNotFound
		}
		if f.Pos >= uint32(len(f.Data)) {
			return fsEnd
		}
		return fsOK
	})
	both("FSGetStatFile", 4, func(m *Machine, cpu *CPU) uint32 {
		f := m.fsInit().Files[arg(cpu, 2)]
		if f == nil {
			return fsNotFound
		}
		m.writeStat(arg(cpu, 3), int64(len(f.Data)), false)
		return fsOK
	})
	both("FSGetStat", 4, func(m *Machine, cpu *CPU) uint32 { return m.fsStat(str(m, cpu, 2), arg(cpu, 3)) })
	both("FSTruncateFile", 3, func(m *Machine, cpu *CPU) uint32 {
		f := m.fsInit().Files[arg(cpu, 2)]
		if f == nil {
			return fsNotFound
		}
		if f.Pos < uint32(len(f.Data)) {
			f.Data = f.Data[:f.Pos]
			f.Dirty = true
		}
		return fsOK
	})
	both("FSFlushFile", 3, func(m *Machine, cpu *CPU) uint32 {
		f := m.fsInit().Files[arg(cpu, 2)]
		if f == nil {
			return fsNotFound
		}
		if f.Dirty {
			m.Files.WriteFile(f.Path, f.Data)
		}
		return fsOK
	})
	both("FSOpenDir", 4, func(m *Machine, cpu *CPU) uint32 {
		h, st := m.fsOpenDir(str(m, cpu, 2))
		if st == fsOK {
			m.Mem.Write32(arg(cpu, 3), h)
		}
		return st
	})
	both("FSReadDir", 4, func(m *Machine, cpu *CPU) uint32 { return m.fsReadDir(arg(cpu, 2), arg(cpu, 3)) })
	both("FSCloseDir", 3, func(m *Machine, cpu *CPU) uint32 {
		delete(m.fsInit().Dirs, arg(cpu, 2))
		return fsOK
	})
	both("FSRewindDir", 3, func(m *Machine, cpu *CPU) uint32 {
		if d := m.fsInit().Dirs[arg(cpu, 2)]; d != nil {
			d.Next = 0
		}
		return fsOK
	})
	both("FSMakeDir", 3, func(m *Machine, cpu *CPU) uint32 {
		p := str(m, cpu, 2)
		if _, _, err := m.Files.Stat(p); err == nil {
			return fsExists
		}
		return errFS(m.Files.MakeDir(p))
	})
	both("FSRemove", 3, func(m *Machine, cpu *CPU) uint32 { return errFS(m.Files.Remove(str(m, cpu, 2))) })
	both("FSRename", 4, func(m *Machine, cpu *CPU) uint32 {
		return errFS(m.Files.Rename(str(m, cpu, 2), str(m, cpu, 3)))
	})
	both("FSChangeDir", 3, func(m *Machine, cpu *CPU) uint32 {
		s := m.fsInit()
		s.Cwd[arg(cpu, 0)] = s.abs(arg(cpu, 0), m.Mem.String(arg(cpu, 2), 1024))
		return fsOK
	})
	both("FSGetCwd", 4, func(m *Machine, cpu *CPU) uint32 {
		cwd := m.fsInit().Cwd[arg(cpu, 0)]
		if cwd == "" {
			cwd = "/"
		}
		if uint32(len(cwd)+1) > arg(cpu, 3) {
			return fsAccessError
		}
		m.Mem.WriteString(arg(cpu, 2), cwd)
		return fsOK
	})
	both("FSGetFreeSpaceSize", 4, func(m *Machine, cpu *CPU) uint32 {
		m.Mem.Write64(arg(cpu, 3), 8<<30)
		return fsOK
	})
	both("FSChangeMode", 5, func(m *Machine, cpu *CPU) uint32 { return fsOK })
	both("FSGetDirSize", 4, func(m *Machine, cpu *CPU) uint32 {
		m.Mem.Write64(arg(cpu, 3), 0)
		return fsOK
	})

	// FSA: client handles are numbers; calls return the status directly
	reg(c, "FSAInit", ok)
	reg(c, "FSAShutdown", ok)
	reg(c, "FSAAddClient", func(m *Machine, cpu *CPU, t *Thread) {
		s := m.fsInit()
		h := s.Next
		s.Next++
		ret(cpu, h)
	})
	reg(c, "FSADelClient", ok)
	reg(c, "FSAGetLastErrorCodeForViewer", ok)
	reg(c, "FSAMount", func(m *Machine, cpu *CPU, t *Thread) {
		// (client, source, target, flags, arg, argSize)
		src := m.Mem.String(arg(cpu, 1), 256)
		if strings.Contains(src, "sdcard") && !m.Files.Mounted("/vol/external01") {
			ret(cpu, fsaNotFound)
			return
		}
		ret(cpu, 0)
	})
	reg(c, "FSAUnmount", ok)
	fsaStr := func(m *Machine, cpu *CPU, i int) string {
		return m.fsInit().abs(arg(cpu, 0), m.Mem.String(arg(cpu, i), 1024))
	}
	fsaOpen := func(handleArg int) hleFunc {
		return func(m *Machine, cpu *CPU, t *Thread) {
			h, st := m.fsOpen(fsaStr(m, cpu, 1), m.Mem.String(arg(cpu, 2), 16))
			if st == fsOK {
				m.Mem.Write32(arg(cpu, handleArg), h)
			}
			ret(cpu, fsaCode(st))
		}
	}
	reg(c, "FSAOpenFile", fsaOpen(3))   // (client, path, mode, handle*)
	reg(c, "FSAOpenFileEx", fsaOpen(6)) // (client, path, mode, createMode, openFlag, prealloc, handle*)
	reg(c, "FSACloseFile", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, fsaCode(m.fsClose(arg(cpu, 1)))) })
	fsaRW := func(write, withPos bool) hleFunc {
		return func(m *Machine, cpu *CPU, t *Thread) {
			// (client, buf, size, count, [pos,] handle, flags)
			buf, size, count := arg(cpu, 1), arg(cpu, 2), arg(cpu, 3)
			pos := int64(-1)
			hi := 4
			if withPos {
				pos, hi = int64(arg(cpu, 4)), 5
			}
			if size == 0 {
				ret(cpu, 0)
				return
			}
			var n, st uint32
			if write {
				n, st = m.fsWrite(arg(cpu, hi), buf, size*count, pos)
			} else {
				n, st = m.fsRead(arg(cpu, hi), buf, size*count, pos)
			}
			if st != fsOK {
				ret(cpu, fsaCode(st))
				return
			}
			ret(cpu, n/size)
		}
	}
	reg(c, "FSAReadFile", fsaRW(false, false))
	reg(c, "FSAReadFileWithPos", fsaRW(false, true))
	reg(c, "FSAWriteFile", fsaRW(true, false))
	reg(c, "FSAWriteFileWithPos", fsaRW(true, true))
	reg(c, "FSAGetStat", func(m *Machine, cpu *CPU, t *Thread) {
		ret(cpu, fsaCode(m.fsStat(fsaStr(m, cpu, 1), arg(cpu, 2))))
	})
	reg(c, "FSAGetStatFile", func(m *Machine, cpu *CPU, t *Thread) {
		f := m.fsInit().Files[arg(cpu, 1)]
		if f == nil {
			ret(cpu, fsaNotFound)
			return
		}
		m.writeStat(arg(cpu, 2), int64(len(f.Data)), false)
		ret(cpu, 0)
	})
	reg(c, "FSASetPosFile", func(m *Machine, cpu *CPU, t *Thread) {
		f := m.fsInit().Files[arg(cpu, 1)]
		if f == nil {
			ret(cpu, fsaNotFound)
			return
		}
		f.Pos = arg(cpu, 2)
		ret(cpu, 0)
	})
	reg(c, "FSAGetPosFile", func(m *Machine, cpu *CPU, t *Thread) {
		f := m.fsInit().Files[arg(cpu, 1)]
		if f == nil {
			ret(cpu, fsaNotFound)
			return
		}
		m.Mem.Write32(arg(cpu, 2), f.Pos)
		ret(cpu, 0)
	})
	reg(c, "FSAIsEof", func(m *Machine, cpu *CPU, t *Thread) {
		f := m.fsInit().Files[arg(cpu, 1)]
		if f == nil {
			ret(cpu, fsaNotFound)
			return
		}
		if f.Pos >= uint32(len(f.Data)) {
			ret(cpu, fsaEndOfFile)
			return
		}
		ret(cpu, 0)
	})
	reg(c, "FSATruncateFile", func(m *Machine, cpu *CPU, t *Thread) {
		f := m.fsInit().Files[arg(cpu, 1)]
		if f == nil {
			ret(cpu, fsaNotFound)
			return
		}
		if f.Pos < uint32(len(f.Data)) {
			f.Data, f.Dirty = f.Data[:f.Pos], true
		}
		ret(cpu, 0)
	})
	reg(c, "FSAFlushFile", func(m *Machine, cpu *CPU, t *Thread) {
		if f := m.fsInit().Files[arg(cpu, 1)]; f != nil && f.Dirty {
			m.Files.WriteFile(f.Path, f.Data)
		}
		ret(cpu, 0)
	})
	reg(c, "FSAOpenDir", func(m *Machine, cpu *CPU, t *Thread) {
		h, st := m.fsOpenDir(fsaStr(m, cpu, 1))
		if st == fsOK {
			m.Mem.Write32(arg(cpu, 2), h)
		}
		ret(cpu, fsaCode(st))
	})
	reg(c, "FSAReadDir", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, fsaCode(m.fsReadDir(arg(cpu, 1), arg(cpu, 2)))) })
	reg(c, "FSACloseDir", func(m *Machine, cpu *CPU, t *Thread) { delete(m.fsInit().Dirs, arg(cpu, 1)); ret(cpu, 0) })
	reg(c, "FSARewindDir", func(m *Machine, cpu *CPU, t *Thread) {
		if d := m.fsInit().Dirs[arg(cpu, 1)]; d != nil {
			d.Next = 0
		}
		ret(cpu, 0)
	})
	reg(c, "FSAMakeDir", func(m *Machine, cpu *CPU, t *Thread) {
		p := fsaStr(m, cpu, 1)
		if _, _, err := m.Files.Stat(p); err == nil {
			ret(cpu, fsaExists)
			return
		}
		ret(cpu, fsaCode(errFS(m.Files.MakeDir(p))))
	})
	reg(c, "FSARemove", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, fsaCode(errFS(m.Files.Remove(fsaStr(m, cpu, 1))))) })
	reg(c, "FSARename", func(m *Machine, cpu *CPU, t *Thread) {
		ret(cpu, fsaCode(errFS(m.Files.Rename(fsaStr(m, cpu, 1), fsaStr(m, cpu, 2)))))
	})
	reg(c, "FSAChangeDir", func(m *Machine, cpu *CPU, t *Thread) {
		s := m.fsInit()
		s.Cwd[arg(cpu, 0)] = fsaStr(m, cpu, 1)
		ret(cpu, 0)
	})
	reg(c, "FSAGetCwd", func(m *Machine, cpu *CPU, t *Thread) {
		cwd := m.fsInit().Cwd[arg(cpu, 0)]
		if cwd == "" {
			cwd = "/"
		}
		m.Mem.WriteString(arg(cpu, 1), cwd)
		ret(cpu, 0)
	})
	reg(c, "FSAGetFreeSpaceSize", func(m *Machine, cpu *CPU, t *Thread) {
		m.Mem.Write64(arg(cpu, 2), 8<<30)
		ret(cpu, 0)
	})
	reg(c, "FSAChangeMode", ok)
}
