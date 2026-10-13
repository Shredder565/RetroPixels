// Package sevenzip unpacks .7z archives, such as the official Dolphin
// emulator builds for Windows, using only the Go standard library.
//
// Supported: plain and compressed headers, solid archives, multi-coder
// folders, and the Copy, LZMA, LZMA2, BCJ (x86), BCJ2, ARM64, Delta,
// Deflate and BZip2 methods. Encrypted archives are refused with a clear
// error.
package sevenzip

import (
	"errors"
	"fmt"
	"hash/crc32"
	"io"
	"os"
	"path/filepath"
	"sort"
	"strings"
)

// List returns the entries of the archive in stored order.
func List(archivePath string) ([]File, error) {
	a, err := openArchive(archivePath)
	if err != nil {
		return nil, err
	}
	defer a.f.Close()
	out := make([]File, 0, len(a.files))
	for _, f := range a.files {
		if f.anti {
			continue
		}
		f.Name = strings.ReplaceAll(f.Name, `\`, "/")
		out = append(out, f)
	}
	return out, nil
}

// ExtractAll unpacks every file and folder of archivePath into destDir.
// progress, if not nil, is called with the bytes written so far and the
// total. Each file is written under a temporary name and renamed into place
// only once it is complete and its checksum matches.
func ExtractAll(archivePath, destDir string, progress func(done, total int64)) error {
	a, err := openArchive(archivePath)
	if err != nil {
		return err
	}
	defer a.f.Close()

	if err := os.MkdirAll(destDir, 0o755); err != nil {
		return err
	}
	// Check every path before writing anything.
	paths := make([]string, len(a.files))
	var total int64
	for i, f := range a.files {
		if f.anti {
			continue
		}
		p, err := safePath(destDir, f.Name)
		if err != nil {
			return err
		}
		if p == "" && !f.IsDir {
			return fmt.Errorf("this .7z contains a file with no usable name: %q", f.Name)
		}
		paths[i] = p
		if f.hasStream {
			total += int64(f.Size)
		}
	}

	var done int64
	report := func() {
		if progress != nil {
			progress(done, total)
		}
	}
	report()

	var dirs []int
	var r io.Reader
	cur := -1
	buf := make([]byte, 256<<10)
	for i := range a.files {
		f := &a.files[i]
		switch {
		case f.anti:
			// Anti-items mark deletions in update archives; nothing to do.
			continue
		case f.IsDir:
			if paths[i] == "" {
				continue // the archive root itself
			}
			if err := os.MkdirAll(paths[i], 0o755); err != nil {
				return err
			}
			dirs = append(dirs, i)
			continue
		}
		if f.hasStream && f.folder != cur {
			cur = f.folder
			if r, err = a.folderReader(a.streams, cur); err != nil {
				return err
			}
		}
		var src io.Reader = eofReader{}
		if f.hasStream {
			src = r
		}
		err := writeFile(paths[i], f, src, buf, func(n int) {
			done += int64(n)
			report()
		})
		if err != nil {
			return err
		}
	}

	// Set folder times last, deepest first, since adding files changes them.
	sort.Slice(dirs, func(x, y int) bool { return len(paths[dirs[x]]) > len(paths[dirs[y]]) })
	for _, i := range dirs {
		if t := a.files[i].ModTime; !t.IsZero() {
			os.Chtimes(paths[i], t, t)
		}
	}
	return nil
}

type eofReader struct{}

func (eofReader) Read([]byte) (int, error) { return 0, io.EOF }

// writeFile copies f.Size bytes from src to a temporary file next to path,
// checks the CRC, then renames it into place.
func writeFile(path string, f *File, src io.Reader, buf []byte, step func(int)) (err error) {
	dir := filepath.Dir(path)
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return err
	}
	tmp, err := os.CreateTemp(dir, ".rp7z-*.tmp")
	if err != nil {
		return err
	}
	defer func() {
		if err != nil {
			tmp.Close()
			os.Remove(tmp.Name())
		}
	}()

	crc := uint32(0)
	left := int64(f.Size)
	for left > 0 {
		chunk := buf
		if int64(len(chunk)) > left {
			chunk = chunk[:left]
		}
		n, rerr := io.ReadFull(src, chunk)
		if n > 0 {
			crc = crc32.Update(crc, crc32.IEEETable, chunk[:n])
			if _, err := tmp.Write(chunk[:n]); err != nil {
				return err
			}
			left -= int64(n)
			step(n)
		}
		if rerr != nil {
			if rerr == io.EOF || rerr == io.ErrUnexpectedEOF {
				rerr = errors.New("compressed data ends early")
			}
			return fmt.Errorf("can't unpack %s: %w", f.Name, rerr)
		}
	}
	if f.hasCRC && crc != f.crc {
		return fmt.Errorf("can't unpack %s: the archive is damaged (checksum mismatch)", f.Name)
	}
	if err := tmp.Close(); err != nil {
		return err
	}
	if !f.ModTime.IsZero() {
		os.Chtimes(tmp.Name(), f.ModTime, f.ModTime)
	}
	return os.Rename(tmp.Name(), path)
}

// safePath joins an archive name onto dest, refusing names that would land
// outside it: absolute paths, drive letters, ".." and the like. Both / and \
// count as separators. It returns "" for a name that is the root itself.
func safePath(dest, name string) (string, error) {
	bad := func() (string, error) {
		return "", fmt.Errorf("this .7z contains an unsafe path, refusing to unpack it: %q", name)
	}
	if name == "" || strings.ContainsRune(name, 0) {
		return bad()
	}
	n := strings.ReplaceAll(name, `\`, "/")
	if strings.HasPrefix(n, "/") || strings.Contains(n, ":") {
		return bad()
	}
	var parts []string
	for _, c := range strings.Split(n, "/") {
		if c == "" || c == "." {
			continue
		}
		// Windows drops trailing dots and spaces, so ". ." or "..." could act
		// like "..".
		if strings.TrimRight(c, ". ") == "" {
			return bad()
		}
		parts = append(parts, c)
	}
	if len(parts) == 0 {
		return "", nil
	}
	// IsLocal also rejects reserved device names such as NUL on Windows.
	rel := filepath.Join(parts...)
	if !filepath.IsLocal(rel) {
		return bad()
	}
	return filepath.Join(dest, rel), nil
}
