package wiiu

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// TestHLEStructs runs hletest.c, which hands library calls structs with
// guard words after them and prints what the calls wrote.
func TestHLEStructs(t *testing.T) {
	rpx := buildModule(t, buildOpts{imports: []string{
		"coreinit FSInit FSAddClient FSInitCmdBlock FSOpenDir FSReadDir FSReadDirAsync FSRewindDir FSCloseDir FSGetStat FSGetCwd",
		"coreinit FSAAddClient FSAGetCwd",
		"sndcore2 AXInit AXAcquireVoiceEx",
		"gx2 GX2RGetBufferAlignment GX2RGetBufferAllocationSize"}}, "hletest.c")
	root := t.TempDir()
	long := strings.Repeat("n", 200) + ".bin"
	for name, data := range map[string]string{
		"code/app.xml":         `<?xml version="1.0"?><app><title_id type="hexBinary" length="8">00050000101C9500</title_id></app>`,
		"content/long/" + long: "x",
		"meta/meta.xml":        "<menu/>",
	} {
		p := filepath.Join(root, filepath.FromSlash(name))
		os.MkdirAll(filepath.Dir(p), 0o755)
		os.WriteFile(p, []byte(data), 0o644)
	}
	rpxPath := filepath.Join(root, "code", "hletest.rpx")
	os.WriteFile(rpxPath, rpx, 0o644)
	files := TitleFolders(rpxPath)
	if files == nil {
		t.Fatal("the title's folders weren't found")
	}
	m, err := New(rpx, "hletest.rpx", files)
	if err != nil {
		t.Fatal(err)
	}
	var log strings.Builder
	m.Log = func(s string) { log.WriteString(s + "\n") }
	for i := 0; i < 30 && !m.Exited && m.Err == nil; i++ {
		m.RunFrame()
	}
	if m.Err != nil {
		t.Fatalf("%v\nlog:\n%s", m.Err, log.String())
	}
	for _, want := range []string{
		// FSDirectoryEntry is 0xE4 bytes: the name is cut to 127 characters
		"dir: open 0 read 0 flag 1000000 len 127 guard 1",
		// the message points at the FSAsyncResult inside the command block
		"async: 0 type 8 in block 1 status 0 block 1 ctx 1234",
		"stat: 0 flag 80000000 size 0 perm 777",
		"cwd: 0 1 guard 1",
		"vpad: 1 err 0 valid 3 battery c0 accx 1 guard 1",
		"vpad1: 0 err -2 guard 1",
		"ax: cb 0 ex 5678 user 9abc",
		"gx2r: 40 100 40",
		"hle test done",
	} {
		if !strings.Contains(log.String(), want) {
			t.Errorf("want %q; log:\n%s", want, log.String())
		}
	}
}
