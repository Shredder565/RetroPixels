package main

// Nintendo 64 sessions: the N64 is too heavy to run inside the page, so it
// runs here (package n64) and the page only shows the picture, plays the sound
// and sends the controllers, over a WebSocket.
//
// Messages are binary; the first byte says what they are.
//
// Page -> RetroPixels:
//
//	0x01 start   u32 sample rate, u32 save length, save bytes, then the ROM
//	0x02 tick    u8 frames to run, then 4 controllers of
//	             u16 buttons (N64 bits), i8 stick X, i8 stick Y, u8 connected
//	0x03 reset
//
// RetroPixels -> page:
//
//	0x81 ready   JSON {session, fps, title, id}
//	0x82 frame   u8 ticks answered (1), u8 0, u16 0, u16 width, u16 height,
//	             u32 sound frames, then width*height ABGR pixels (little-endian
//	             u32) and the sound as stereo int16 at the page's sample rate
//	0x83 save    the battery save (sent at the start and whenever it changes)
//	0x84 error   a message for the person
//
// Save states go through GET / POST /api/n64/state?s=<session> instead, so the
// page can ask for them synchronously.

import (
	"bytes"
	"compress/flate"
	"crypto/rand"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"net/http"
	"runtime/debug"
	"sync"
	"time"

	"retropixels/n64"
)

const (
	n64MsgStart = 0x01
	n64MsgTick  = 0x02
	n64MsgReset = 0x03

	n64MsgReady = 0x81
	n64MsgFrame = 0x82
	n64MsgSave  = 0x83
	n64MsgError = 0x84

	n64MaxMessage = 160 << 20 // the biggest ROMs are 64 MB
)

type n64Tick struct {
	frames int
	pads   [4]n64.PadState
}

type n64Reset struct{}

// n64StateReq asks the session for a save state (load == nil) or to load one.
type n64StateReq struct {
	load  []byte
	reply chan n64StateReply
}

type n64StateReply struct {
	data []byte
	err  error
}

type n64Session struct {
	id   string
	ws   *wsConn
	cmd  chan any
	quit chan struct{} // closed when the page goes away
	done chan struct{} // closed when the emulator goroutine has stopped

	// only used on the emulator goroutine
	m          *n64.Machine
	rate       int // the page's sample rate
	failed     string
	rs         n64Resampler
	soundAhead float64 // sound sent minus sound due, in samples at the page's rate
	frameNo    int
	audioOut   []int16
	out        []byte
}

var (
	n64Mu       sync.Mutex
	n64Sessions = map[string]*n64Session{}
)

func registerN64Routes(mux *http.ServeMux) {
	mux.HandleFunc("/api/n64/ws", func(w http.ResponseWriter, r *http.Request) {
		// Browsers can't add headers to a WebSocket, so the token comes in the address.
		if !hostOK[r.Host] || token == "" || r.URL.Query().Get("token") != token {
			http.Error(w, "forbidden", http.StatusForbidden)
			return
		}
		if o := r.Header.Get("Origin"); o != "" && o != "http://"+r.Host {
			http.Error(w, "forbidden", http.StatusForbidden)
			return
		}
		ws, err := wsUpgrade(w, r, n64MaxMessage)
		if err != nil {
			return
		}
		serveN64(ws)
	})

	mux.HandleFunc("/api/n64/state", guard(func(w http.ResponseWriter, r *http.Request) {
		n64Mu.Lock()
		s := n64Sessions[r.URL.Query().Get("s")]
		n64Mu.Unlock()
		if s == nil {
			http.Error(w, "The Nintendo 64 game isn't running any more.", http.StatusNotFound)
			return
		}
		req := n64StateReq{reply: make(chan n64StateReply, 1)}
		if r.Method == http.MethodPost {
			b, err := io.ReadAll(io.LimitReader(r.Body, n64MaxMessage))
			if err != nil || len(b) == 0 {
				http.Error(w, "The save state didn't arrive.", http.StatusBadRequest)
				return
			}
			req.load = b
		}
		select {
		case s.cmd <- req:
		case <-s.done:
			http.Error(w, "The Nintendo 64 game isn't running any more.", http.StatusNotFound)
			return
		}
		var rep n64StateReply
		select {
		case rep = <-req.reply:
		case <-s.done:
			rep.err = errors.New("the Nintendo 64 game stopped")
		case <-time.After(20 * time.Second):
			rep.err = errors.New("the Nintendo 64 emulator didn't answer")
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

// serveN64 runs one page connection: one machine, freed when the page goes.
func serveN64(ws *wsConn) {
	defer ws.Close()
	op, msg, err := ws.ReadMessage()
	if err != nil {
		return
	}
	if op != wsBinary || len(msg) < 9 || msg[0] != n64MsgStart {
		sendN64Error(ws, "The game didn't arrive. Try loading it again.")
		return
	}
	rate := int(binary.LittleEndian.Uint32(msg[1:]))
	sl := int(binary.LittleEndian.Uint32(msg[5:]))
	if rate < 8000 || rate > 384000 || 9+sl > len(msg) {
		sendN64Error(ws, "The game didn't arrive. Try loading it again.")
		return
	}
	save := msg[9 : 9+sl]
	rom := msg[9+sl:]

	idb := make([]byte, 12)
	_, _ = rand.Read(idb)
	s := &n64Session{id: hex.EncodeToString(idb), ws: ws, rate: rate,
		cmd: make(chan any, 32), quit: make(chan struct{}), done: make(chan struct{})}
	n64Mu.Lock()
	n64Sessions[s.id] = s
	n64Mu.Unlock()
	go s.run(rom, save)

	defer func() {
		close(s.quit)
		<-s.done
		n64Mu.Lock()
		delete(n64Sessions, s.id)
		n64Mu.Unlock()
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
		case n64MsgTick:
			c = parseN64Tick(msg)
		case n64MsgReset:
			c = n64Reset{}
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

func parseN64Tick(msg []byte) any {
	if len(msg) < 2+4*5 {
		return nil
	}
	t := n64Tick{frames: int(msg[1])}
	if t.frames < 1 {
		t.frames = 1
	}
	for i := range t.pads {
		p := msg[2+i*5:]
		t.pads[i] = n64.PadState{
			Buttons: binary.LittleEndian.Uint16(p),
			X:       int8(p[2]),
			Y:       int8(p[3]),
			Present: i == 0 || p[4]&1 != 0,
			Pak:     true,
		}
	}
	return t
}

func sendN64Error(ws *wsConn, text string) {
	_ = ws.WriteBinary(append([]byte{n64MsgError}, text...))
}

// safely runs f, turning an emulator crash into a message for the page.
func (s *n64Session) safely(f func()) (ok bool) {
	defer func() {
		if e := recover(); e != nil {
			log.Printf("n64: emulator stopped: %v\n%s", e, debug.Stack())
			s.failed = fmt.Sprint(e)
			s.m = nil
			sendN64Error(s.ws, "The Nintendo 64 emulator ran into a problem with this game and stopped ("+s.failed+").")
			ok = false
		}
	}()
	f()
	return true
}

// run is the session's emulator goroutine.
func (s *n64Session) run(rom, save []byte) {
	defer close(s.done)
	defer func() { s.m = nil }() // the machine is freed with the session
	var err error
	if !s.safely(func() { s.m, err = n64.New(rom) }) {
		return
	}
	if err != nil {
		sendN64Error(s.ws, "That doesn't look like a Nintendo 64 game ("+err.Error()+").")
		return
	}
	if !s.safely(func() {
		if len(save) > 0 {
			s.m.Save.Import(save)
		}
		s.m.SaveChanged()
	}) {
		return
	}
	hello, _ := json.Marshal(map[string]any{"session": s.id, "fps": s.m.FPS(), "title": s.m.Title(), "id": s.m.GameID()})
	_ = s.ws.WriteBinary(append([]byte{n64MsgReady}, hello...))
	s.sendSave()

	for {
		var c any
		select {
		case c = <-s.cmd:
		case <-s.quit:
			return
		}
		switch c := c.(type) {
		case n64Tick:
			// One picture per tick, so the page always knows how far along the emulator is.
			s.tick(c)
			s.sendFrame(1)
		case n64Reset:
			if s.m != nil {
				s.safely(func() { s.m.Reset() })
			}
		case n64StateReq:
			c.reply <- s.state(c.load)
		}
	}
}

// tick runs frames with the controllers as given.
func (s *n64Session) tick(t n64Tick) {
	if s.m == nil {
		return
	}
	s.safely(func() {
		s.m.Pad = t.pads
		for i := 0; i < t.frames; i++ {
			s.m.RunFrame()
			s.takeAudio()
			if s.frameNo++; s.frameNo%30 == 0 && s.m.SaveChanged() {
				s.sendSave()
			}
		}
	})
}

// takeAudio moves the game's sound since the last frame to audioOut,
// converted to the page's sample rate.
func (s *n64Session) takeAudio() {
	m := s.m
	before := len(s.audioOut)
	if len(m.Audio) >= 2 && m.AudioRate > 0 {
		s.audioOut = s.rs.run(m.Audio, m.AudioRate, s.rate, s.audioOut)
	}
	m.Audio = m.Audio[:0]
	// Games hand over sound in pieces that don't line up with frames. Keep
	// count of how far the sound is ahead of or behind the picture, and when
	// the game plays nothing for a while, send silence so the page's sound
	// clock (which sets the game's speed) keeps the right pace.
	per := float64(s.rate) / m.FPS()
	s.soundAhead += float64(len(s.audioOut)-before)/2 - per
	if s.soundAhead > 30*per {
		s.soundAhead = 30 * per
	}
	if s.soundAhead < -3*per {
		n := int(-s.soundAhead - per)
		for i := 0; i < n; i++ {
			s.audioOut = append(s.audioOut, 0, 0)
		}
		s.soundAhead += float64(n)
	}
}

func (s *n64Session) sendFrame(ticks int) {
	w, h := 320, 240
	var pix []uint32
	if s.m != nil && s.m.FrameW > 0 && s.m.FrameH > 0 && len(s.m.Frame) >= s.m.FrameW*s.m.FrameH {
		w, h = s.m.FrameW, s.m.FrameH
		pix = s.m.Frame[:w*h]
	}
	sound := len(s.audioOut) / 2
	size := 12 + w*h*4 + len(s.audioOut)*2
	if cap(s.out) < size {
		s.out = make([]byte, size)
	}
	b := s.out[:size]
	b[0], b[1], b[2], b[3] = n64MsgFrame, byte(ticks), 0, 0
	binary.LittleEndian.PutUint16(b[4:], uint16(w))
	binary.LittleEndian.PutUint16(b[6:], uint16(h))
	binary.LittleEndian.PutUint32(b[8:], uint32(sound))
	p := b[12:]
	if pix == nil {
		for i := 0; i < w*h; i++ {
			binary.LittleEndian.PutUint32(p[i*4:], 0xff000000)
		}
	} else {
		for i, c := range pix {
			binary.LittleEndian.PutUint32(p[i*4:], c)
		}
	}
	a := p[w*h*4:]
	for i, v := range s.audioOut {
		binary.LittleEndian.PutUint16(a[i*2:], uint16(v))
	}
	s.audioOut = s.audioOut[:0]
	_ = s.ws.WriteBinary(b)
}

func (s *n64Session) sendSave() {
	if s.m == nil {
		return
	}
	_ = s.ws.WriteBinary(append([]byte{n64MsgSave}, s.m.Save.Export()...))
}

// Save states are kept compressed: "RPN1" + deflate(machine state).
func (s *n64Session) state(load []byte) (rep n64StateReply) {
	if s.m == nil {
		return n64StateReply{err: errors.New("the Nintendo 64 emulator has stopped")}
	}
	ok := s.safely(func() {
		if load == nil {
			raw := s.m.SaveState()
			if len(raw) == 0 {
				rep.err = errors.New("save states aren't available for Nintendo 64 yet")
				return
			}
			var buf bytes.Buffer
			buf.WriteString("RPN1")
			zw, _ := flate.NewWriter(&buf, flate.BestSpeed)
			_, _ = zw.Write(raw)
			_ = zw.Close()
			rep.data = buf.Bytes()
			return
		}
		raw := load
		if len(load) >= 4 && string(load[:4]) == "RPN1" {
			var err error
			raw, err = io.ReadAll(io.LimitReader(flate.NewReader(bytes.NewReader(load[4:])), 1<<30))
			if err != nil {
				rep.err = errors.New("that save state is damaged")
				return
			}
		}
		if err := s.m.LoadState(raw); err != nil {
			rep.err = fmt.Errorf("couldn't load that save state: %w", err)
			return
		}
		s.m.Audio = s.m.Audio[:0]
		s.audioOut = s.audioOut[:0]
	})
	if !ok {
		return n64StateReply{err: errors.New("the Nintendo 64 emulator stopped: " + s.failed)}
	}
	return rep
}

// n64Resampler converts stereo int16 sound between sample rates by straight-
// line interpolation, carrying its position from one piece to the next.
type n64Resampler struct {
	pos          float64 // where the next output sample is; 0 = the last input sample
	prevL, prevR float64
}

func (r *n64Resampler) run(in []int16, inRate, outRate int, out []int16) []int16 {
	n := len(in) / 2
	if n == 0 {
		return out
	}
	step := float64(inRate) / float64(outRate)
	at := func(i int) (float64, float64) { // i = 0 is the previous piece's last sample
		if i == 0 {
			return r.prevL, r.prevR
		}
		return float64(in[2*i-2]), float64(in[2*i-1])
	}
	pos := r.pos
	for pos < float64(n) {
		i := int(pos)
		f := pos - float64(i)
		l0, r0 := at(i)
		l1, r1 := at(i + 1)
		out = append(out, int16(l0+(l1-l0)*f), int16(r0+(r1-r0)*f))
		pos += step
	}
	r.pos = pos - float64(n)
	r.prevL, r.prevR = float64(in[2*n-2]), float64(in[2*n-1])
	return out
}
