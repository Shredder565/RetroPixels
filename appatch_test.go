package main

// The page's routes for Archipelago patch files (the reader itself is tested
// in internal/apatch).

import (
	"bytes"
	"encoding/binary"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"retropixels/internal/apatch"
)

func apTestFile(t *testing.T, name string) []byte {
	t.Helper()
	b, err := os.ReadFile(filepath.Join("testdata", "archipelago", name))
	if err != nil {
		t.Fatal(err)
	}
	return b
}

func apExpectMD5(t *testing.T) string {
	var e struct {
		MD5 string `json:"procedure_result_md5"`
	}
	if err := json.Unmarshal(apTestFile(t, "expected.json"), &e); err != nil {
		t.Fatal(err)
	}
	return e.MD5
}

func TestAPPatchHTTP(t *testing.T) {
	token = "test-token"
	hostOK["test.local"] = true
	srv := httptest.NewServer(newMux())
	defer srv.Close()
	post := func(path string, body []byte) (*http.Response, []byte) {
		req, _ := http.NewRequest(http.MethodPost, srv.URL+path, bytes.NewReader(body))
		req.Host = "test.local"
		req.Header.Set("X-Retro-Token", "test-token")
		res, err := http.DefaultClient.Do(req)
		if err != nil {
			t.Fatal(err)
		}
		defer res.Body.Close()
		b, _ := io.ReadAll(res.Body)
		return res, b
	}
	patch := apTestFile(t, "AP_test_P1_Tester.aprptest")
	res, b := post("/api/ap/inspect?name=AP_test_P1_Tester.aprptest", patch)
	if res.StatusCode != 200 || !strings.Contains(string(b), `"system":"snes"`) || !strings.Contains(string(b), `"playerName":"Tester"`) {
		t.Fatalf("inspect: %d %s", res.StatusCode, b)
	}
	body := make([]byte, 4)
	binary.LittleEndian.PutUint32(body, uint32(len(patch)))
	body = append(append(body, patch...), apTestFile(t, "procedure_base.bin")...)
	res, b = post("/api/ap/patch", body)
	if res.StatusCode != 200 || apatch.MD5Hex(b) != apExpectMD5(t) {
		t.Fatalf("patch: %d %.200s", res.StatusCode, b)
	}
	body[len(body)-1] ^= 1
	res, b = post("/api/ap/patch", body)
	if res.StatusCode != 400 || !strings.Contains(string(b), `"code":"wrongBase"`) {
		t.Fatalf("wrong base: %d %s", res.StatusCode, b)
	}
	// without the token: refused
	req, _ := http.NewRequest(http.MethodPost, srv.URL+"/api/ap/inspect", bytes.NewReader(patch))
	req.Host = "test.local"
	r2, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	r2.Body.Close()
	if r2.StatusCode != 403 {
		t.Fatalf("no token: %d", r2.StatusCode)
	}
}
