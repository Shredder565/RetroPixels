package wiiu

// The title's files, as the program sees them.
//
// A Wii U title has three folders: code (the .rpx, its .rpl libraries and
// the title's XML files), content (the game's data) and meta (its icon and
// names). Programs read content through the FS / FSA functions as
// /vol/content/..., and homebrew reads the SD card as /vol/external01/....

import (
	"errors"
	"io/fs"
	"os"
	"path"
	"path/filepath"
	"sort"
	"strings"
)

// FileSystem is the title's folders and the SD card.
type FileSystem struct {
	TitleID uint64
	// mounted folders by their Wii U path ("/vol/content"); each is a file
	// system rooted at that folder
	mounts map[string]fs.FS
	// files the program wrote (to save data or the SD card)
	written map[string][]byte
	dirs    map[string]bool
	// folders on the PC that writes go to, by Wii U path
	hostDirs map[string]string
}

// NewFileSystem is an empty file system (a program on its own).
func NewFileSystem() *FileSystem {
	return &FileSystem{TitleID: 0x0005000013374842, mounts: map[string]fs.FS{},
		written: map[string][]byte{}, dirs: map[string]bool{}, hostDirs: map[string]string{}}
}

// Mount makes f visible at the Wii U path at ("/vol/content").
func (f *FileSystem) Mount(at string, files fs.FS) {
	f.mounts[cleanPath(at)] = files
}

// MountDir makes a folder on the PC visible at a Wii U path; with writable
// set, files the program writes there are saved in it.
func (f *FileSystem) MountDir(at, dir string, writable bool) {
	f.Mount(at, os.DirFS(dir))
	if writable {
		f.hostDirs[cleanPath(at)] = dir
	}
}

// Mounted reports whether something is mounted at a Wii U path.
func (f *FileSystem) Mounted(at string) bool {
	_, ok := f.mounts[cleanPath(at)]
	return ok
}

// hostPath is where a written file goes on the PC ("" = keep it in memory).
func (f *FileSystem) hostPath(p string) string {
	p = cleanPath(p)
	for at, dir := range f.hostDirs {
		if p == at || strings.HasPrefix(p, at+"/") {
			rel := strings.TrimPrefix(strings.TrimPrefix(p, at), "/")
			if strings.Contains(rel, "..") {
				return ""
			}
			return filepath.Join(dir, filepath.FromSlash(rel))
		}
	}
	return ""
}

func cleanPath(p string) string {
	p = strings.ReplaceAll(p, "\\", "/")
	if !strings.HasPrefix(p, "/") {
		p = "/" + p
	}
	return path.Clean(p)
}

// resolve finds the mount holding p and the path inside it.
func (f *FileSystem) resolve(p string) (fs.FS, string, bool) {
	p = cleanPath(p)
	best := ""
	for at := range f.mounts {
		if (p == at || strings.HasPrefix(p, at+"/")) && len(at) > len(best) {
			best = at
		}
	}
	if best == "" {
		return nil, "", false
	}
	rel := strings.TrimPrefix(strings.TrimPrefix(p, best), "/")
	if rel == "" {
		rel = "."
	}
	return f.mounts[best], rel, true
}

// ReadFile reads a file at a Wii U path.
func (f *FileSystem) ReadFile(p string) ([]byte, error) {
	p = cleanPath(p)
	if b, ok := f.written[p]; ok {
		return b, nil
	}
	m, rel, ok := f.resolve(p)
	if !ok {
		return nil, fs.ErrNotExist
	}
	return readFS(m, rel)
}

// readFS reads a file, matching its name without regard to case (Wii U
// file systems are case-insensitive, the folders it was copied to may not be).
func readFS(m fs.FS, rel string) ([]byte, error) {
	b, err := fs.ReadFile(m, rel)
	if err == nil {
		return b, nil
	}
	if real := findFold(m, rel); real != "" {
		return fs.ReadFile(m, real)
	}
	return nil, err
}

func findFold(m fs.FS, rel string) string {
	parts := strings.Split(rel, "/")
	cur := "."
	for _, part := range parts {
		ents, err := fs.ReadDir(m, cur)
		if err != nil {
			return ""
		}
		found := ""
		for _, e := range ents {
			if strings.EqualFold(e.Name(), part) {
				found = e.Name()
				break
			}
		}
		if found == "" {
			return ""
		}
		if cur == "." {
			cur = found
		} else {
			cur = cur + "/" + found
		}
	}
	return cur
}

// Stat describes a file or folder at a Wii U path.
func (f *FileSystem) Stat(p string) (size int64, dir bool, err error) {
	p = cleanPath(p)
	if b, ok := f.written[p]; ok {
		return int64(len(b)), false, nil
	}
	if f.dirs[p] {
		return 0, true, nil
	}
	for at := range f.mounts {
		if at == p || strings.HasPrefix(at, p+"/") {
			return 0, true, nil
		}
	}
	m, rel, ok := f.resolve(p)
	if !ok {
		return 0, false, fs.ErrNotExist
	}
	st, err := fs.Stat(m, rel)
	if err != nil {
		if real := findFold(m, rel); real != "" {
			st, err = fs.Stat(m, real)
		}
		if err != nil {
			return 0, false, err
		}
	}
	return st.Size(), st.IsDir(), nil
}

// DirEntry is one entry of a folder listing.
type DirEntry struct {
	Name string
	Dir  bool
	Size int64
}

// ReadDir lists a folder at a Wii U path.
func (f *FileSystem) ReadDir(p string) ([]DirEntry, error) {
	p = cleanPath(p)
	seen := map[string]bool{}
	var out []DirEntry
	if m, rel, ok := f.resolve(p); ok {
		ents, err := fs.ReadDir(m, rel)
		if err != nil {
			if real := findFold(m, rel); real != "" {
				ents, err = fs.ReadDir(m, real)
			}
		}
		if err == nil {
			for _, e := range ents {
				var size int64
				if info, err := e.Info(); err == nil {
					size = info.Size()
				}
				out = append(out, DirEntry{e.Name(), e.IsDir(), size})
				seen[e.Name()] = true
			}
		}
	}
	// mount points and written files inside p
	add := func(full string, dir bool, size int64) {
		if !strings.HasPrefix(full, p+"/") {
			return
		}
		rest := strings.TrimPrefix(full, p+"/")
		name := rest
		if i := strings.IndexByte(rest, '/'); i >= 0 {
			name, dir = rest[:i], true
		}
		if !seen[name] {
			seen[name] = true
			out = append(out, DirEntry{name, dir, size})
		}
	}
	for at := range f.mounts {
		add(at, true, 0)
	}
	for w, b := range f.written {
		add(w, false, int64(len(b)))
	}
	for d := range f.dirs {
		add(d, true, 0)
	}
	if out == nil {
		if _, dir, err := f.Stat(p); err != nil || !dir {
			return nil, fs.ErrNotExist
		}
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Name < out[j].Name })
	return out, nil
}

// WriteFile stores a file the program wrote.
func (f *FileSystem) WriteFile(p string, b []byte) error {
	if hp := f.hostPath(p); hp != "" {
		os.MkdirAll(filepath.Dir(hp), 0o755)
		return os.WriteFile(hp, b, 0o644)
	}
	f.written[cleanPath(p)] = append([]byte(nil), b...)
	return nil
}

// Remove deletes a file the program wrote.
func (f *FileSystem) Remove(p string) error {
	p = cleanPath(p)
	if _, ok := f.written[p]; ok {
		delete(f.written, p)
		return nil
	}
	if f.dirs[p] {
		delete(f.dirs, p)
		return nil
	}
	if hp := f.hostPath(p); hp != "" {
		return os.Remove(hp)
	}
	return errors.New("read-only")
}

// Rename moves a file the program wrote.
func (f *FileSystem) Rename(from, to string) error {
	from, to = cleanPath(from), cleanPath(to)
	if b, ok := f.written[from]; ok {
		delete(f.written, from)
		f.written[to] = b
		return nil
	}
	hf, ht := f.hostPath(from), f.hostPath(to)
	if hf != "" && ht != "" {
		return os.Rename(hf, ht)
	}
	return errors.New("read-only")
}

// MakeDir makes a folder.
func (f *FileSystem) MakeDir(p string) error {
	if hp := f.hostPath(p); hp != "" {
		return os.MkdirAll(hp, 0o755)
	}
	f.dirs[cleanPath(p)] = true
	return nil
}

func (f *FileSystem) exists(p string) bool {
	_, _, err := f.Stat(p)
	return err == nil
}

// ReadCode reads a file of the title's code folder.
func (f *FileSystem) ReadCode(name string) ([]byte, error) {
	return f.ReadFile("/vol/code/" + name)
}

// Saves: the files and folders the program made under /vol/save, as one
// blob for the app to keep (the core's SRAM), and back.

const saveMagic = "RPWiiUSave1\n"

// SaveData is the program's save files ("" if it has none).
func (f *FileSystem) SaveData() []byte {
	var paths []string
	for p := range f.written {
		if strings.HasPrefix(p, "/vol/save/") {
			paths = append(paths, p)
		}
	}
	for p := range f.dirs {
		if strings.HasPrefix(p, "/vol/save/") {
			paths = append(paths, p)
		}
	}
	if len(paths) == 0 {
		return nil
	}
	sort.Strings(paths)
	out := []byte(saveMagic)
	put32 := func(v int) { out = append(out, byte(v>>24), byte(v>>16), byte(v>>8), byte(v)) }
	for _, p := range paths {
		b, file := f.written[p]
		if file {
			out = append(out, 'F')
		} else {
			out = append(out, 'D')
		}
		put32(len(p))
		out = append(out, p...)
		put32(len(b))
		out = append(out, b...)
	}
	return out
}

// LoadSaveData restores files SaveData made.
func (f *FileSystem) LoadSaveData(b []byte) error {
	if !strings.HasPrefix(string(b), saveMagic) {
		return errors.New("that isn't Wii U save data")
	}
	b = b[len(saveMagic):]
	get := func(n int) []byte {
		if n < 0 || n > len(b) {
			return nil
		}
		v := b[:n]
		b = b[n:]
		return v
	}
	get32 := func() int {
		v := get(4)
		if v == nil {
			return -1
		}
		return int(v[0])<<24 | int(v[1])<<16 | int(v[2])<<8 | int(v[3])
	}
	for len(b) > 0 {
		kind := get(1)
		p := get(get32())
		n := get32()
		data := get(n)
		if kind == nil || p == nil || data == nil && n != 0 {
			return errors.New("the Wii U save data is cut short")
		}
		path := cleanPath(string(p))
		if !strings.HasPrefix(path, "/vol/save/") {
			continue
		}
		if kind[0] == 'F' {
			f.written[path] = append([]byte(nil), data...)
		} else {
			f.dirs[path] = true
		}
	}
	return nil
}

// TitleFolders: when the program at rpxPath sits in a title's code folder
// (<title>/code/<name>.rpx), a file system with the title's code, content
// and meta folders mounted (read-only) and its title id from code/app.xml;
// otherwise nil.
func TitleFolders(rpxPath string) *FileSystem {
	if rpxPath == "" {
		return nil
	}
	code := filepath.Dir(rpxPath)
	if !strings.EqualFold(filepath.Base(code), "code") {
		return nil
	}
	root := filepath.Dir(code)
	f := NewFileSystem()
	f.MountDir("/vol/code", code, false)
	ents, _ := os.ReadDir(root)
	for _, e := range ents {
		name := strings.ToLower(e.Name())
		if e.IsDir() && (name == "content" || name == "meta") {
			f.MountDir("/vol/"+name, filepath.Join(root, e.Name()), false)
		}
	}
	if b, err := f.ReadCode("app.xml"); err == nil {
		if id, ok := xmlHex(b, "title_id"); ok {
			f.TitleID = id
		}
	}
	return f
}

// xmlHex reads a hex number element (<title_id ...>0005000010101D00</title_id>).
func xmlHex(b []byte, tag string) (uint64, bool) {
	s := string(b)
	i := strings.Index(s, "<"+tag)
	if i < 0 {
		return 0, false
	}
	j := strings.IndexByte(s[i:], '>')
	if j < 0 {
		return 0, false
	}
	s = s[i+j+1:]
	k := strings.IndexByte(s, '<')
	if k < 0 {
		return 0, false
	}
	var v uint64
	digits := strings.TrimSpace(s[:k])
	if digits == "" || len(digits) > 16 {
		return 0, false
	}
	for _, c := range digits {
		switch {
		case c >= '0' && c <= '9':
			v = v<<4 | uint64(c-'0')
		case c >= 'a' && c <= 'f':
			v = v<<4 | uint64(c-'a'+10)
		case c >= 'A' && c <= 'F':
			v = v<<4 | uint64(c-'A'+10)
		default:
			return 0, false
		}
	}
	return v, true
}
