package apbridge

// Tests against an emulator stand-in (memory in Go slices, carried out with
// Exec, as RetroPixels Native does), talking the way Archipelago's BizHawk
// Client and SNI Client do. RetroPixels.exe's page bridge is tested in the
// top-level package.

import (
	"bufio"
	"bytes"
	"encoding/base64"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"
	"time"
)

// fakeMem: domains in slices; "Bus" mirrors "WRAM" over 0x10000 bytes the way
// a System Bus domain would, through per-byte calls.
type fakeMem struct {
	d map[string][]byte
}

func (m *fakeMem) MemoryDomains() []string {
	out := []string{"Bus"}
	for k := range m.d {
		out = append(out, k)
	}
	return out
}

func (m *fakeMem) MemorySize(d string) int {
	if d == "Bus" {
		return 0x10000
	}
	return len(m.d[d])
}

func (m *fakeMem) ReadMemory(d string, a uint32, buf []byte) error {
	if d == "Bus" {
		w := m.d["WRAM"]
		for i := range buf {
			buf[i] = w[(int(a)+i)%len(w)]
		}
		return nil
	}
	mem := m.d[d]
	if int(a)+len(buf) > len(mem) {
		return fmt.Errorf("Exec read past the end of %s", d)
	}
	copy(buf, mem[a:])
	return nil
}

func (m *fakeMem) WriteMemory(d string, a uint32, data []byte) error {
	if d == "Bus" {
		w := m.d["WRAM"]
		for i, v := range data {
			w[(int(a)+i)%len(w)] = v
		}
		return nil
	}
	mem := m.d[d]
	if int(a)+len(data) > len(mem) {
		return fmt.Errorf("Exec wrote past the end of %s", d)
	}
	copy(mem[a:], data)
	return nil
}

// fakeTarget runs batches with Exec, like RetroPixels Native's emulation thread.
type fakeTarget struct {
	mu     sync.Mutex
	mem    *fakeMem
	msgs   []string
	locked bool
}

func (t *fakeTarget) Run(ops []Op) ([]Res, error) {
	t.mu.Lock()
	defer t.mu.Unlock()
	return Exec(t.mem, ops, func(o Op) {
		switch o.T {
		case "msg":
			t.msgs = append(t.msgs, o.Text)
		case "lock":
			t.locked = true
		case "unlock":
			t.locked = false
		}
	}), nil
}

func TestExec(t *testing.T) {
	m := &fakeMem{d: map[string][]byte{"WRAM": make([]byte, 0x100), "ROM": []byte("HELLO WORLD")}}
	res := Exec(m, []Op{
		{T: "size", D: "ROM"},
		{T: "read", D: "ROM", A: 6, N: 5},
		{T: "read", D: "ROM", A: 8, N: 5},             // past the end: refused
		{T: "read", D: "ROM", A: 8, N: 5, Pad: true},  // padded with zeros
		{T: "read", D: "ROM", A: 50, N: 2, Pad: true}, // wholly past the end
		{T: "write", D: "WRAM", A: 0xfe, V: []byte{1, 2, 3}, Pad: true},
		{T: "write", D: "WRAM", A: 0xfe, V: []byte{1, 2, 3}},
		{T: "read", D: "Nope", N: 1},
		{T: "bogus"},
		{T: "write", D: "Bus", A: 0x110, V: []byte{7}}, // the bus mirrors WRAM
		{T: "guard", D: "WRAM", A: 0x10, V: []byte{7}},
		{T: "guard", D: "ROM", A: 10, V: []byte{'D', 0}, Pad: true},
		{T: "guard", D: "ROM", A: 0, V: []byte{'X'}},
		{T: "write", D: "WRAM", A: 0, V: []byte{9}}, // after a failed guard: not done
	}, nil)
	if len(res) != 13 {
		t.Fatalf("%d answers, want 13 (stopping at the failed guard): %+v", len(res), res)
	}
	if res[0].N != 11 || string(res[1].V) != "WORLD" || res[2].Err == "" || !bytes.Equal(res[3].V, []byte("RLD\x00\x00")) ||
		!bytes.Equal(res[4].V, []byte{0, 0}) || res[5].Err != "" || res[6].Err == "" ||
		!strings.Contains(res[7].Err, `no memory domain named "Nope"`) || res[8].Err == "" {
		t.Fatalf("answers: %+v", res)
	}
	if m.d["WRAM"][0xfe] != 1 || m.d["WRAM"][0xff] != 2 || m.d["WRAM"][0x10] != 7 {
		t.Fatalf("writes: %x", m.d["WRAM"])
	}
	if !res[10].OK || !res[11].OK || res[12].OK || m.d["WRAM"][0] != 0 {
		t.Fatalf("guards: %+v", res[10:])
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

func TestBizHawkProtocol(t *testing.T) {
	h := NewHub(0, 0)
	changes := make(chan struct{}, 100)
	h.OnChange = func() { changes <- struct{}{} }
	rom := make([]byte, 0x8000)
	copy(rom[0x134:], "POKEMON RED")
	wram := make([]byte, 0x2000)
	copy(wram[0x100:], []byte{1, 2, 3, 4})
	tg := &fakeTarget{mem: &fakeMem{d: map[string][]byte{"ROM": rom, "WRAM": wram}}}

	a, b := net.Pipe()
	defer a.Close()
	go h.ServeBizHawk(b)
	_ = a.SetDeadline(time.Now().Add(10 * time.Second))
	rw := bufio.NewReadWriter(bufio.NewReader(a), bufio.NewWriter(a))
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

	if got := bizhawk(t, rw, "VERSION"); got != "1" {
		t.Fatalf("VERSION = %q", got)
	}
	// no game yet
	r := ask(map[string]any{"type": "SYSTEM"}, map[string]any{"type": "READ", "address": 0, "size": 1, "domain": "ROM"})
	if r[0]["value"] != "NULL" || r[1]["type"] != "ERROR" {
		t.Fatalf("no game: %v", r)
	}
	if st := h.Status(); len(st.Clients) != 1 || st.Clients[0] != BizHawkClient || st.Game {
		t.Fatalf("status: %+v", st)
	}

	h.SetGame(&Game{Core: "gb", System: "GBC", Title: "test", Hash: "ABC123"}, tg)
	r = ask(map[string]any{"type": "PING"}, map[string]any{"type": "SYSTEM"}, map[string]any{"type": "HASH"},
		map[string]any{"type": "MEMORY_SIZE", "domain": "ROM"})
	if r[0]["type"] != "PONG" || r[1]["value"] != "GBC" || r[2]["value"] != "ABC123" || r[3]["value"] != float64(0x8000) {
		t.Fatalf("handshake: %v", r)
	}
	r = ask(map[string]any{"type": "READ", "address": 0x134, "size": 11, "domain": "ROM"})
	if v, _ := base64.StdEncoding.DecodeString(r[0]["value"].(string)); string(v) != "POKEMON RED" || r[0]["type"] != "READ_RESPONSE" {
		t.Fatalf("read: %v", r)
	}
	b64 := func(b ...byte) string { return base64.StdEncoding.EncodeToString(b) }
	// LOCK, guarded write, read back, UNLOCK: all in one batch, as the client's guarded_write does
	r = ask(map[string]any{"type": "LOCK"},
		map[string]any{"type": "GUARD", "address": 0x100, "expected_data": b64(1, 2, 3, 4), "domain": "WRAM"},
		map[string]any{"type": "WRITE", "address": 0x200, "value": b64(9, 8, 7), "domain": "WRAM"},
		map[string]any{"type": "READ", "address": 0x200, "size": 3, "domain": "WRAM"})
	if r[0]["type"] != "LOCKED" || r[1]["value"] != true || r[2]["type"] != "WRITE_RESPONSE" || r[3]["value"] != b64(9, 8, 7) || !tg.locked {
		t.Fatalf("locked guarded write: %v", r)
	}
	r = ask(map[string]any{"type": "UNLOCK"})
	if r[0]["type"] != "UNLOCKED" || tg.locked {
		t.Fatalf("unlock: %v", r)
	}
	// guard fails: the write after it doesn't happen, and every later answer is the failed guard
	r = ask(map[string]any{"type": "GUARD", "address": 0x100, "expected_data": b64(5), "domain": "WRAM"},
		map[string]any{"type": "WRITE", "address": 0x300, "value": b64(0x55), "domain": "WRAM"},
		map[string]any{"type": "PING"})
	if r[0]["value"] != false || r[1]["type"] != "GUARD_RESPONSE" || r[2]["type"] != "GUARD_RESPONSE" || wram[0x300] != 0 {
		t.Fatalf("failed guard: %v", r)
	}
	r = ask(map[string]any{"type": "READ", "address": 0, "size": 4, "domain": "No Such RAM"}, map[string]any{"type": "BOGUS"},
		map[string]any{"type": "READ", "address": 0x1ffe, "size": 4, "domain": "WRAM"})
	if r[0]["type"] != "ERROR" || r[1]["type"] != "ERROR" || r[2]["type"] != "ERROR" {
		t.Fatalf("errors: %v", r)
	}
	r = ask(map[string]any{"type": "DISPLAY_MESSAGE", "message": "Got the Hookshot"}, map[string]any{"type": "SET_MESSAGE_INTERVAL", "value": 0.5},
		map[string]any{"type": "PREFERRED_CORES"})
	if r[0]["type"] != "DISPLAY_MESSAGE_RESPONSE" || r[1]["type"] != "SET_MESSAGE_INTERVAL_RESPONSE" || r[2]["type"] != "PREFERRED_CORES_RESPONSE" ||
		len(tg.msgs) != 1 || tg.msgs[0] != "Got the Hookshot" {
		t.Fatalf("misc: %v %v", r, tg.msgs)
	}
	// the game goes away; another target clearing doesn't count
	h.ClearGame(&fakeTarget{})
	if g, _ := h.Current(); g == nil {
		t.Fatal("cleared by a target that wasn't running the game")
	}
	h.ClearGame(tg)
	r = ask(map[string]any{"type": "SYSTEM"})
	if r[0]["value"] != "NULL" {
		t.Fatalf("after the game: %v", r)
	}
	a.Close()
	for i := 0; i < 100 && len(h.Status().Clients) > 0; i++ {
		time.Sleep(10 * time.Millisecond)
	}
	if st := h.Status(); len(st.Clients) != 0 {
		t.Fatalf("client still listed: %+v", st)
	}
	if len(changes) == 0 {
		t.Fatal("OnChange never called")
	}
}

// a tiny WebSocket client, like the SNI Client's
type wsClient struct {
	c net.Conn
	r *bufio.Reader
	t *testing.T
}

func wsDial(t *testing.T, addr string, header map[string]string) (*wsClient, string) {
	t.Helper()
	c, err := net.Dial("tcp", addr)
	if err != nil {
		t.Fatal(err)
	}
	_ = c.SetDeadline(time.Now().Add(30 * time.Second))
	req := "GET / HTTP/1.1\r\nHost: localhost:23074\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n" +
		"Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n"
	for k, v := range header {
		req += k + ": " + v + "\r\n"
	}
	if _, err := io.WriteString(c, req+"\r\n"); err != nil {
		t.Fatal(err)
	}
	r := bufio.NewReader(c)
	resp, err := http.ReadResponse(r, nil)
	if err != nil {
		t.Fatal(err)
	}
	if resp.StatusCode != 101 {
		c.Close()
		return nil, resp.Status
	}
	return &wsClient{c: c, r: r, t: t}, resp.Status
}

func (w *wsClient) frame(op byte, data []byte) {
	var b bytes.Buffer
	b.WriteByte(0x80 | op)
	switch n := len(data); {
	case n < 126:
		b.WriteByte(0x80 | byte(n))
	default:
		b.WriteByte(0x80 | 126)
		_ = binary.Write(&b, binary.BigEndian, uint16(n))
	}
	b.Write([]byte{1, 2, 3, 4})
	for i, c := range data {
		b.WriteByte(c ^ byte(i&3+1))
	}
	_, _ = w.c.Write(b.Bytes())
}

func (w *wsClient) send(v map[string]any) {
	b, _ := json.Marshal(v)
	w.frame(1, b)
}

func (w *wsClient) read() (byte, []byte) {
	w.t.Helper()
	var h [2]byte
	if _, err := io.ReadFull(w.r, h[:]); err != nil {
		w.t.Fatal(err)
	}
	n := int(h[1] & 0x7f)
	if n == 126 {
		var b [2]byte
		io.ReadFull(w.r, b[:])
		n = int(binary.BigEndian.Uint16(b[:]))
	} else if n == 127 {
		var b [8]byte
		io.ReadFull(w.r, b[:])
		n = int(binary.BigEndian.Uint64(b[:]))
	}
	data := make([]byte, n)
	if _, err := io.ReadFull(w.r, data); err != nil {
		w.t.Fatal(err)
	}
	return h[0] & 15, data
}

func (w *wsClient) results() []string {
	w.t.Helper()
	op, b := w.read()
	var m struct{ Results []string }
	if op != 1 || json.Unmarshal(b, &m) != nil {
		w.t.Fatalf("expected JSON results, got op %d %q", op, b)
	}
	return m.Results
}

func (w *wsClient) get(addr, size int) []byte {
	w.t.Helper()
	w.send(map[string]any{"Opcode": "GetAddress", "Space": "SNES", "Operands": []string{fmt.Sprintf("%x", addr), fmt.Sprintf("%x", size)}})
	var got []byte
	for len(got) < size {
		op, b := w.read()
		if op != 2 {
			w.t.Fatalf("expected data, got op %d", op)
		}
		got = append(got, b...)
	}
	return got
}

func TestSNIProtocol(t *testing.T) {
	h := NewHub(0, 0)
	h.Name = "RetroPixels Native 9.9"
	srv := httptest.NewServer(http.HandlerFunc(h.ServeSNIHTTP))
	defer srv.Close()
	addr := strings.TrimPrefix(srv.URL, "http://")

	// browsers on other websites can't reach it; programs and local pages can
	if c, status := wsDial(t, addr, map[string]string{"Origin": "https://evil.example"}); c != nil || !strings.HasPrefix(status, "403") {
		t.Fatalf("website origin: %s", status)
	}
	c, status := wsDial(t, addr, nil)
	if c == nil {
		t.Fatal(status)
	}
	c.send(map[string]any{"Opcode": "DeviceList", "Space": "SNES", "Operands": []string{}})
	if d := c.results(); len(d) != 0 {
		t.Fatalf("devices with no game: %v", d)
	}
	// a Game Boy game isn't a SNES device either
	gbt := &fakeTarget{mem: &fakeMem{d: map[string][]byte{"WRAM": make([]byte, 0x2000)}}}
	h.SetGame(&Game{Core: "gb", System: "GB"}, gbt)
	c.send(map[string]any{"Opcode": "DeviceList", "Space": "SNES", "Operands": []string{}})
	if d := c.results(); len(d) != 0 {
		t.Fatalf("devices with a Game Boy game: %v", d)
	}

	rom := make([]byte, 0x10000)
	copy(rom[0x7fc0:], "SM12345")
	sram := make([]byte, 0x2000)
	copy(sram[0x2000-4:], []byte{0xde, 0xad, 0xbe, 0xef})
	wram := make([]byte, 0x20000)
	wram[0x1f000] = 0x42
	tg := &fakeTarget{mem: &fakeMem{d: map[string][]byte{"CARTROM": rom, "CARTRAM": sram, "WRAM": wram,
		"VRAM": make([]byte, 0x10000), "APURAM": make([]byte, 0x10000), "CGRAM": make([]byte, 0x200), "OAM": make([]byte, 0x220)}}}
	h.SetGame(&Game{Core: "snes", System: "SNES", Title: "SUPER METROID", Hash: "SNES1"}, tg)

	c.send(map[string]any{"Opcode": "DeviceList", "Space": "SNES", "Operands": []string{}})
	d := c.results()
	if len(d) != 1 {
		t.Fatalf("devices: %v", d)
	}
	c.send(map[string]any{"Opcode": "AppVersion"})
	if v := c.results(); len(v) != 1 || !strings.Contains(v[0], "SNI") || !strings.HasPrefix(v[0], "RetroPixels Native 9.9") {
		t.Fatalf("AppVersion: %v (the SNI Client warns unless it says SNI)", v)
	}
	c.send(map[string]any{"Opcode": "Attach", "Space": "SNES", "Operands": d})
	c.send(map[string]any{"Opcode": "Info", "Space": "SNES"})
	if v := c.results(); len(v) < 3 || v[2] != "SUPER METROID" {
		t.Fatalf("Info: %v", v)
	}
	if st := h.Status(); len(st.Clients) != 1 || st.Clients[0] != SNIClient {
		t.Fatalf("status: %+v", st)
	}
	if got := c.get(0x7fc0, 7); string(got) != "SM12345" {
		t.Fatalf("ROM read: %q", got)
	}
	if got := c.get(0xE00000+0x2000-4, 8); !bytes.Equal(got, []byte{0xde, 0xad, 0xbe, 0xef, 0, 0, 0, 0}) {
		t.Fatalf("SRAM read past its end: %x", got)
	}
	if got := c.get(0xF50000+0x1f000, 1); got[0] != 0x42 {
		t.Fatalf("WRAM read: %x", got)
	}
	if got := c.get(0xF6FFFE, 4); len(got) != 4 {
		t.Fatalf("split read: %x", got)
	}
	if got := c.get(0xF00000, 16); !bytes.Equal(got, make([]byte, 16)) {
		t.Fatalf("unmapped read: %x", got)
	}
	// PutAddress: the data comes in a separate binary message
	c.send(map[string]any{"Opcode": "PutAddress", "Space": "SNES", "Operands": []string{"f5f000", "3"}})
	c.frame(2, []byte{7, 8, 9})
	if got := c.get(0xF5F000, 3); !bytes.Equal(got, []byte{7, 8, 9}) || !bytes.Equal(wram[0xf000:0xf003], []byte{7, 8, 9}) {
		t.Fatalf("WRAM write: %x", got)
	}
	// SRAM write past its end: the part inside lands
	c.send(map[string]any{"Opcode": "PutAddress", "Space": "SNES", "Operands": []string{"e01fff", "2"}})
	c.frame(2, []byte{0x5a, 0x5b})
	if got := c.get(0xE01FFF, 1); got[0] != 0x5a {
		t.Fatalf("SRAM write: %x", got)
	}
	// the game goes: the device is gone
	h.ClearGame(tg)
	c.send(map[string]any{"Opcode": "DeviceList", "Space": "SNES", "Operands": []string{}})
	if d := c.results(); len(d) != 0 {
		t.Fatalf("devices after the game: %v", d)
	}
}

func TestSNISplit(t *testing.T) {
	ops := SNISplit(0xF6FFFE, 4)
	if len(ops) != 2 || ops[0].D != "WRAM" || ops[0].A != 0x1fffe || ops[0].N != 2 || ops[1].D != "VRAM" || ops[1].A != 0 || ops[1].N != 2 {
		t.Fatalf("WRAM/VRAM: %+v", ops)
	}
	ops = SNISplit(0xEFFFFF, 3) // SRAM's last byte, then a gap
	if len(ops) != 2 || ops[0].D != "CARTRAM" || ops[1].D != "" || ops[1].N != 2 {
		t.Fatalf("SRAM/gap: %+v", ops)
	}
	ops = SNISplit(0xF90100, 0x200) // colours then sprites
	if len(ops) != 2 || ops[0].D != "CGRAM" || ops[0].A != 0x100 || ops[1].D != "OAM" || ops[1].A != 0 {
		t.Fatalf("CGRAM/OAM: %+v", ops)
	}
}

// The listeners are on the loopback address only, and the BizHawk one only
// while a game is running.
func TestListenersLoopbackOnly(t *testing.T) {
	free := func() int {
		ln, err := net.Listen("tcp4", "127.0.0.1:0")
		if err != nil {
			t.Fatal(err)
		}
		p := ln.Addr().(*net.TCPAddr).Port
		ln.Close()
		return p
	}
	// An SNI port and a BizHawk base whose BizHawkPorts don't include the
	// SNI port (else a BizHawk listener could answer on it).
	ports := func() (int, int) {
		for {
			sni, base := free(), free()
			if sni < base || sni >= base+BizHawkPorts {
				return sni, base
			}
		}
	}
	h := NewHub(ports())
	h.Start(true)
	waitFor := func(what string, f func(Status) bool) Status {
		for i := 0; i < 300; i++ {
			if st := h.Status(); f(st) {
				return st
			}
			time.Sleep(20 * time.Millisecond)
		}
		t.Fatal(what)
		return Status{}
	}
	waitFor("SNI listener", func(st Status) bool { return st.SNIPort != 0 })
	if st := h.Status(); st.BizHawkPort != 0 {
		t.Fatal("BizHawk port open with no game")
	}
	tg := &fakeTarget{mem: &fakeMem{d: map[string][]byte{}}}
	h.SetGame(&Game{Core: "gb", System: "GB"}, tg)
	st := waitFor("BizHawk listener", func(st Status) bool { return st.BizHawkPort != 0 })
	if st.BizHawkPort != h.bizhawkBase {
		t.Fatalf("BizHawk port %d, want %d", st.BizHawkPort, h.bizhawkBase)
	}
	for _, port := range []int{st.BizHawkPort, st.SNIPort} {
		c, err := net.DialTimeout("tcp4", fmt.Sprintf("127.0.0.1:%d", port), time.Second)
		if err != nil {
			t.Fatalf("port %d not reachable on loopback: %v", port, err)
		}
		c.Close()
		addrs, _ := net.InterfaceAddrs()
		for _, a := range addrs {
			ipn, ok := a.(*net.IPNet)
			if !ok || ipn.IP.IsLoopback() || ipn.IP.To4() == nil {
				continue
			}
			if c, err := net.DialTimeout("tcp4", fmt.Sprintf("%s:%d", ipn.IP, port), 300*time.Millisecond); err == nil {
				c.Close()
				t.Fatalf("port %d answers on %s", port, ipn.IP)
			}
		}
	}
	// the game goes: the BizHawk port closes a few seconds later
	h.ClearGame(tg)
	waitFor("BizHawk listener closing", func(st Status) bool { return st.BizHawkPort == 0 })
	if !Loopback(fmt.Sprintf("127.0.0.1:%d", h.sniPort)) || Loopback(fmt.Sprintf("0.0.0.0:%d", h.sniPort)) {
		t.Fatal("Loopback()")
	}

	// without sni, only the BizHawk ports are served
	h2 := NewHub(ports())
	h2.Start(false)
	h2.SetGame(&Game{Core: "gb", System: "GB"}, tg)
	waitFor2 := func() Status {
		for i := 0; i < 300; i++ {
			if st := h2.Status(); st.BizHawkPort != 0 {
				return st
			}
			time.Sleep(20 * time.Millisecond)
		}
		t.Fatal("BizHawk listener (no SNI)")
		return Status{}
	}
	if st := waitFor2(); st.SNIPort != 0 {
		t.Fatal("SNI served when it wasn't asked for")
	}
	if c, err := net.DialTimeout("tcp4", fmt.Sprintf("127.0.0.1:%d", h2.sniPort), 300*time.Millisecond); err == nil {
		c.Close()
		t.Fatal("SNI port answers")
	}
}
