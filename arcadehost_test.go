package main

import (
	"encoding/binary"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"retropixels/cores/arcade"
)

// hostTestBoard: a 4x2 board, shown turned 90 degrees, that paints its pixels
// from the buttons held, a DIP switch and a frame counter.
type hostTestBoard struct {
	screen []uint32
	pads   []arcade.Pad
	dips   map[string]int
	frames int
	sram   []byte
}

func (b *hostTestBoard) RunFrame() {
	b.frames++
	v := uint32(0)
	if len(b.pads) > 0 && b.pads[0]["Fire"] {
		v = 1
	}
	for i := range b.screen {
		b.screen[i] = uint32(i)<<16 | uint32(b.dips["lives"])<<8 | v
	}
	b.screen[0] = 0x123456
	b.sram[0] = byte(b.frames)
}
func (b *hostTestBoard) Screen() []uint32           { return b.screen }
func (b *hostTestBoard) SetInput(p []arcade.Pad)    { b.pads = p }
func (b *hostTestBoard) TakeAudio() []float32       { return []float32{0.5, -0.5, 0} }
func (b *hostTestBoard) Reset()                     { b.frames = 0 }
func (b *hostTestBoard) SaveState() ([]byte, error) { return []byte{byte(b.frames)}, nil }
func (b *hostTestBoard) LoadState(s []byte) error {
	if len(s) != 1 {
		return arcade.ErrBadState
	}
	b.frames = int(s[0])
	return nil
}
func (b *hostTestBoard) SRAM() []byte { return b.sram }

func init() {
	arcade.Register(&arcade.BoardDef{ID: "hosttest", Title: "Host test", Width: 4, Height: 2, Rotate: 90, FPS: 60,
		Buttons: []string{"Up", "Fire", "Coin", "Start (1P)", "Start (2P)"},
		Dips:    []arcade.Dip{{ID: "lives", Label: "Lives", Options: []arcade.DipOption{{Label: "3", Value: 3}, {Label: "5", Value: 5}}, Default: 3}},
		Games: []arcade.Game{{ID: "hosttest", Title: "Host Test", Year: 2026, Maker: "RetroPixels",
			Files: []arcade.File{{Name: "ht.1", Size: 4, Role: "cpu"}, {Name: "ht.pld", Size: 2, Optional: true}}}},
		New: func(g *arcade.Game, f arcade.Files, o arcade.Opts) (arcade.Board, error) {
			return &hostTestBoard{screen: make([]uint32, 8), dips: o.Dips, sram: make([]byte, 4)}, nil
		}})
}

func arcStartMsg(rate int, hdr string, save []byte, files map[string][]byte) []byte {
	b := []byte{arcMsgStart}
	b = binary.LittleEndian.AppendUint32(b, uint32(rate))
	b = binary.LittleEndian.AppendUint32(b, uint32(len(hdr)))
	b = append(b, hdr...)
	b = binary.LittleEndian.AppendUint32(b, uint32(len(save)))
	b = append(b, save...)
	for n, d := range files {
		b = binary.LittleEndian.AppendUint16(b, uint16(len(n)))
		b = append(b, n...)
		b = binary.LittleEndian.AppendUint32(b, uint32(len(d)))
		b = append(b, d...)
	}
	return b
}

func TestArcadeBoardsList(t *testing.T) {
	var hit *arcadeBoardJSON
	for _, b := range remoteArcadeBoards() {
		if webBoards[strings.TrimPrefix(b.ID, "go-")] {
			t.Fatalf("%s has a web board and shouldn't be listed", b.ID)
		}
		if b.ID == "go-hosttest" {
			hit = &b
		}
	}
	if hit == nil || len(hit.Games) != 1 {
		t.Fatal("the test board isn't listed")
	}
	g := hit.Games[0]
	// turned upright here, so the page sees a 2x4 horizontal game
	if g.Width != 2 || g.Height != 4 || g.Rotate != 0 || g.Aspect != 0.75 || len(g.Dips) != 1 || len(g.Files) != 2 || !g.Files[1].Optional {
		t.Fatalf("game: %+v", g)
	}
	if _, err := json.Marshal(hit); err != nil {
		t.Fatal(err)
	}
}

func TestArcadeSession(t *testing.T) {
	token = "test-token"
	srv := httptest.NewServer(newMux())
	defer srv.Close()
	addr := strings.TrimPrefix(srv.URL, "http://")
	hostOK[addr] = true
	defer delete(hostOK, addr)
	if c, status := wsDial(t, addr, "/api/arcade/ws?token=wrong", nil); c != nil || !strings.HasPrefix(status, "403") {
		t.Fatalf("wrong token: %s", status)
	}
	c, status := wsDial(t, addr, "/api/arcade/ws?token=test-token", map[string]string{"Origin": "http://127.0.0.1:47821"})
	if c == nil {
		t.Fatal(status)
	}
	c.frame(true, wsBinary, arcStartMsg(48000, `{"game":"hosttest","zip":"hosttest.zip","dips":{"lives":5}}`, nil,
		map[string][]byte{"ht.1": {1, 2, 3, 4}}))
	_, msg := c.read(t)
	if msg[0] != arcMsgReady {
		t.Fatalf("expected ready, got %x %q", msg[0], msg[1:])
	}
	var hello struct{ Session string }
	if json.Unmarshal(msg[1:], &hello) != nil || hello.Session == "" {
		t.Fatalf("hello %s", msg[1:])
	}
	if _, msg = c.read(t); msg[0] != arcMsgSave || len(msg) != 5 {
		t.Fatalf("expected the NVRAM, got %x", msg)
	}

	tick := []byte{arcMsgTick, 1, 2, 2, 0, 0, 0, 0, 0, 0, 0} // Fire held on controller 1
	c.frame(true, wsBinary, tick)
	_, msg = c.read(t)
	if msg[0] != arcMsgFrame || msg[1] != 1 {
		t.Fatalf("expected a frame, got %x", msg[:4])
	}
	w, h, n := binary.LittleEndian.Uint16(msg[4:]), binary.LittleEndian.Uint16(msg[6:]), binary.LittleEndian.Uint32(msg[8:])
	if w != 2 || h != 4 || n != 3 || len(msg) != 12+2*4*4+3*2 {
		t.Fatalf("frame %dx%d, %d samples, %d bytes", w, h, n, len(msg))
	}
	// The board's top-left pixel (0x123456) is at the top right once turned
	// 90 degrees clockwise, and comes as the page's 0xAABBGGRR.
	if p := binary.LittleEndian.Uint32(msg[12+1*4:]); p != 0xff563412 {
		t.Fatalf("top-right pixel %08x", p)
	}
	// Fire and the "5 lives" setting reached the board.
	if p := binary.LittleEndian.Uint32(msg[12+0*4:]); p&0xffff00 != 0x010500 { // page order: B=fire, G=lives
		t.Fatalf("pixel %08x: inputs or settings didn't arrive", p)
	}
	if s := int16(binary.LittleEndian.Uint16(msg[12+32:])); s < 16000 || s > 16400 {
		t.Fatalf("first sample %d", s)
	}

	// settings change, then a save state round trip
	c.frame(true, wsBinary, append([]byte{arcMsgDips}, `{"lives":3}`...))
	c.frame(true, wsBinary, tick)
	_, msg = c.read(t)
	if p := binary.LittleEndian.Uint32(msg[12:]); p&0xff00 != 0x0300 { // G=lives
		t.Fatalf("pixel %08x: the new setting didn't arrive", p)
	}
	get := func(method string, body io.Reader) (int, []byte) {
		req, _ := http.NewRequest(method, srv.URL+"/api/arcade/state?s="+hello.Session, body)
		req.Header.Set("X-Retro-Token", token)
		res, err := http.DefaultClient.Do(req)
		if err != nil {
			t.Fatal(err)
		}
		defer res.Body.Close()
		b, _ := io.ReadAll(res.Body)
		return res.StatusCode, b
	}
	code, st := get("GET", nil)
	if code != 200 || len(st) < 2 || st[len(st)-1] != 2 {
		t.Fatalf("state: %d %x", code, st)
	}
	if code, b := get("POST", strings.NewReader("xx")); code != 500 {
		t.Fatalf("a bad state was accepted: %d %s", code, b)
	}
	st[len(st)-1] = 7
	if code, b := get("POST", strings.NewReader(string(st))); code != 200 {
		t.Fatalf("load: %d %s", code, b)
	}
	if _, st = get("GET", nil); st[len(st)-1] != 7 {
		t.Fatalf("state after load: %x", st)
	}
}

func TestArcadeSessionBadSet(t *testing.T) {
	token = "test-token"
	srv := httptest.NewServer(newMux())
	defer srv.Close()
	c, status := wsDial(t, strings.TrimPrefix(srv.URL, "http://"), "/api/arcade/ws?token=test-token", nil)
	if c == nil {
		t.Fatal(status)
	}
	c.frame(true, wsBinary, arcStartMsg(48000, `{"game":"hosttest","zip":"hosttest.zip"}`, nil, map[string][]byte{"ht.1": {1, 2}}))
	if _, msg := c.read(t); msg[0] != arcMsgError || !strings.Contains(string(msg[1:]), "wrong size") {
		t.Fatalf("expected an error about the file, got %x %q", msg[0], msg[1:])
	}
}
