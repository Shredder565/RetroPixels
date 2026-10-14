//go:build !windows

package main

import (
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func volRequest(t *testing.T, mux http.Handler, method, body, tok string) (int, map[string]any) {
	t.Helper()
	req := httptest.NewRequest(method, "/api/dol/volume", strings.NewReader(body))
	req.Host = fmt.Sprintf("127.0.0.1:%d", port)
	req.Header.Set("X-Retro-Token", tok)
	w := httptest.NewRecorder()
	mux.ServeHTTP(w, req)
	var out map[string]any
	_ = json.Unmarshal(w.Body.Bytes(), &out)
	return w.Code, out
}

func TestDolphinVolumeSetting(t *testing.T) {
	os.Setenv("XDG_CONFIG_HOME", t.TempDir())
	cfgMu.Lock()
	cfg = config{}
	cfgMu.Unlock()
	token = "test-token"
	mux := newMux()

	if code, _ := volRequest(t, mux, "GET", "", "wrong"); code != 403 {
		t.Fatalf("wrong token: %d", code)
	}
	code, out := volRequest(t, mux, "GET", "", token)
	if code != 200 || out["volume"] != 100.0 || out["muted"] != false {
		t.Fatalf("default: %d %v", code, out)
	}
	code, out = volRequest(t, mux, "POST", `{"volume": 35.4}`, token)
	if code != 200 || out["volume"] != 35.0 || out["muted"] != false {
		t.Fatalf("set volume: %d %v", code, out)
	}
	// Mute alone keeps the volume.
	code, out = volRequest(t, mux, "POST", `{"muted": true}`, token)
	if code != 200 || out["volume"] != 35.0 || out["muted"] != true {
		t.Fatalf("mute: %d %v", code, out)
	}
	// 0 is a real volume, not "unset".
	code, out = volRequest(t, mux, "POST", `{"volume": 0, "muted": false}`, token)
	if code != 200 || out["volume"] != 0.0 || out["muted"] != false {
		t.Fatalf("zero: %d %v", code, out)
	}
	for _, bad := range []string{`{"volume": 101}`, `{"volume": -1}`, `{"volume": "loud"}`} {
		if code, out := volRequest(t, mux, "POST", bad, token); code != 400 {
			t.Fatalf("%s accepted: %d %v", bad, code, out)
		}
	}
	if v, m := dolphinVolume(); v != 0 || m {
		t.Fatalf("bad values changed the setting: %d %v", v, m)
	}

	// Saved in the config file, and read back after a restart.
	b, err := os.ReadFile(filepath.Join(configDir(), "config.json"))
	if err != nil || !strings.Contains(string(b), `"dolphinVolume": 0`) {
		t.Fatalf("config.json: %v\n%s", err, b)
	}
	volRequest(t, mux, "POST", `{"volume": 80, "muted": true}`, token)
	cfgMu.Lock()
	cfg = config{}
	cfgMu.Unlock()
	loadConfig()
	if v, m := dolphinVolume(); v != 80 || !m {
		t.Fatalf("after reload: %d %v", v, m)
	}
	if st := dolStatus(); st["volume"] != 80 || st["muted"] != true {
		t.Fatalf("status: %v %v", st["volume"], st["muted"])
	}
}
