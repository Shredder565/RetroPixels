package main

import (
	"bytes"
	"encoding/binary"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"strings"
	"testing"
	"time"
)

// The test ROM comes from RP_N64_TEST_ROM (a CPU test that draws text works best).
func n64TestROM(t *testing.T) []byte {
	p := os.Getenv("RP_N64_TEST_ROM")
	if p == "" {
		p = "/tmp/claude-0/roms/pl/CPUTest/CPU/ADD/CPUADD.N64"
	}
	b, err := os.ReadFile(p)
	if err != nil {
		t.Skip("no Nintendo 64 test ROM (set RP_N64_TEST_ROM)")
	}
	return b
}

func n64Start(rate int, save, rom []byte) []byte {
	b := []byte{n64MsgStart, 0, 0, 0, 0, 0, 0, 0, 0}
	binary.LittleEndian.PutUint32(b[1:], uint32(rate))
	binary.LittleEndian.PutUint32(b[5:], uint32(len(save)))
	b = append(b, save...)
	return append(b, rom...)
}

func TestN64TokenRequired(t *testing.T) {
	token = "test-token"
	srv := httptest.NewServer(newMux())
	defer srv.Close()
	addr := strings.TrimPrefix(srv.URL, "http://")
	if c, status := wsDial(t, addr, "/api/n64/ws?token=wrong", nil); c != nil || !strings.HasPrefix(status, "403") {
		t.Fatalf("wrong token: %s", status)
	}
	if c, status := wsDial(t, addr, "/api/n64/ws?token=test-token", map[string]string{"Origin": "http://evil.example"}); c != nil || !strings.HasPrefix(status, "403") {
		t.Fatalf("other website: %s", status)
	}
}

func TestN64Session(t *testing.T) {
	rom := n64TestROM(t)
	token = "test-token"
	srv := httptest.NewServer(newMux())
	defer srv.Close()
	addr := strings.TrimPrefix(srv.URL, "http://")
	c, status := wsDial(t, addr, "/api/n64/ws?token=test-token", map[string]string{"Origin": "http://127.0.0.1:47821"})
	if c == nil {
		t.Fatal(status)
	}
	c.frame(true, wsBinary, n64Start(48000, nil, rom))

	_, msg := c.read(t)
	if msg[0] != n64MsgReady {
		t.Fatalf("expected ready, got %x %q", msg[0], msg[1:])
	}
	var hello struct {
		Session string
		FPS     float64
		Title   string
	}
	if err := json.Unmarshal(msg[1:], &hello); err != nil || hello.Session == "" || hello.FPS != 60 {
		t.Fatalf("hello %s: %v", msg[1:], err)
	}
	_, msg = c.read(t)
	if msg[0] != n64MsgSave || string(msg[1:5]) != "RP64" {
		t.Fatalf("expected the battery save, got %x", msg[:5])
	}

	tick := make([]byte, 22)
	tick[0], tick[1] = n64MsgTick, 1
	frames, sound, nonBlack := 0, 0, 0
	var w, h int
	for frames < 120 {
		c.frame(true, wsBinary, tick)
		_, msg = c.read(t)
		if msg[0] == n64MsgSave {
			continue
		}
		if msg[0] != n64MsgFrame {
			t.Fatalf("message %x: %q", msg[0], msg[1:])
		}
		frames += int(msg[1])
		w, h = int(binary.LittleEndian.Uint16(msg[4:])), int(binary.LittleEndian.Uint16(msg[6:]))
		n := int(binary.LittleEndian.Uint32(msg[8:]))
		if len(msg) != 12+w*h*4+n*4 {
			t.Fatalf("frame size %d for %dx%d + %d", len(msg), w, h, n)
		}
		sound += n
		nonBlack = 0
		for i := 12; i < 12+w*h*4; i += 4 {
			if msg[i]|msg[i+1]|msg[i+2] != 0 {
				nonBlack++
			}
		}
	}
	t.Logf("%q: %dx%d, %d lit pixels, %d sound frames", hello.Title, w, h, nonBlack, sound)
	if nonBlack == 0 {
		t.Error("the picture is all black")
	}
	// silent games still send silence at the right pace (2 s = 96000 at 48 kHz)
	if sound < 90000 || sound > 100000 {
		t.Errorf("%d sound frames for 2 seconds", sound)
	}

	// save state round trip through the HTTP side
	get := func(method string, body []byte) (int, []byte) {
		req, _ := http.NewRequest(method, srv.URL+"/api/n64/state?s="+hello.Session, bytes.NewReader(body))
		req.Host = "127.0.0.1:47821"
		req.Header.Set("X-Retro-Token", "test-token")
		resp, err := http.DefaultClient.Do(req)
		if err != nil {
			t.Fatal(err)
		}
		defer resp.Body.Close()
		b, _ := io.ReadAll(resp.Body)
		return resp.StatusCode, b
	}
	code, st := get("GET", nil)
	if code != 200 || string(st[:4]) != "RPN1" {
		t.Fatalf("save state: %d %.80q", code, st)
	}
	if code, b := get("POST", st); code != 200 {
		t.Fatalf("load state: %d %s", code, b)
	}
	if code, _ := get("POST", []byte("nonsense")); code == 200 {
		t.Fatal("loading nonsense worked")
	}

	// reset, then more frames still come
	c.frame(true, wsBinary, []byte{n64MsgReset})
	c.frame(true, wsBinary, tick)
	if _, msg = c.read(t); msg[0] != n64MsgFrame && msg[0] != n64MsgSave {
		t.Fatalf("after reset: %x", msg[0])
	}

	// closing frees the session
	c.frame(true, wsClose, nil)
	for {
		op, _ := c.read(t)
		if op == wsClose {
			break
		}
	}
	c.c.Close()
	// the server frees the session just after it answers the close
	code = 0
	for i := 0; i < 100; i++ {
		if code, _ = get("GET", nil); code == http.StatusNotFound {
			break
		}
		time.Sleep(20 * time.Millisecond)
	}
	if code != http.StatusNotFound {
		t.Fatalf("session still there after close: %d", code)
	}
}

func TestN64BadROM(t *testing.T) {
	token = "test-token"
	srv := httptest.NewServer(newMux())
	defer srv.Close()
	c, status := wsDial(t, strings.TrimPrefix(srv.URL, "http://"), "/api/n64/ws?token=test-token", nil)
	if c == nil {
		t.Fatal(status)
	}
	c.frame(true, wsBinary, n64Start(48000, nil, []byte("not a game at all")))
	_, msg := c.read(t)
	if msg[0] != n64MsgError {
		t.Fatalf("expected an error, got %x", msg[0])
	}
	t.Logf("message: %s", msg[1:])
}
