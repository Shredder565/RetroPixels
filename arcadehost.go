package main

// Arcade boards written only in Go (the ones web/emu has no board file for)
// play in RetroPixels.exe the way the Nintendo 64 does: they run here, and the
// page shows the pictures, plays the sound and sends the controls over a
// WebSocket. web/emu/arcade_remote.js is the page's side: it asks for the list
// of these boards (GET /api/arcade/boards) and adds them to RPArcade, so the
// page finds the games in zips exactly as it finds its own boards.
//
// The picture comes already turned upright, so the page sees every remote game
// as a horizontal one.
//
// Messages are binary; the first byte says what they are.
//
// Page -> RetroPixels:
//
//	0x01 start   u32 sample rate, u32 JSON length, JSON {game, zip, dips},
//	             u32 save length, save bytes, then the zip's files, each
//	             u16 name length, name, u32 length, bytes
//	0x02 tick    u8 frames to run, u8 controllers, then u32 buttons each
//	             (bit i = the game's button i)
//	0x03 reset
//	0x04 dips    JSON {id: value}
//
// RetroPixels -> page:
//
//	0x81 ready   JSON {session}
//	0x82 frame   u8 ticks answered (1), u8 0, u16 0, u16 width, u16 height,
//	             u32 sound samples, then width*height pixels (u32, the page's
//	             0xAABBGGRR) and the sound as mono int16 at the page's rate
//	0x83 save    the high-score / NVRAM memory (at the start and when it changes)
//	0x84 error   a message for the person
//
// Save states go through GET / POST /api/arcade/state?s=<session>.

import (
	"bytes"
	"crypto/rand"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"math"
	"net/http"
	"runtime/debug"
	"sync"
	"time"

	"retropixels/core"
	"retropixels/cores/arcade"
	_ "retropixels/cores/arcadeboards" // the Go-only arcade boards
)

const (
	arcMsgStart = 0x01
	arcMsgTick  = 0x02
	arcMsgReset = 0x03
	arcMsgDips  = 0x04

	arcMsgReady = 0x81
	arcMsgFrame = 0x82
	arcMsgSave  = 0x83
	arcMsgError = 0x84

	arcMaxMessage = 512 << 20 // the biggest arcade sets are a few hundred MB
)

// webBoards are the boards web/emu has its own files for; the page runs those
// itself.
var webBoards = map[string]bool{"pacman": true, "galaxian": true, "invaders": true, "dkong": true,
	"tmnt": true, "tmnt2": true, "simpsons": true}

// arcadeBoardJSON: one board for the page, in the shape RPArcade's board
// classes have (see the header of web/emu/arcade.js).
type arcadeBoardJSON struct {
	ID      string           `json:"id"`
	Title   string           `json:"title"`
	Width   int              `json:"width"`
	Height  int              `json:"height"`
	Rotate  int              `json:"rotate"`
	FPS     float64          `json:"fps"`
	Buttons []string         `json:"buttons"`
	DipsJS  []map[string]any `json:"dips"`
	Players int              `json:"players"`
	Games   []arcadeGameJSON `json:"games"`
	// the board reads the stick itself / aims with the mouse (ticks then
	// carry them, see arcTick)
	AnalogStick bool `json:"analogStick,omitempty"`
	MouseAim    bool `json:"mouseAim,omitempty"`
}

type arcadeFileJSON struct {
	Name     string   `json:"name"`
	Alt      []string `json:"alt,omitempty"`
	Size     int      `json:"size"`
	Optional bool     `json:"optional,omitempty"`
}

type arcadeGameJSON struct {
	ID         string           `json:"id"`
	Title      string           `json:"title"`
	Year       int              `json:"year,omitempty"`
	Maker      string           `json:"maker,omitempty"`
	Parent     string           `json:"parent,omitempty"`
	Files      []arcadeFileJSON `json:"files"`
	Loose      bool             `json:"loose,omitempty"`
	Width      int              `json:"width"`
	Height     int              `json:"height"`
	Rotate     int              `json:"rotate"`
	Aspect     float64          `json:"aspect"`
	FPS        float64          `json:"fps"`
	Dips       []map[string]any `json:"dips"`
	Buttons    []string         `json:"buttons"`
	Players    int              `json:"players"`
	Characters []string         `json:"characters,omitempty"`
}

func dipsJS(l []arcade.Dip) []map[string]any {
	out := []map[string]any{}
	for _, d := range l {
		var opts []map[string]any
		for _, o := range d.Options {
			opts = append(opts, map[string]any{"label": o.Label, "value": o.Value})
		}
		out = append(out, map[string]any{"id": d.ID, "label": d.Label, "options": opts, "default": d.Default})
	}
	return out
}

func or(a, b int) int {
	if a != 0 {
		return a
	}
	return b
}

// remoteArcadeBoards lists the Go-only boards for the page.
func remoteArcadeBoards() []arcadeBoardJSON {
	var out []arcadeBoardJSON
	for _, b := range arcade.Boards() {
		if webBoards[b.ID] {
			continue
		}
		bj := arcadeBoardJSON{ID: "go-" + b.ID, Title: b.Title, Width: b.Width, Height: b.Height, FPS: b.FPS,
			Buttons: b.Buttons, DipsJS: dipsJS(b.Dips), Players: or(b.Players, 2),
			AnalogStick: b.AnalogStick, MouseAim: b.MouseAim}
		if bj.Buttons == nil {
			bj.Buttons = arcade.DefaultButtons
		}
		for _, g := range b.Games {
			w, h := or(g.Width, b.Width), or(g.Height, b.Height)
			rot := b.Rotate
			if g.Rotate != 0 {
				rot = g.Rotate
			}
			rot = ((rot % 360) + 360) % 360
			a := g.Aspect
			if a == 0 {
				a = b.Aspect
			}
			if a == 0 {
				a = 4.0 / 3
			}
			if rot == 90 || rot == 270 {
				w, h, a = h, w, 1/a
			}
			fps := g.FPS
			if fps == 0 {
				fps = b.FPS
			}
			if fps == 0 {
				fps = 60
			}
			dips := g.Dips
			if dips == nil {
				dips = b.Dips
			}
			btn := g.Buttons
			if btn == nil {
				btn = bj.Buttons
			}
			gj := arcadeGameJSON{ID: g.ID, Title: g.Title, Year: g.Year, Maker: g.Maker, Parent: g.Parent, Loose: g.Loose,
				Width: w, Height: h, Rotate: 0, Aspect: a, FPS: fps, Dips: dipsJS(dips), Buttons: btn,
				Players: or(g.Players, bj.Players), Characters: g.Characters}
			if gj.Characters == nil {
				gj.Characters = b.Characters
			}
			for _, f := range g.Files {
				gj.Files = append(gj.Files, arcadeFileJSON{Name: f.Name, Alt: f.Alt, Size: f.Size, Optional: f.Optional})
			}
			bj.Games = append(bj.Games, gj)
		}
		out = append(out, bj)
	}
	return out
}

type arcTick struct {
	frames int
	pads   []uint32
	// boards with AnalogStick or MouseAim: player 1's stick (-1..1, up
	// negative), the mouse over the picture (-1..1, down positive, NaN when
	// off it) and its buttons (bit 0 left, bit 1 right)
	extra          bool
	stickX, stickY float32
	mouseX, mouseY float32
	mouseButtons   uint32
}

type arcReset struct{}

type arcDips map[string]int

type arcStateReq struct {
	load  []byte
	reply chan n64StateReply
}

type arcSession struct {
	id   string
	ws   *wsConn
	cmd  chan any
	quit chan struct{}
	done chan struct{}

	// only used on the emulator goroutine
	c        *arcade.Core
	rate     int
	lastSave []byte
	frameNo  int
	sound    []float32
	out      []byte
}

var (
	arcMu       sync.Mutex
	arcSessions = map[string]*arcSession{}
)

func registerArcadeRoutes(mux *http.ServeMux) {
	mux.HandleFunc("/api/arcade/boards", guard(func(w http.ResponseWriter, r *http.Request) {
		writeJSON(w, 200, remoteArcadeBoards())
	}))

	mux.HandleFunc("/api/arcade/ws", func(w http.ResponseWriter, r *http.Request) {
		// Browsers can't add headers to a WebSocket, so the token comes in the address.
		if !hostOK[r.Host] || token == "" || r.URL.Query().Get("token") != token {
			http.Error(w, "forbidden", http.StatusForbidden)
			return
		}
		if o := r.Header.Get("Origin"); o != "" && o != "http://"+r.Host {
			http.Error(w, "forbidden", http.StatusForbidden)
			return
		}
		ws, err := wsUpgrade(w, r, arcMaxMessage)
		if err != nil {
			return
		}
		serveArcade(ws)
	})

	mux.HandleFunc("/api/arcade/state", guard(func(w http.ResponseWriter, r *http.Request) {
		const gone = "The arcade game isn't running any more."
		arcMu.Lock()
		s := arcSessions[r.URL.Query().Get("s")]
		arcMu.Unlock()
		if s == nil {
			http.Error(w, gone, http.StatusNotFound)
			return
		}
		req := arcStateReq{reply: make(chan n64StateReply, 1)}
		if r.Method == http.MethodPost {
			b, err := io.ReadAll(io.LimitReader(r.Body, arcMaxMessage))
			if err != nil || len(b) == 0 {
				http.Error(w, "The save state didn't arrive.", http.StatusBadRequest)
				return
			}
			req.load = b
		}
		select {
		case s.cmd <- req:
		case <-s.done:
			http.Error(w, gone, http.StatusNotFound)
			return
		}
		var rep n64StateReply
		select {
		case rep = <-req.reply:
		case <-s.done:
			rep.err = errors.New("the arcade game stopped")
		case <-time.After(20 * time.Second):
			rep.err = errors.New("the arcade emulator didn't answer")
		}
		if rep.err != nil {
			http.Error(w, rep.err.Error(), http.StatusInternalServerError)
			return
		}
		w.Header().Set("Content-Type", "application/octet-stream")
		w.Header().Set("Cache-Control", "no-store")
		_, _ = w.Write(rep.data)
	}))
}

// arcStart is what the start message holds.
type arcStart struct {
	rate  int
	game  string
	zip   string
	dips  map[string]int
	save  []byte
	names []string
	data  [][]byte
}

func parseArcStart(msg []byte) (st arcStart, err error) {
	bad := errors.New("the game didn't arrive")
	if len(msg) < 9 || msg[0] != arcMsgStart {
		return st, bad
	}
	st.rate = int(binary.LittleEndian.Uint32(msg[1:]))
	jl := int(binary.LittleEndian.Uint32(msg[5:]))
	p := msg[9:]
	if st.rate < 8000 || st.rate > 384000 || jl > len(p) {
		return st, bad
	}
	var hdr struct {
		Game string         `json:"game"`
		Zip  string         `json:"zip"`
		Dips map[string]int `json:"dips"`
	}
	if json.Unmarshal(p[:jl], &hdr) != nil {
		return st, bad
	}
	st.game, st.zip, st.dips = hdr.Game, hdr.Zip, hdr.Dips
	p = p[jl:]
	if len(p) < 4 {
		return st, bad
	}
	sl := int(binary.LittleEndian.Uint32(p))
	p = p[4:]
	if sl > len(p) {
		return st, bad
	}
	st.save = p[:sl]
	p = p[sl:]
	for len(p) > 0 {
		if len(p) < 2 {
			return st, bad
		}
		nl := int(binary.LittleEndian.Uint16(p))
		p = p[2:]
		if nl+4 > len(p) {
			return st, bad
		}
		name := string(p[:nl])
		dl := int(binary.LittleEndian.Uint32(p[nl:]))
		p = p[nl+4:]
		if dl > len(p) {
			return st, bad
		}
		st.names = append(st.names, name)
		st.data = append(st.data, p[:dl])
		p = p[dl:]
	}
	return st, nil
}

// serveArcade runs one page connection: one board, freed when the page goes.
func serveArcade(ws *wsConn) {
	defer ws.Close()
	op, msg, err := ws.ReadMessage()
	if err != nil {
		return
	}
	st, err := parseArcStart(msg)
	if op != wsBinary || err != nil {
		sendArcError(ws, "The game didn't arrive. Try loading it again.")
		return
	}
	idb := make([]byte, 12)
	_, _ = rand.Read(idb)
	s := &arcSession{id: hex.EncodeToString(idb), ws: ws, rate: st.rate,
		cmd: make(chan any, 32), quit: make(chan struct{}), done: make(chan struct{})}
	arcMu.Lock()
	arcSessions[s.id] = s
	arcMu.Unlock()
	go s.run(st)

	defer func() {
		close(s.quit)
		<-s.done
		arcMu.Lock()
		delete(arcSessions, s.id)
		arcMu.Unlock()
	}()
	for {
		op, msg, err := ws.ReadMessage()
		if err != nil {
			return
		}
		if op != wsBinary || len(msg) == 0 {
			continue
		}
		var c any
		switch msg[0] {
		case arcMsgTick:
			if len(msg) >= 3 {
				t := arcTick{frames: max(1, int(msg[1]))}
				n := int(msg[2])
				for i := 0; i < n && 3+i*4+4 <= len(msg); i++ {
					t.pads = append(t.pads, binary.LittleEndian.Uint32(msg[3+i*4:]))
				}
				if x := msg[min(len(msg), 3+n*4):]; len(x) >= 20 {
					f := func(o int) float32 { return math.Float32frombits(binary.LittleEndian.Uint32(x[o:])) }
					t.extra = true
					t.stickX, t.stickY, t.mouseX, t.mouseY = f(0), f(4), f(8), f(12)
					t.mouseButtons = binary.LittleEndian.Uint32(x[16:])
				}
				c = t
			}
		case arcMsgReset:
			c = arcReset{}
		case arcMsgDips:
			var d map[string]int
			if json.Unmarshal(msg[1:], &d) == nil {
				c = arcDips(d)
			}
		}
		if c == nil {
			continue
		}
		select {
		case s.cmd <- c:
		case <-s.done:
			return
		}
	}
}

func sendArcError(ws *wsConn, text string) {
	_ = ws.WriteBinary(append([]byte{arcMsgError}, text...))
}

// safely runs f, turning an emulator crash into a message for the page.
func (s *arcSession) safely(f func()) (ok bool) {
	defer func() {
		if e := recover(); e != nil {
			log.Printf("arcade: emulator stopped: %v\n%s", e, debug.Stack())
			s.c = nil
			sendArcError(s.ws, fmt.Sprintf("The arcade emulator ran into a problem with this game and stopped (%v).", e))
			ok = false
		}
	}()
	f()
	return true
}

func (s *arcSession) run(st arcStart) {
	defer close(s.done)
	defer func() { s.c = nil }()
	sizes := make([]int, len(st.data))
	for i, d := range st.data {
		sizes[i] = len(d)
	}
	var err error
	if !s.safely(func() {
		s.c, err = arcade.NewFromFiles(st.names, sizes, st.data, st.zip,
			core.Options{SampleRate: st.rate, Dips: st.dips, SRAM: st.save})
	}) {
		return
	}
	if err != nil {
		sendArcError(s.ws, err.Error())
		return
	}
	if s.c.GameID() != st.game {
		// The page and this side picked different games from the same zip:
		// shouldn't happen, but say so rather than play the wrong one.
		sendArcError(s.ws, fmt.Sprintf("RetroPixels found %s in this zip, not %s.", s.c.GameTitle(), st.game))
		return
	}
	hello, _ := json.Marshal(map[string]any{"session": s.id})
	_ = s.ws.WriteBinary(append([]byte{arcMsgReady}, hello...))
	s.sendSave(true)

	for {
		var c any
		select {
		case c = <-s.cmd:
		case <-s.quit:
			return
		}
		switch c := c.(type) {
		case arcTick:
			s.tick(c)
			s.sendFrame(1)
		case arcReset:
			if s.c != nil {
				s.safely(func() { s.c.Reset() })
			}
		case arcDips:
			if s.c != nil {
				for id, v := range c {
					s.c.SetDip(id, v)
				}
			}
		case arcStateReq:
			c.reply <- s.state(c.load)
		}
	}
}

func (s *arcSession) tick(t arcTick) {
	if s.c == nil {
		return
	}
	s.safely(func() {
		for p := 0; p < 4; p++ {
			var b uint32
			if p < len(t.pads) {
				b = t.pads[p]
			}
			s.c.SetInput(p, b)
		}
		if t.extra {
			s.c.SetAnalog(0, 0, t.stickX)
			s.c.SetAnalog(0, 1, t.stickY)
			s.c.SetMouse(t.mouseX, t.mouseY, t.mouseButtons)
		}
		for i := 0; i < t.frames; i++ {
			s.c.RunFrame()
			s.sound = append(s.sound, s.c.TakeAudio()...)
			if s.frameNo++; s.frameNo%30 == 0 {
				s.sendSave(false)
			}
		}
	})
}

func (s *arcSession) sendFrame(ticks int) {
	w, h := 0, 0
	var pix []uint32
	if s.c != nil {
		w, h = s.c.Width(), s.c.Height()
		pix = s.c.Screen()
	}
	if len(pix) < w*h {
		w, h, pix = 0, 0, nil
	}
	size := 12 + w*h*4 + len(s.sound)*2
	if cap(s.out) < size {
		s.out = make([]byte, size)
	}
	b := s.out[:size]
	b[0], b[1], b[2], b[3] = arcMsgFrame, byte(ticks), 0, 0
	binary.LittleEndian.PutUint16(b[4:], uint16(w))
	binary.LittleEndian.PutUint16(b[6:], uint16(h))
	binary.LittleEndian.PutUint32(b[8:], uint32(len(s.sound)))
	p := b[12:]
	for i, c := range pix[:w*h] {
		// 0x00RRGGBB -> the page's 0xAABBGGRR
		binary.LittleEndian.PutUint32(p[i*4:], 0xff000000|(c&0xff)<<16|(c&0xff00)|(c>>16)&0xff)
	}
	a := p[w*h*4:]
	for i, v := range s.sound {
		x := math.Round(float64(v) * 32767)
		x = math.Max(-32768, math.Min(32767, x))
		binary.LittleEndian.PutUint16(a[i*2:], uint16(int16(x)))
	}
	s.sound = s.sound[:0]
	_ = s.ws.WriteBinary(b)
}

// sendSave sends the board's NVRAM when it changed (always, when first).
func (s *arcSession) sendSave(first bool) {
	if s.c == nil {
		return
	}
	b := s.c.SRAM()
	if b == nil {
		return
	}
	if !first && bytes.Equal(b, s.lastSave) {
		return
	}
	s.lastSave = append(s.lastSave[:0], b...)
	_ = s.ws.WriteBinary(append([]byte{arcMsgSave}, b...))
}

func (s *arcSession) state(load []byte) (rep n64StateReply) {
	if s.c == nil {
		return n64StateReply{err: errors.New("the arcade emulator has stopped")}
	}
	s.safely(func() {
		if load != nil {
			rep.err = s.c.LoadState(load)
			s.sound = s.sound[:0]
			return
		}
		rep.data, rep.err = s.c.SaveState()
	})
	if s.c == nil && rep.err == nil && rep.data == nil {
		rep.err = errors.New("the arcade emulator has stopped")
	}
	return rep
}
