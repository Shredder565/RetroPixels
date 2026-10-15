package main

// Tests for the Archipelago bridge: a stand-in page answers memory operations
// the way web/emu/archipelago.js does, and the tests talk to RetroPixels the
// way Archipelago's BizHawk Client and SNI Client do.

import (
	"bufio"
	"bytes"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"
)

// fakePage connects to /api/ap/ws and keeps memory domains in Go slices.
type fakePage struct {
	c       *wsTestClient
	domains map[string][]byte
	msgs    []string
}

func startFakePage(t *testing.T, addr string, game apGame, domains map[string][]byte) *fakePage {
	t.Helper()
	c, status := wsDial(t, addr, "/api/ap/ws?token=test-token", map[string]string{"Origin": "http://127.0.0.1:47821"})
	if c == nil {
		t.Fatal(status)
	}
	p := &fakePage{c: c, domains: domains}
	b, _ := json.Marshal(map[string]any{"ev": "game", "game": game})
	c.frame(true, wsText, b)
	go p.serve(t)
	// wait until RetroPixels has the game
	for i := 0; i < 200; i++ {
		if g, _ := ap.Current(); g != nil && g.Hash == game.Hash {
			return p
		}
		time.Sleep(10 * time.Millisecond)
	}
	t.Fatal("the page's game never arrived")
	return nil
}

func (p *fakePage) serve(t *testing.T) {
	for {
		var h [2]byte
		if _, err := io.ReadFull(p.c.r, h[:]); err != nil {
			return
		}
		n := uint64(h[1] & 0x7f)
		if n == 126 {
			var b [2]byte
			if _, err := io.ReadFull(p.c.r, b[:]); err != nil {
				return
			}
			n = uint64(b[0])<<8 | uint64(b[1])
		} else if n == 127 {
			var b [8]byte
			if _, err := io.ReadFull(p.c.r, b[:]); err != nil {
				return
			}
			n = 0
			for _, x := range b {
				n = n<<8 | uint64(x)
			}
		}
		data := make([]byte, n)
		if _, err := io.ReadFull(p.c.r, data); err != nil {
			return
		}
		var m struct {
			ID  uint64 `json:"id"`
			Ops []apOp `json:"ops"`
		}
		if json.Unmarshal(data, &m) != nil || m.Ops == nil {
			continue
		}
		b, _ := json.Marshal(map[string]any{"id": m.ID, "res": p.run(m.Ops)})
		p.c.frame(true, wsText, b)
	}
}

// run: the same rules as archipelago.js's Bridge.run.
func (p *fakePage) run(ops []apOp) []apRes {
	var out []apRes
	for _, o := range ops {
		var r apRes
		d, ok := p.domains[o.D]
		switch o.T {
		case "size":
			if !ok {
				r.Err = "no memory domain named " + o.D
			}
			r.N = int64(len(d))
		case "read":
			if !ok || (!o.Pad && o.A+o.N > int64(len(d))) {
				r.Err = "bad read"
				break
			}
			r.V = make([]byte, o.N)
			if o.A < int64(len(d)) {
				copy(r.V, d[o.A:])
			}
		case "write":
			if !ok || (!o.Pad && o.A+int64(len(o.V)) > int64(len(d))) {
				r.Err = "bad write"
				break
			}
			if o.A < int64(len(d)) {
				copy(d[o.A:], o.V)
			}
		case "guard":
			r.OK = ok && o.A+int64(len(o.V)) <= int64(len(d)) && bytes.Equal(d[o.A:o.A+int64(len(o.V))], o.V)
		case "msg":
			p.msgs = append(p.msgs, o.Text)
		}
		out = append(out, r)
		if o.T == "guard" && !r.OK {
			break
		}
	}
	return out
}

func apTestServer(t *testing.T) (string, func()) {
	token = "test-token"
	srv := httptest.NewServer(newMux())
	return strings.TrimPrefix(srv.URL, "http://"), srv.Close
}

func waitNoGame(t *testing.T) {
	for i := 0; i < 200; i++ {
		if g, _ := ap.Current(); g == nil {
			return
		}
		time.Sleep(10 * time.Millisecond)
	}
	t.Fatal("the game didn't go away")
}

func TestAPPageNeedsToken(t *testing.T) {
	addr, done := apTestServer(t)
	defer done()
	if c, status := wsDial(t, addr, "/api/ap/ws?token=nope", nil); c != nil || !strings.HasPrefix(status, "403") {
		t.Fatalf("wrong token: %s", status)
	}
	if c, status := wsDial(t, addr, "/api/ap/ws?token=test-token", map[string]string{"Origin": "http://evil.example"}); c != nil || !strings.HasPrefix(status, "403") {
		t.Fatalf("other website: %s", status)
	}
}

// bizhawk sends one line and reads one line back, like the BizHawk Client.
func bizhawk(t *testing.T, rw *bufio.ReadWriter, line string) string {
	t.Helper()
	if _, err := rw.WriteString(line + "\n"); err != nil {
		t.Fatal(err)
	}
	_ = rw.Flush()
	s, err := rw.ReadString('\n')
	if err != nil {
		t.Fatal(err)
	}
	return strings.TrimSuffix(s, "\n")
}

func TestAPBizHawkProtocol(t *testing.T) {
	addr, done := apTestServer(t)
	defer done()
	rom := make([]byte, 0x8000)
	copy(rom[0x134:], "POKEMON RED")
	wram := make([]byte, 0x2000)
	copy(wram[0x100:], []byte{1, 2, 3, 4})
	page := startFakePage(t, addr, apGame{Core: "gb", System: "GB", Title: "test", Hash: "ABC123"},
		map[string][]byte{"ROM": rom, "WRAM": wram})

	a, b := net.Pipe()
	defer a.Close()
	go ap.ServeBizHawk(b)
	_ = a.SetDeadline(time.Now().Add(10 * time.Second))
	rw := bufio.NewReadWriter(bufio.NewReader(a), bufio.NewWriter(a))

	if got := bizhawk(t, rw, "VERSION"); got != "1" {
		t.Fatalf("VERSION = %q", got)
	}
	var res []map[string]any
	ask := func(reqs ...map[string]any) []map[string]any {
		t.Helper()
		line, _ := json.Marshal(reqs)
		res = nil
		if err := json.Unmarshal([]byte(bizhawk(t, rw, string(line))), &res); err != nil {
			t.Fatal(err)
		}
		if len(res) != len(reqs) {
			t.Fatalf("%d answers for %d requests", len(res), len(reqs))
		}
		return res
	}
	r := ask(map[string]any{"type": "PING"}, map[string]any{"type": "SYSTEM"}, map[string]any{"type": "HASH"},
		map[string]any{"type": "MEMORY_SIZE", "domain": "ROM"})
	if r[0]["type"] != "PONG" || r[1]["value"] != "GB" || r[2]["value"] != "ABC123" || r[3]["value"] != float64(0x8000) {
		t.Fatalf("handshake: %v", r)
	}
	r = ask(map[string]any{"type": "READ", "address": 0x134, "size": 11, "domain": "ROM"})
	if v, _ := base64.StdEncoding.DecodeString(r[0]["value"].(string)); string(v) != "POKEMON RED" || r[0]["type"] != "READ_RESPONSE" {
		t.Fatalf("read: %v", r)
	}
	// guarded write: guard passes, then the write lands
	b64 := func(b ...byte) string { return base64.StdEncoding.EncodeToString(b) }
	r = ask(map[string]any{"type": "GUARD", "address": 0x100, "expected_data": b64(1, 2, 3, 4), "domain": "WRAM"},
		map[string]any{"type": "WRITE", "address": 0x200, "value": b64(9, 8, 7), "domain": "WRAM"})
	if r[0]["value"] != true || r[1]["type"] != "WRITE_RESPONSE" || !bytes.Equal(wram[0x200:0x203], []byte{9, 8, 7}) {
		t.Fatalf("guarded write: %v %x", r, wram[0x200:0x203])
	}
	// guard fails: the write after it doesn't happen, and every later answer is the failed guard
	r = ask(map[string]any{"type": "GUARD", "address": 0x100, "expected_data": b64(5), "domain": "WRAM"},
		map[string]any{"type": "WRITE", "address": 0x300, "value": b64(0x55), "domain": "WRAM"},
		map[string]any{"type": "PING"})
	if r[0]["value"] != false || r[1]["type"] != "GUARD_RESPONSE" || r[2]["type"] != "GUARD_RESPONSE" || wram[0x300] != 0 {
		t.Fatalf("failed guard: %v", r)
	}
	// errors come back as ERROR answers, like the connector script's
	r = ask(map[string]any{"type": "READ", "address": 0, "size": 4, "domain": "No Such RAM"}, map[string]any{"type": "BOGUS"})
	if r[0]["type"] != "ERROR" || r[1]["type"] != "ERROR" {
		t.Fatalf("errors: %v", r)
	}
	r = ask(map[string]any{"type": "DISPLAY_MESSAGE", "message": "Got the Hookshot"}, map[string]any{"type": "LOCK"},
		map[string]any{"type": "UNLOCK"}, map[string]any{"type": "SET_MESSAGE_INTERVAL", "value": 0.5},
		map[string]any{"type": "PREFERRED_CORES"})
	if r[0]["type"] != "DISPLAY_MESSAGE_RESPONSE" || r[1]["type"] != "LOCKED" || r[2]["type"] != "UNLOCKED" ||
		r[3]["type"] != "SET_MESSAGE_INTERVAL_RESPONSE" || r[4]["type"] != "PREFERRED_CORES_RESPONSE" || len(page.msgs) != 1 {
		t.Fatalf("misc: %v %v", r, page.msgs)
	}

	// the indicator hears about the client
	st := ap.Status()
	if len(st.Clients) != 1 || st.Clients[0] != "BizHawk Client" {
		t.Fatalf("status: %+v", st)
	}

	// the page goes away: the client is told there's no game
	page.c.c.Close()
	waitNoGame(t)
	r = ask(map[string]any{"type": "SYSTEM"}, map[string]any{"type": "READ", "address": 0, "size": 1, "domain": "ROM"})
	if r[0]["value"] != "NULL" || r[1]["type"] != "ERROR" {
		t.Fatalf("no game: %v", r)
	}
}

// sni is a usb2snes client like Archipelago's SNI Client.
type sniTest struct {
	t *testing.T
	c *wsTestClient
}

func (s sniTest) send(v map[string]any) {
	b, _ := json.Marshal(v)
	s.c.frame(true, wsText, b)
}

func (s sniTest) results() []string {
	op, b := s.c.read(s.t)
	if op != wsText {
		s.t.Fatalf("expected JSON, got op %d", op)
	}
	var m struct{ Results []string }
	if err := json.Unmarshal(b, &m); err != nil {
		s.t.Fatal(err)
	}
	return m.Results
}

func (s sniTest) read(addr, size int) []byte {
	s.send(map[string]any{"Opcode": "GetAddress", "Space": "SNES", "Operands": []string{fmt.Sprintf("%x", addr), fmt.Sprintf("%x", size)}})
	var got []byte
	for len(got) < size {
		op, b := s.c.read(s.t)
		if op != wsBinary {
			s.t.Fatalf("expected data, got op %d", op)
		}
		got = append(got, b...)
	}
	return got
}

func TestAPSNIProtocol(t *testing.T) {
	addr, done := apTestServer(t)
	defer done()
	sniSrv := httptest.NewServer(http.HandlerFunc(ap.ServeSNIHTTP))
	defer sniSrv.Close()
	sniAddr := strings.TrimPrefix(sniSrv.URL, "http://")

	// browsers on other websites can't reach it; programs and local pages can
	if c, status := wsDial(t, sniAddr, "/", map[string]string{"Origin": "https://evil.example"}); c != nil || !strings.HasPrefix(status, "403") {
		t.Fatalf("website origin: %s", status)
	}

	c, status := wsDial(t, sniAddr, "/", nil)
	if c == nil {
		t.Fatal(status)
	}
	s := sniTest{t, c}
	// no SNES game yet: no devices
	s.send(map[string]any{"Opcode": "DeviceList", "Space": "SNES", "Operands": []string{}})
	if d := s.results(); len(d) != 0 {
		t.Fatalf("devices with no game: %v", d)
	}

	rom := make([]byte, 0x10000)
	copy(rom[0x7fc0:], "SM12345")
	sram := make([]byte, 0x2000)
	copy(sram[0x2000-4:], []byte{0xde, 0xad, 0xbe, 0xef})
	wram := make([]byte, 0x20000)
	wram[0x1f000] = 0x42
	page := startFakePage(t, addr, apGame{Core: "snes", System: "SNES", Title: "SUPER METROID", Hash: "SNES1"},
		map[string][]byte{"CARTROM": rom, "CARTRAM": sram, "WRAM": wram})
	defer page.c.c.Close()

	s.send(map[string]any{"Opcode": "DeviceList", "Space": "SNES", "Operands": []string{}})
	d := s.results()
	if len(d) != 1 {
		t.Fatalf("devices: %v", d)
	}
	s.send(map[string]any{"Opcode": "AppVersion"})
	if v := s.results(); len(v) != 1 || !strings.Contains(v[0], "SNI") {
		t.Fatalf("AppVersion: %v (the SNI Client warns unless it says SNI)", v)
	}
	s.send(map[string]any{"Opcode": "Attach", "Space": "SNES", "Operands": d})
	s.send(map[string]any{"Opcode": "Info", "Space": "SNES"})
	if v := s.results(); len(v) < 3 || v[2] != "SUPER METROID" {
		t.Fatalf("Info: %v", v)
	}
	// ROM, SRAM and WRAM in the SNI address space
	if got := s.read(0x7fc0, 7); string(got) != "SM12345" {
		t.Fatalf("ROM read: %q", got)
	}
	if got := s.read(0xE00000+0x2000-4, 8); !bytes.Equal(got, []byte{0xde, 0xad, 0xbe, 0xef, 0, 0, 0, 0}) {
		t.Fatalf("SRAM read past its end: %x", got)
	}
	if got := s.read(0xF50000+0x1f000, 1); got[0] != 0x42 {
		t.Fatalf("WRAM read: %x", got)
	}
	// a read across the end of WRAM into VRAM (not in the fake page) and an unmapped area
	if got := s.read(0xF6FFFE, 4); len(got) != 4 {
		t.Fatalf("split read: %x", got)
	}
	// PutAddress: the data comes in a separate binary message
	s.send(map[string]any{"Opcode": "PutAddress", "Space": "SNES", "Operands": []string{"f5f000", "3"}})
	c.frame(true, wsBinary, []byte{7, 8, 9})
	if got := s.read(0xF5F000, 3); !bytes.Equal(got, []byte{7, 8, 9}) || !bytes.Equal(wram[0xf000:0xf003], []byte{7, 8, 9}) {
		t.Fatalf("WRAM write: %x", got)
	}
	st := ap.Status()
	if len(st.Clients) != 1 || st.Clients[0] != "SNI Client" {
		t.Fatalf("status: %+v", st)
	}
}
