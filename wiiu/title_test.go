package wiiu

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"testing/fstest"
)

// TestTitleLibraries runs titletest.c with its libraries in the code folder.
func TestTitleLibraries(t *testing.T) {
	helper := buildModule(t, buildOpts{rpl: true, exports: []string{"helper_mul", "data:helper_ready"}}, "libhelper.c")
	game := buildModule(t, buildOpts{rpl: true, exports: []string{"lib_add", "data:lib_counter"},
		imports: []string{"helperlib helper_mul"}}, "libgame.c")
	dyn := buildModule(t, buildOpts{rpl: true, exports: []string{"dyn_value"}}, "libdyn.c")
	rpx := buildModule(t, buildOpts{imports: []string{"gamelib lib_add data:lib_counter",
		"coreinit OSDynLoad_Acquire OSDynLoad_FindExport"}}, "titletest.c")

	files := NewFileSystem()
	files.Mount("/vol/code", fstest.MapFS{
		"helperlib.rpl": {Data: helper},
		"gamelib.rpl":   {Data: game},
		"dynlib.rpl":    {Data: dyn},
	})
	m, err := New(rpx, "titletest.rpx", files)
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
	for _, want := range []string{"lib_add 106", "lib_counter 100", "dyn 0 47", "title test done"} {
		if !strings.Contains(log.String(), want) {
			t.Fatalf("want %q; log:\n%s", want, log.String())
		}
	}
	if len(m.Modules) != 4 {
		t.Errorf("%d modules loaded, want 4", len(m.Modules))
	}
}

// TestTitleFolder runs savetest.c from a title's folders twice, keeping its
// save between the runs.
func TestTitleFolder(t *testing.T) {
	rpx := buildModule(t, buildOpts{imports: []string{
		"coreinit FSInit FSAddClient FSInitCmdBlock FSOpenFile FSReadFile FSWriteFile FSCloseFile OSGetTitleID",
		"nn_save SAVEInit SAVEInitSaveDir SAVEOpenFile",
		"nn_act GetSlotNo__Q2_2nn3actFv"}}, "savetest.c")
	root := t.TempDir()
	for name, data := range map[string]string{
		"code/app.xml":      `<?xml version="1.0"?><app><title_id type="hexBinary" length="8">00050000101C9500</title_id></app>`,
		"content/hello.txt": "hi there",
		"meta/meta.xml":     "<menu/>",
	} {
		p := filepath.Join(root, filepath.FromSlash(name))
		os.MkdirAll(filepath.Dir(p), 0o755)
		os.WriteFile(p, []byte(data), 0o644)
	}
	rpxPath := filepath.Join(root, "code", "savetest.rpx")
	os.WriteFile(rpxPath, rpx, 0o644)

	var save []byte
	for run := 1; run <= 2; run++ {
		files := TitleFolders(rpxPath)
		if files == nil {
			t.Fatal("the title's folders weren't found")
		}
		if save != nil {
			if err := files.LoadSaveData(save); err != nil {
				t.Fatal(err)
			}
		}
		m, err := New(rpx, "savetest.rpx", files)
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
		for _, want := range []string{"title 00050000101c9500", "content hi there", fmt.Sprintf("runs %d", run), "save test done"} {
			if !strings.Contains(log.String(), want) {
				t.Fatalf("run %d: want %q; log:\n%s", run, want, log.String())
			}
		}
		save = files.SaveData()
		if save == nil {
			t.Fatal("no save data")
		}
	}
}
