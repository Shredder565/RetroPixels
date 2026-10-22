package dolphin

// The owner's Wii keys. PixelU's Wii engine doesn't ship Nintendo's keys:
// it reads them from a dump of the player's own console in its Wii NAND
// root (keys.bin, as BootMii's NAND backup writes it, or a Wii otp.bin).
// This finds such a dump on the PC, checks its layout and puts it in place
// as keys.bin. Nothing here downloads anything.
//
// keys.bin (1 KiB): 0x000 text written by the dumper, 0x100 the Wii's
// 128-byte OTP (common key at 0x114, console ID at 0x124), 0x200 its
// SEEPROM (MS ID, CA ID, NG key ID, NG signature, ..., Korean key at 0x274).
// A Wii otp.bin is just the 128-byte OTP. A Wii U otp.bin (1 KiB, eight
// banks of 0x80) holds the vWii's OTP in bank 0 and its MS ID, CA ID, NG
// key ID and signature at 0x300, Korean key at 0x348. A BootMii nand.bin
// may carry keys.bin as its last 1 KiB.

import (
	"bytes"
	"context"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"os"
	"path/filepath"
	"strings"
	"time"
)

// Where the engine looks (Source/Core/Core/IOS/IOSC.cpp reads the same files).
const (
	WiiKeysName = "keys.bin"
	wiiOTPName  = "otp.bin"
)

const (
	keysBinSize    = 0x400
	keysOTPOffset  = 0x100
	keysSEEPROMOff = 0x200
	wiiOTPSize     = 0x80
	wiiUOTPSize    = 0x400
	wiiUCertOffset = 0x300 // bank 6: MS ID, CA ID, NG key ID, NG signature
	wiiUCertSize   = 0x48
	wiiUKoreanOff  = 0x348
	keysKoreanOff  = 0x274
	otpCommonKey   = 0x14 // in the OTP
	otpConsoleID   = 0x24
	retailMSID     = 2
	retailCAID     = 1
	devCAID        = 2

	// BootMii's nand.bin: 0x40000 pages of 0x800 bytes plus 0x40 of ECC,
	// optionally followed by keys.bin.
	nandBackupSize = 0x40000 * (0x800 + 0x40)
)

// WiiKeysKind is what kind of dump a file is.
type WiiKeysKind int

const (
	KeysBin    WiiKeysKind = iota + 1 // BootMii keys.bin
	WiiOTP                            // a Wii's otp.bin (128 bytes)
	WiiUOTP                           // a Wii U's otp.bin (1 KiB; its vWii keys)
	NANDBackup                        // a BootMii nand.bin with keys.bin at its end
)

func (k WiiKeysKind) String() string {
	switch k {
	case KeysBin:
		return "keys.bin"
	case WiiOTP:
		return "Wii otp.bin"
	case WiiUOTP:
		return "Wii U otp.bin (vWii)"
	case NANDBackup:
		return "NAND backup with keys"
	}
	return "unknown"
}

// WiiKeysFile is a checked dump, already in the keys.bin layout.
type WiiKeysFile struct {
	Path      string
	Kind      WiiKeysKind
	ConsoleID uint32
	Dev       bool // a development console's keys (CA ID 2)
	keys      [keysBinSize]byte
}

// Describe: one line for a list of found files.
func (f *WiiKeysFile) Describe() string {
	c := "Wii"
	if f.Dev {
		c = "development Wii"
	}
	return fmt.Sprintf("%s  (%s, %s %08X)", f.Path, f.Kind, c, f.ConsoleID)
}

// WiiRoot is the engine's Wii NAND folder: User\Wii, unless another one is
// set in Dolphin.ini ([General] NANDRootPath).
func WiiRoot(user string) string {
	if p, ok := ReadINI(ConfigFile(user, "Dolphin.ini")).Get("General", "NANDRootPath"); ok && strings.TrimSpace(p) != "" {
		return filepath.Clean(strings.TrimSpace(p))
	}
	return filepath.Join(user, "Wii")
}

// HaveWiiKeys: whether the engine will find usable keys (the same checks it
// makes: keys.bin with a common key, else a 128-byte or 1 KiB otp.bin with one).
func HaveWiiKeys(user string) bool {
	root := WiiRoot(user)
	if b, err := readUpTo(filepath.Join(root, WiiKeysName), keysBinSize); err == nil && len(b) == keysBinSize &&
		usableKey(b[keysOTPOffset+otpCommonKey:][:16]) {
		return true
	}
	if b, err := readUpTo(filepath.Join(root, wiiOTPName), wiiUOTPSize+1); err == nil &&
		(len(b) == wiiOTPSize || len(b) == wiiUOTPSize) && usableKey(b[otpCommonKey:][:16]) {
		return true
	}
	return false
}

func readUpTo(path string, n int) ([]byte, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	b := make([]byte, n)
	k, err := io.ReadFull(f, b)
	if err != nil && !errors.Is(err, io.ErrUnexpectedEOF) && !errors.Is(err, io.EOF) {
		return nil, err
	}
	return b[:k], nil
}

// usableKey: not blank (all 0x00 or all 0xFF, as an erased or failed dump reads).
func usableKey(k []byte) bool {
	return !bytes.Equal(k, make([]byte, len(k))) && !bytes.Equal(k, bytes.Repeat([]byte{0xff}, len(k)))
}

// ErrNotWiiKeys: the file isn't a keys dump this understands.
var ErrNotWiiKeys = errors.New("this isn't a keys file from a Wii (keys.bin, otp.bin) or a Wii U (otp.bin)")

// sizeCouldBeKeys: the sizes ReadWiiKeysFile accepts.
func sizeCouldBeKeys(n int64) bool {
	return n == wiiOTPSize || n == keysBinSize || n == nandBackupSize+keysBinSize
}

// ReadWiiKeysFile checks a file's size and layout and converts it to the
// keys.bin layout.
func ReadWiiKeysFile(path string) (*WiiKeysFile, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	st, err := f.Stat()
	if err != nil {
		return nil, err
	}
	if !st.Mode().IsRegular() {
		return nil, ErrNotWiiKeys
	}
	return readWiiKeys(path, f, st.Size())
}

func readWiiKeys(path string, r io.ReaderAt, size int64) (*WiiKeysFile, error) {
	if !sizeCouldBeKeys(size) {
		return nil, ErrNotWiiKeys
	}
	out := &WiiKeysFile{Path: path}
	switch size {
	case wiiOTPSize:
		b := make([]byte, wiiOTPSize)
		if _, err := r.ReadAt(b, 0); err != nil {
			return nil, err
		}
		if !fromWiiOTP(out, b) {
			return nil, ErrNotWiiKeys
		}
	case keysBinSize:
		b := make([]byte, keysBinSize)
		if _, err := r.ReadAt(b, 0); err != nil {
			return nil, err
		}
		if !fromKeysBin(out, b, KeysBin) && !fromWiiUOTP(out, b) {
			return nil, ErrNotWiiKeys
		}
	default: // nand.bin with keys.bin at its end
		b := make([]byte, keysBinSize)
		if n, err := r.ReadAt(b, nandBackupSize); n != len(b) {
			return nil, err
		}
		if !fromKeysBin(out, b, NANDBackup) {
			return nil, ErrNotWiiKeys
		}
	}
	out.ConsoleID = binary.BigEndian.Uint32(out.keys[keysOTPOffset+otpConsoleID:])
	out.Dev = binary.BigEndian.Uint32(out.keys[keysSEEPROMOff+4:]) == devCAID
	return out, nil
}

// textHeader: keys.bin starts with the dumper's text ("BackupMii v1,
// ConsoleID: ...") and zeros; a Wii U OTP starts with boot1's hash.
func textHeader(b []byte) bool {
	for _, c := range b {
		if c != 0 && c != '\n' && c != '\r' && c != '\t' && (c < 0x20 || c >= 0x7f) {
			return false
		}
	}
	return true
}

func fromKeysBin(out *WiiKeysFile, b []byte, kind WiiKeysKind) bool {
	if !textHeader(b[:keysOTPOffset]) || !usableKey(b[keysOTPOffset+otpCommonKey:][:16]) {
		return false
	}
	copy(out.keys[:], b)
	out.Kind = kind
	return true
}

func fromWiiOTP(out *WiiKeysFile, b []byte) bool {
	if !usableKey(b[otpCommonKey:][:16]) {
		return false
	}
	out.Kind = WiiOTP
	setHeader(out, "Wii otp.bin")
	copy(out.keys[keysOTPOffset:], b[:wiiOTPSize])
	// No SEEPROM: a retail console's MS and CA IDs, as the engine assumes for otp.bin.
	binary.BigEndian.PutUint32(out.keys[keysSEEPROMOff:], retailMSID)
	binary.BigEndian.PutUint32(out.keys[keysSEEPROMOff+4:], retailCAID)
	return true
}

func fromWiiUOTP(out *WiiKeysFile, b []byte) bool {
	if !usableKey(b[otpCommonKey:][:16]) {
		return false
	}
	out.Kind = WiiUOTP
	setHeader(out, "Wii U otp.bin")
	copy(out.keys[keysOTPOffset:], b[:wiiOTPSize])
	copy(out.keys[keysSEEPROMOff:], b[wiiUCertOffset:][:wiiUCertSize])
	copy(out.keys[keysKoreanOff:], b[wiiUKoreanOff:][:16])
	return true
}

// setHeader: the text at the start of keys.bin (the engine doesn't read it).
func setHeader(out *WiiKeysFile, from string) {
	copy(out.keys[:keysOTPOffset], fmt.Sprintf("RetroPixels: converted from %s\n", from))
}

// InstallWiiKeys writes f to the engine's Wii folder as keys.bin. A
// keys.bin already there (unusable, or the player chose another) is kept
// as keys.bin.old.
func InstallWiiKeys(user string, f *WiiKeysFile) (string, error) {
	root := WiiRoot(user)
	if err := os.MkdirAll(root, 0o755); err != nil {
		return "", err
	}
	dst := filepath.Join(root, WiiKeysName)
	tmp := dst + ".tmp"
	if err := os.WriteFile(tmp, f.keys[:], 0o644); err != nil {
		os.Remove(tmp)
		return "", err
	}
	if _, err := os.Stat(dst); err == nil {
		old := dst + ".old"
		os.Remove(old)
		if err := os.Rename(dst, old); err != nil {
			os.Remove(tmp)
			return "", err
		}
	}
	if err := os.Rename(tmp, dst); err != nil {
		os.Remove(tmp)
		return "", err
	}
	return dst, nil
}

// ---------------- looking for a dump on the PC ----------------

// WiiKeysScan limits a search.
type WiiKeysScan struct {
	Roots    []string      // where to look (WiiKeysScanRoots by default)
	MaxDepth int           // folders below a root (12 by default)
	Timeout  time.Duration // 90 s by default
	Progress func(dir string, found int)
}

// Folders never worth opening: Windows itself, installed programs,
// caches and source trees.
var keysSkipDirs = map[string]bool{
	"windows": true, "program files": true, "program files (x86)": true, "programdata": true,
	"$recycle.bin": true, "system volume information": true, "recovery": true, "$windows.~bt": true,
	"$windows.~ws": true, "$winreagent": true, "msocache": true, "perflogs": true, "config.msi": true,
	"windowsapps": true, "winsxs": true, "temp": true, "tmp": true, "cache": true, "caches": true,
	"node_modules": true, ".git": true, ".svn": true, ".hg": true, "__pycache__": true,
	"steamapps": true, "shadercache": true, "packages": true, "microsoft": true, "google": true,
	"mozilla": true, "nvidia": true, "nvidia corporation": true, "amd": true, "intel": true,
	"proc": true, "sys": true, "dev": true, "usr": true, "lib": true, "bin": true, "snap": true,
}

// keysName: file names a dump goes by (keys.bin, otp.bin, "keys (1).bin",
// "wiiu_otp.bin", nand.bin...).
func keysName(name string) bool {
	n := strings.ToLower(name)
	if !strings.HasSuffix(n, ".bin") {
		return false
	}
	return strings.Contains(n, "keys") || strings.Contains(n, "otp") || strings.Contains(n, "nand")
}

// ScanWiiKeys looks for dumps under the roots until done, the time limit,
// or ctx is cancelled. Copies of the same dump are listed once.
func ScanWiiKeys(ctx context.Context, s WiiKeysScan) []*WiiKeysFile {
	if s.Roots == nil {
		s.Roots = WiiKeysScanRoots()
	}
	if s.MaxDepth <= 0 {
		s.MaxDepth = 12
	}
	if s.Timeout <= 0 {
		s.Timeout = 90 * time.Second
	}
	ctx, cancel := context.WithTimeout(ctx, s.Timeout)
	defer cancel()
	var found []*WiiKeysFile
	seen := map[[keysBinSize]byte]bool{}
	lastReport := time.Time{}
	done := map[string]bool{} // roots already searched, not searched again inside a later one
	for _, root := range s.Roots {
		root = filepath.Clean(root)
		if done[strings.ToLower(root)] {
			continue
		}
		done[strings.ToLower(root)] = true
		base := strings.Count(strings.TrimSuffix(root, string(filepath.Separator)), string(filepath.Separator))
		_ = filepath.WalkDir(root, func(p string, d fs.DirEntry, err error) error {
			if ctx.Err() != nil {
				return fs.SkipAll
			}
			if err != nil {
				if d != nil && d.IsDir() {
					return fs.SkipDir
				}
				return nil
			}
			if d.IsDir() {
				if p == root {
					return nil
				}
				if d.Type()&fs.ModeSymlink != 0 || keysSkipDirs[strings.ToLower(d.Name())] || done[strings.ToLower(p)] ||
					strings.Count(p, string(filepath.Separator))-base > s.MaxDepth {
					return fs.SkipDir
				}
				if s.Progress != nil && time.Since(lastReport) > 200*time.Millisecond {
					lastReport = time.Now()
					s.Progress(p, len(found))
				}
				return nil
			}
			if !d.Type().IsRegular() || !keysName(d.Name()) {
				return nil
			}
			if info, err := d.Info(); err != nil || !sizeCouldBeKeys(info.Size()) {
				return nil
			}
			if f, err := ReadWiiKeysFile(p); err == nil && !seen[f.keys] {
				seen[f.keys] = true
				found = append(found, f)
				if s.Progress != nil {
					s.Progress(filepath.Dir(p), len(found))
				}
			}
			return nil
		})
	}
	return found
}
