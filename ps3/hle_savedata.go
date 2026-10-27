package ps3

// cellSaveData: the game's saves. Each save is a folder under
// <save folder>/savedata/<dir name>, holding the game's files and PARAM.bin
// (the title/subtitle/detail block the game gives). The load and save calls
// run the game's callbacks the way the console does: an optional "fixed"
// callback picks the folder, the "stat" callback sees what's there, then
// the "file" callback is called until it says it's done, reading or
// writing one file each time.

import (
	"os"
	"path/filepath"
	"sort"
	"strings"
	"time"
)

const (
	sdErrCBResult = 0x8002b401
	sdErrAccess   = 0x8002b402
	sdErrParam    = 0x8002b404

	sdParamSize    = 0x558 // CellSaveDataSystemFileParam
	sdStatGetSize  = 0x5F0
	sdFileStatSize = 0x30
	sdDirListSize  = 0x30
)

// sdScratch is where the structures passed to the callbacks live.
type sdScratch struct {
	base                                uint32
	cbResult, statGet, statSet, fileGet uint32
	fileSet, listGet, fixedSet, dirList uint32
	fileList                            uint32
}

func (m *Machine) savedataScratch() *sdScratch {
	if m.sd == nil {
		b := m.sysAlloc(0x10000, 128)
		m.sd = &sdScratch{
			base: b, cbResult: b, statGet: b + 0x100, statSet: b + 0x700, fileGet: b + 0x740,
			fileSet: b + 0x7C0, listGet: b + 0x800, fixedSet: b + 0x880,
			fileList: b + 0x900,  // 64 entries
			dirList:  b + 0x1600, // up to 256 entries (0x3000 bytes)
		}
	}
	return m.sd
}

func (m *Machine) saveFolder(dir string) string {
	return filepath.Join(m.saveRoot(), "savedata", filepath.Base(filepath.Clean("/"+dir)))
}

// savedataOp runs one load or save. fixed is the callback that picks the
// folder (0 for the Auto calls, which name it in dirName).
type sdOp struct {
	save                      bool
	dirName                   string
	prefix                    string
	setBuf, fixed, stat, file uint32
	userdata                  uint32
}

func (m *Machine) savedataRun(t *Thread, op sdOp) {
	sc := m.savedataScratch()
	mem := m.Mem
	clear := func(a, n uint32) { mem.Fill(a, 0, n) }
	finish := func(r uint32) { t.ret(r) }
	result := func() int32 { return int32(mem.Read32(sc.cbResult)) }
	resetCB := func() {
		clear(sc.cbResult, 0x20)
		mem.Write32(sc.cbResult+16, op.userdata)
	}

	var afterDir func()
	if op.fixed != 0 {
		// list the folders whose names start with the prefix
		var names []string
		ents, _ := os.ReadDir(filepath.Join(m.saveRoot(), "savedata"))
		for _, e := range ents {
			if e.IsDir() && strings.HasPrefix(e.Name(), op.prefix) {
				names = append(names, e.Name())
			}
		}
		sort.Strings(names)
		max := mem.Read32(op.setBuf)
		if max > 256 {
			max = 256
		}
		n := uint32(len(names))
		if n > max {
			n = max
		}
		clear(sc.listGet, 0x50)
		mem.Write32(sc.listGet, uint32(len(names)))
		mem.Write32(sc.listGet+4, n)
		mem.Write32(sc.listGet+8, sc.dirList)
		for i := uint32(0); i < n; i++ {
			e := sc.dirList + i*sdDirListSize
			clear(e, sdDirListSize)
			mem.WriteString(e, names[i])
		}
		clear(sc.fixedSet, 16)
		resetCB()
		m.callGuest(t, op.fixed, []uint64{uint64(sc.cbResult), uint64(sc.listGet), uint64(sc.fixedSet)}, func(uint64) {
			if result() < 0 {
				finish(sdErrCBResult)
				return
			}
			d := mem.Read32(sc.fixedSet)
			if d == 0 {
				finish(sdErrParam)
				return
			}
			op.dirName = mem.String(d, 32)
			afterDir()
		})
	}

	afterDir = func() {
		folder := m.saveFolder(op.dirName)
		_, err := os.Stat(folder)
		isNew := err != nil
		m.Logf("savedata %s %q (new %v)", map[bool]string{false: "load", true: "save"}[op.save], op.dirName, isNew)

		// stat
		g := sc.statGet
		clear(g, sdStatGetSize)
		mem.Write32(g, 1<<20) // free space (KB)
		if isNew {
			mem.Write32(g+4, 1)
		}
		now := uint64(time.Now().Unix())
		for i := uint32(0); i < 3; i++ {
			mem.Write64(g+8+8*i, now)
		}
		mem.WriteString(g+0x20, op.dirName)
		if p, err := os.ReadFile(filepath.Join(folder, "PARAM.bin")); err == nil && len(p) == sdParamSize {
			mem.WriteBytes(g+0x40, p)
		}
		var files []os.DirEntry
		if !isNew {
			ents, _ := os.ReadDir(folder)
			for _, e := range ents {
				if !e.IsDir() && e.Name() != "PARAM.bin" {
					files = append(files, e)
				}
			}
		}
		total := uint32(0)
		maxFiles := mem.Read32(op.setBuf + 4)
		if maxFiles > 64 {
			maxFiles = 64
		}
		listed := uint32(0)
		for _, f := range files {
			info, _ := f.Info()
			size := uint64(0)
			if info != nil {
				size = uint64(info.Size())
			}
			total += uint32((size + 1023) / 1024)
			if listed < maxFiles {
				e := sc.fileList + listed*sdFileStatSize
				clear(e, sdFileStatSize)
				typ := uint32(0) // secure file
				switch strings.ToUpper(f.Name()) {
				case "ICON0.PNG":
					typ = 2
				case "ICON1.PAM":
					typ = 3
				case "PIC1.PNG":
					typ = 4
				case "SND0.AT3":
					typ = 5
				}
				mem.Write32(e, typ)
				mem.Write64(e+8, size)
				mem.WriteString(e+0x28, f.Name())
				listed++
			}
		}
		mem.Write32(g+0x598, 0) // bind
		mem.Write32(g+0x59C, total+1)
		mem.Write32(g+0x5A0, 1)
		mem.Write32(g+0x5A4, uint32(len(files)))
		mem.Write32(g+0x5A8, listed)
		mem.Write32(g+0x5AC, sc.fileList)
		clear(sc.statSet, 16)
		resetCB()
		m.callGuest(t, op.stat, []uint64{uint64(sc.cbResult), uint64(g), uint64(sc.statSet)}, func(uint64) {
			r := result()
			if r < 0 {
				finish(sdErrCBResult)
				return
			}
			if p := mem.Read32(sc.statSet); p != 0 && op.save {
				buf := make([]byte, sdParamSize)
				mem.ReadBytes(p, buf)
				os.MkdirAll(folder, 0o755)
				os.WriteFile(filepath.Join(folder, "PARAM.bin"), buf, 0o644)
			}
			if r == 1 || r == 2 { // OK_LAST
				finish(0)
				return
			}
			m.savedataFiles(t, op, folder, finish)
		})
	}

	if op.fixed == 0 {
		afterDir()
	}
}

// savedataFiles calls the file callback until it's done.
func (m *Machine) savedataFiles(t *Thread, op sdOp, folder string, finish func(uint32)) {
	sc := m.sd
	mem := m.Mem
	var step func()
	step = func() {
		mem.Fill(sc.cbResult, 0, 0x20)
		mem.Write32(sc.cbResult+16, op.userdata)
		mem.Fill(sc.fileSet, 0, 0x30)
		m.callGuest(t, op.file, []uint64{uint64(sc.cbResult), uint64(sc.fileGet), uint64(sc.fileSet)}, func(uint64) {
			r := int32(mem.Read32(sc.cbResult))
			if r < 0 {
				finish(sdErrCBResult)
				return
			}
			if r == 1 || r == 2 {
				finish(0)
				return
			}
			fs := sc.fileSet
			opn := mem.Read32(fs)
			typ := mem.Read32(fs + 8)
			name := ""
			switch typ {
			case 0, 1:
				if p := mem.Read32(fs + 28); p != 0 {
					name = mem.String(p, 13)
				}
			case 2:
				name = "ICON0.PNG"
			case 3:
				name = "ICON1.PAM"
			case 4:
				name = "PIC1.PNG"
			case 5:
				name = "SND0.AT3"
			}
			off := mem.Read32(fs + 32)
			size := mem.Read32(fs + 36)
			bufSize := mem.Read32(fs + 40)
			buf := mem.Read32(fs + 44)
			if size > bufSize {
				size = bufSize
			}
			path := filepath.Join(folder, filepath.Base(filepath.Clean("/"+name)))
			mem.Fill(sc.fileGet, 0, 0x44)
			var exc uint32
			switch opn {
			case 0: // read
				data, err := os.ReadFile(path)
				if err != nil {
					finish(sdErrAccess)
					return
				}
				if int(off) < len(data) {
					data = data[off:]
					if uint32(len(data)) > size {
						data = data[:size]
					}
					mem.WriteBytes(buf, data)
					exc = uint32(len(data))
				}
			case 1, 3: // write (3: without truncating)
				os.MkdirAll(folder, 0o755)
				data := make([]byte, size)
				mem.ReadBytes(buf, data)
				flags := os.O_WRONLY | os.O_CREATE
				if opn == 1 {
					flags |= os.O_TRUNC
				}
				f, err := os.OpenFile(path, flags, 0o644)
				if err != nil {
					finish(sdErrAccess)
					return
				}
				f.WriteAt(data, int64(off))
				f.Close()
				exc = size
			case 2: // delete
				os.Remove(path)
			}
			mem.Write32(sc.fileGet, exc)
			step()
		})
	}
	step()
}

// arg8 is a function's ninth argument (on the stack).
func (t *Thread) arg8(m *Machine) uint32 { return m.Mem.Read32(uint32(t.GPR[1]) + 0x74) }

func init() {
	auto := func(save bool) hleFunc {
		// (version, userId, dirName, errDialog, setBuf, funcStat, funcFile, container, userdata)
		return func(m *Machine, t *Thread) {
			m.savedataRun(t, sdOp{save: save, dirName: m.Mem.String(t.arg(2), 32), setBuf: t.arg(4),
				stat: t.arg(5), file: t.arg(6), userdata: t.arg8(m)})
		}
	}
	fixed := func(save bool) hleFunc {
		// (version, userId, setList, setBuf, funcFixed, funcStat, funcFile, container, userdata)
		return func(m *Machine, t *Thread) {
			prefix := ""
			if p := m.Mem.Read32(t.arg(2) + 8); p != 0 {
				prefix = m.Mem.String(p, 256)
			}
			if i := strings.IndexByte(prefix, '|'); i >= 0 {
				prefix = prefix[:i]
			}
			m.savedataRun(t, sdOp{save: save, prefix: prefix, setBuf: t.arg(3), fixed: t.arg(4),
				stat: t.arg(5), file: t.arg(6), userdata: t.arg8(m)})
		}
	}
	reg("cellSaveDataUserAutoLoad", auto(false))
	reg("cellSaveDataUserAutoSave", auto(true))
	reg("cellSaveDataUserFixedLoad", fixed(false))
	reg("cellSaveDataUserFixedSave", fixed(true))
}
