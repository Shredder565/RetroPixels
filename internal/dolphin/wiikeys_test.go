package dolphin

import (
	"bytes"
	"context"
	"encoding/binary"
	"io"
	"os"
	"path/filepath"
	"testing"
	"time"
)

// Synthetic dumps only: made-up bytes in the right places, no real console's keys.

func pattern(n int, seed byte) []byte {
	b := make([]byte, n)
	for i := range b {
		b[i] = seed + byte(i*7)
	}
	return b
}

func synthKeysBin(dev bool) []byte {
	b := make([]byte, keysBinSize)
	copy(b, "BackupMii v1, ConsoleID: 0badf00d\n")
	copy(b[keysOTPOffset:], pattern(wiiOTPSize, 0x31))
	binary.BigEndian.PutUint32(b[keysOTPOffset+otpConsoleID:], 0x0badf00d)
	ca, ms := uint32(retailCAID), uint32(retailMSID)
	if dev {
		ca, ms = devCAID, 3
	}
	binary.BigEndian.PutUint32(b[keysSEEPROMOff:], ms)
	binary.BigEndian.PutUint32(b[keysSEEPROMOff+4:], ca)
	copy(b[keysSEEPROMOff+8:0x248], pattern(0x40, 0x52))
	copy(b[keysKoreanOff:], pattern(16, 0x77))
	return b
}

func synthWiiUOTP() []byte {
	b := pattern(wiiUOTPSize, 0x90) // bank 0 starts with boot1's hash: not text
	binary.BigEndian.PutUint32(b[otpConsoleID:], 0x12345678)
	binary.BigEndian.PutUint32(b[wiiUCertOffset:], retailMSID)
	binary.BigEndian.PutUint32(b[wiiUCertOffset+4:], retailCAID)
	return b
}

func write(t *testing.T, path string, b []byte) string {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, b, 0o644); err != nil {
		t.Fatal(err)
	}
	return path
}

func TestWiiRoot(t *testing.T) {
	user := t.TempDir()
	if got := WiiRoot(user); got != filepath.Join(user, "Wii") {
		t.Fatalf("WiiRoot = %q", got)
	}
	other := filepath.Join(t.TempDir(), "nand")
	write(t, ConfigFile(user, "Dolphin.ini"), []byte("[General]\nNANDRootPath = "+other+"\n"))
	if got := WiiRoot(user); got != other {
		t.Fatalf("WiiRoot with NANDRootPath = %q, want %q", got, other)
	}
}

func TestHaveWiiKeys(t *testing.T) {
	user := t.TempDir()
	root := WiiRoot(user)
	if HaveWiiKeys(user) {
		t.Fatal("keys reported with no file")
	}
	write(t, filepath.Join(root, "keys.bin"), make([]byte, keysBinSize)) // blank dump
	if HaveWiiKeys(user) {
		t.Fatal("a blank keys.bin counted")
	}
	write(t, filepath.Join(root, "otp.bin"), pattern(wiiOTPSize, 1))
	if !HaveWiiKeys(user) {
		t.Fatal("otp.bin not counted when keys.bin is blank (the engine falls back to it)")
	}
	os.Remove(filepath.Join(root, "otp.bin"))
	write(t, filepath.Join(root, "keys.bin"), synthKeysBin(false))
	if !HaveWiiKeys(user) {
		t.Fatal("keys.bin not counted")
	}
	write(t, filepath.Join(root, "keys.bin"), synthKeysBin(false)[:0x200]) // cut short
	if HaveWiiKeys(user) {
		t.Fatal("a short keys.bin counted")
	}
}

func TestReadWiiKeysFile(t *testing.T) {
	dir := t.TempDir()

	kb := synthKeysBin(false)
	f, err := ReadWiiKeysFile(write(t, filepath.Join(dir, "keys.bin"), kb))
	if err != nil || f.Kind != KeysBin || f.ConsoleID != 0x0badf00d || f.Dev || !bytes.Equal(f.keys[:], kb) {
		t.Fatalf("keys.bin: %+v %v", f, err)
	}
	f, err = ReadWiiKeysFile(write(t, filepath.Join(dir, "dev.bin"), synthKeysBin(true)))
	if err != nil || !f.Dev {
		t.Fatalf("development keys.bin: %+v %v", f, err)
	}

	// A Wii otp.bin becomes keys.bin with a retail console's MS and CA IDs.
	otp := pattern(wiiOTPSize, 0x44)
	f, err = ReadWiiKeysFile(write(t, filepath.Join(dir, "otp.bin"), otp))
	if err != nil || f.Kind != WiiOTP {
		t.Fatalf("otp.bin: %+v %v", f, err)
	}
	if !bytes.Equal(f.keys[keysOTPOffset:keysSEEPROMOff-0x80], otp) ||
		binary.BigEndian.Uint32(f.keys[keysSEEPROMOff:]) != retailMSID ||
		binary.BigEndian.Uint32(f.keys[keysSEEPROMOff+4:]) != retailCAID ||
		!bytes.Equal(f.keys[keysSEEPROMOff+8:], make([]byte, keysBinSize-keysSEEPROMOff-8)) {
		t.Fatal("otp.bin converted wrongly")
	}

	// A Wii U otp.bin: bank 0, bank 6's certificate fields and the Korean key.
	wu := synthWiiUOTP()
	f, err = ReadWiiKeysFile(write(t, filepath.Join(dir, "wiiu_otp.bin"), wu))
	if err != nil || f.Kind != WiiUOTP || f.ConsoleID != 0x12345678 {
		t.Fatalf("Wii U otp.bin: %+v %v", f, err)
	}
	if !bytes.Equal(f.keys[keysOTPOffset:keysOTPOffset+wiiOTPSize], wu[:wiiOTPSize]) ||
		!bytes.Equal(f.keys[keysSEEPROMOff:keysSEEPROMOff+wiiUCertSize], wu[wiiUCertOffset:wiiUCertOffset+wiiUCertSize]) ||
		!bytes.Equal(f.keys[keysKoreanOff:keysKoreanOff+16], wu[wiiUKoreanOff:wiiUKoreanOff+16]) {
		t.Fatal("Wii U otp.bin converted wrongly")
	}

	// Not keys: wrong sizes, blank dumps.
	for name, b := range map[string][]byte{
		"short.bin": pattern(100, 1), "blank.bin": make([]byte, wiiOTPSize),
		"erased.bin": bytes.Repeat([]byte{0xff}, keysBinSize), "big.bin": pattern(2048, 3),
	} {
		if f, err := ReadWiiKeysFile(write(t, filepath.Join(dir, "bad", name), b)); err == nil {
			t.Errorf("%s accepted as %s", name, f.Kind)
		}
	}
}

// tailReader: a nand.bin of the full size that is zeros except for its last bytes (no 528 MiB file).
type tailReader struct {
	tail []byte
	size int64
}

func (r tailReader) ReadAt(p []byte, off int64) (int, error) {
	n := 0
	for n < len(p) && off+int64(n) < r.size {
		if i := off + int64(n) - (r.size - int64(len(r.tail))); i >= 0 {
			p[n] = r.tail[i]
		} else {
			p[n] = 0
		}
		n++
	}
	if n < len(p) {
		return n, io.EOF
	}
	return n, nil
}

func TestReadWiiKeysFromNANDBackup(t *testing.T) {
	kb := synthKeysBin(false)
	f, err := readWiiKeys("nand.bin", tailReader{kb, nandBackupSize + keysBinSize}, nandBackupSize+keysBinSize)
	if err != nil || f.Kind != NANDBackup || !bytes.Equal(f.keys[:], kb) {
		t.Fatalf("nand.bin: %+v %v", f, err)
	}
	// A backup without keys at its end, or of the wrong size.
	if _, err := readWiiKeys("nand.bin", tailReader{nil, nandBackupSize}, nandBackupSize); err == nil {
		t.Fatal("nand.bin without keys accepted")
	}
	if _, err := readWiiKeys("nand.bin", tailReader{make([]byte, keysBinSize), nandBackupSize + keysBinSize}, nandBackupSize+keysBinSize); err == nil {
		t.Fatal("nand.bin with blank keys accepted")
	}
}

func TestInstallWiiKeys(t *testing.T) {
	user := t.TempDir()
	write(t, filepath.Join(WiiRoot(user), "keys.bin"), make([]byte, keysBinSize)) // a blank one already there
	f, err := ReadWiiKeysFile(write(t, filepath.Join(t.TempDir(), "otp.bin"), pattern(wiiOTPSize, 0x21)))
	if err != nil {
		t.Fatal(err)
	}
	dst, err := InstallWiiKeys(user, f)
	if err != nil {
		t.Fatal(err)
	}
	got, _ := os.ReadFile(dst)
	if !bytes.Equal(got, f.keys[:]) || !HaveWiiKeys(user) {
		t.Fatal("installed keys.bin isn't the converted dump")
	}
	if old, err := os.ReadFile(dst + ".old"); err != nil || len(old) != keysBinSize {
		t.Fatal("the previous keys.bin wasn't kept")
	}
	// What the engine reads back is the same dump.
	again, err := ReadWiiKeysFile(dst)
	if err != nil || again.keys != f.keys {
		t.Fatalf("installed keys.bin doesn't read back: %v", err)
	}
}

func TestScanWiiKeys(t *testing.T) {
	root := t.TempDir()
	write(t, filepath.Join(root, "SD backup", "keys.bin"), synthKeysBin(false))
	write(t, filepath.Join(root, "copy", "keys (1).bin"), synthKeysBin(false))             // same dump again
	write(t, filepath.Join(root, "wiiu", "otp.bin"), synthWiiUOTP())                       // another console
	write(t, filepath.Join(root, "Windows", "keys.bin"), synthKeysBin(true))               // skipped folder
	write(t, filepath.Join(root, "a", "b", "c", "d", "e", "keys.bin"), synthKeysBin(true)) // too deep
	write(t, filepath.Join(root, "music", "song.bin"), synthKeysBin(true))                 // not a keys name
	write(t, filepath.Join(root, "junk", "otp.bin"), pattern(333, 9))                      // wrong size

	var progress int
	found := ScanWiiKeys(context.Background(), WiiKeysScan{Roots: []string{root, filepath.Join(root, "wiiu")}, MaxDepth: 4,
		Progress: func(string, int) { progress++ }})
	if len(found) != 2 {
		for _, f := range found {
			t.Log(f.Describe())
		}
		t.Fatalf("found %d dumps, want 2", len(found))
	}
	kinds := map[WiiKeysKind]bool{}
	for _, f := range found {
		kinds[f.Kind] = true
	}
	if !kinds[KeysBin] || !kinds[WiiUOTP] {
		t.Fatalf("found kinds %v", kinds)
	}
	if progress == 0 {
		t.Fatal("no progress reported")
	}

	// Cancelled: stops at once.
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if got := ScanWiiKeys(ctx, WiiKeysScan{Roots: []string{root}}); len(got) != 0 {
		t.Fatalf("cancelled scan found %d", len(got))
	}
	// Time limit.
	start := time.Now()
	ScanWiiKeys(context.Background(), WiiKeysScan{Roots: []string{root}, Timeout: time.Nanosecond})
	if time.Since(start) > 5*time.Second {
		t.Fatal("time limit ignored")
	}
}
