package main

import (
	"bytes"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"os/exec"
	"path/filepath"
	"testing"

	"retropixels/internal/snesdbg"
)

// The page's side of the SNES Debugger: start a session with the ROM, see a
// request, send the capture (packed by web/emu/snes_debug.js) and end.
func TestSNESDebugHTTP(t *testing.T) {
	node, err := exec.LookPath("node")
	if err != nil {
		t.Skip("node not installed")
	}
	base := t.TempDir()
	t.Setenv("RETRO_SNESDBG_DIR", base)
	snesDbgLaunch = false
	token = "test-token"
	hostOK["test.local"] = true
	srv := httptest.NewServer(newMux())
	defer srv.Close()
	do := func(method, path string, body []byte) (int, map[string]any) {
		req, _ := http.NewRequest(method, srv.URL+path, bytes.NewReader(body))
		req.Host = "test.local"
		req.Header.Set("X-Retro-Token", "test-token")
		res, err := http.DefaultClient.Do(req)
		if err != nil {
			t.Fatal(err)
		}
		defer res.Body.Close()
		b, _ := io.ReadAll(res.Body)
		var m map[string]any
		json.Unmarshal(b, &m)
		return res.StatusCode, m
	}
	rom, _ := os.ReadFile(filepath.Join("tools", "snes-debugger", "testroms", "bgtest-direct.sfc"))
	code, st := do("POST", "/api/snesdbg/start?name=bgtest", rom)
	if code != 200 {
		t.Fatalf("start: %d %v", code, st)
	}
	id, dir := st["id"].(string), st["session"].(string)
	if err := snesdbg.Request(dir, "r1"); err != nil {
		t.Fatal(err)
	}
	code, st = do("GET", "/api/snesdbg/poll?id="+id, nil)
	if p, _ := st["pending"].([]any); code != 200 || len(p) != 1 || p[0] != "r1" {
		t.Fatalf("poll: %d %v", code, st)
	}
	// a capture from the web core, packed the way the page packs it
	out := filepath.Join(t.TempDir(), "cap.bin")
	script := `
const fs = require("fs"), path = require("path"), emu = process.argv[1];
for (const f of ["w65816.js", "spc700.js", "sdsp.js", "snes.js", "snes_debug.js"]) (0, eval)(fs.readFileSync(path.join(emu, f), "utf8"));
const s = new globalThis.RPCores.snes(new Uint8Array(fs.readFileSync(process.argv[2])), { sampleRate: 48000, trace: true });
for (let i = 0; i < 4; i++) { s.runFrame(); s.takeAudio(); }
const c = s.debugCapture(); const files = Object.assign({}, c.files);
files["capture.json"] = new TextEncoder().encode(JSON.stringify(c.meta));
fs.writeFileSync(process.argv[3], globalThis.RPSNESDebug.pack(files));`
	cmd := exec.Command(node, "-e", script, filepath.Join("web", "emu"), filepath.Join("tools", "snes-debugger", "testroms", "bgtest-direct.sfc"), out)
	if b, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("node: %v %s", err, b)
	}
	blob, _ := os.ReadFile(out)
	code, st = do("POST", "/api/snesdbg/capture?id="+id+"&req=r1", blob)
	if code != 200 {
		t.Fatalf("capture: %d %v", code, st)
	}
	if _, err := os.Stat(filepath.Join(dir, "captures", "r1", "vram_src.bin")); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(filepath.Join(dir, "requests", "r1.req")); err == nil {
		t.Error("the request wasn't removed")
	}
	do("POST", "/api/snesdbg/end?id="+id, nil)
	b, _ := os.ReadFile(filepath.Join(base, "active.json"))
	var act struct{ Session string }
	json.Unmarshal(b, &act)
	if act.Session != "" {
		t.Errorf("still active: %s", b)
	}
}
