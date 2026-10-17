package main

// Wii U sessions: like the Nintendo 64, the Wii U runs here (RetroPixels' own
// Wii U emulator, package wiiu, through its native-app core in cores/wiiu) and
// the page only shows the picture, plays the sound and sends the GamePad,
// over a WebSocket.
//
// Messages are binary; the first byte says what they are.
//
// Page -> RetroPixels:
//
//	0x01 start   u32 sample rate, u32 name length, the file's name, then the .rpx
//	0x02 tick    u8 frames to run, u32 buttons (bit i = cores/wiiu Buttons[i]),
//	             4 x i8 sticks (left X, left Y, right X, right Y; down is +)
//	0x03 reset
//
// RetroPixels -> page:
//
//	0x81 ready   JSON {session, fps}
//	0x82 frame   u8 ticks answered (1), u8 picture kind, u8 rumble (0-255),
//	             u8 0, u16 width, u16 height, u32 sound frames, then the
//	             picture, then the sound as stereo int16 at the page's sample
//	             rate. Picture kinds: 0 = all of it (width*height ABGR pixels,
//	             little-endian u32), 1 = the same as before (nothing sent),
//	             2 = some rows changed (u16 first row, u16 rows, then those
//	             rows' pixels)
//	0x84 error   a message for the person
//
// A 1280x720 picture is 3.6 MB, so only the rows that changed since the last
// picture are sent (homebrew often changes a few lines of text a frame).

import (
	"crypto/rand"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"log"
	"math"
	"net/http"
	"runtime/debug"
	"slices"

	"retropixels/core"
	wiiucore "retropixels/cores/wiiu"
)

const (
	wiiuMsgStart = 0x01
	wiiuMsgTick  = 0x02
	wiiuMsgReset = 0x03

	wiiuMsgReady = 0x81
	wiiuMsgFrame = 0x82
	wiiuMsgError = 0x84

	wiiuMaxMessage = 80 << 20 // .rpx files are up to a few tens of MB
)

type wiiuTick struct {
	frames  int
	buttons uint32
	sticks  [4]float32
}

type wiiuReset struct{}

type wiiuSession struct {
	ws   *wsConn
	cmd  chan any
	quit chan struct{}
	done chan struct{}

	// only used on the emulator goroutine
	c      core.Core
	rate   int
	failed string
	last   []uint32 // the last picture sent
	lw, lh int
	sound  []float32
	out    []byte
}

func registerWiiURoutes(mux *http.ServeMux) {
	mux.HandleFunc("/api/wiiu/ws", func(w http.ResponseWriter, r *http.Request) {
		// Browsers can't add headers to a WebSocket, so the token comes in the address.
		if !hostOK[r.Host] || token == "" || r.URL.Query().Get("token") != token {
			http.Error(w, "forbidden", http.StatusForbidden)
			return
		}
		if o := r.Header.Get("Origin"); o != "" && o != "http://"+r.Host {
			http.Error(w, "forbidden", http.StatusForbidden)
			return
		}
		ws, err := wsUpgrade(w, r, wiiuMaxMessage)
		if err != nil {
			return
		}
		serveWiiU(ws)
	})
}

func serveWiiU(ws *wsConn) {
	defer ws.Close()
	op, msg, err := ws.ReadMessage()
	if err != nil {
		return
	}
	bad := func() { sendWiiUError(ws, "The program didn't arrive. Try loading it again.") }
	if op != wsBinary || len(msg) < 9 || msg[0] != wiiuMsgStart {
		bad()
		return
	}
	rate := int(binary.LittleEndian.Uint32(msg[1:]))
	nl := int(binary.LittleEndian.Uint32(msg[5:]))
	if rate < 8000 || rate > 384000 || nl > 1024 || 9+nl > len(msg) {
		bad()
		return
	}
	name := string(msg[9 : 9+nl])
	rpx := msg[9+nl:]

	s := &wiiuSession{ws: ws, rate: rate, cmd: make(chan any, 32), quit: make(chan struct{}), done: make(chan struct{})}
	go s.run(rpx, name)
	defer func() {
		close(s.quit)
		<-s.done
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
		case wiiuMsgTick:
			if t, ok := parseWiiUTick(msg); ok {
				c = t
			}
		case wiiuMsgReset:
			c = wiiuReset{}
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

func parseWiiUTick(msg []byte) (wiiuTick, bool) {
	if len(msg) < 10 {
		return wiiuTick{}, false
	}
	t := wiiuTick{frames: int(msg[1]), buttons: binary.LittleEndian.Uint32(msg[2:])}
	if t.frames < 1 {
		t.frames = 1
	}
	if t.frames > 4 {
		t.frames = 4
	}
	for i := range t.sticks {
		t.sticks[i] = float32(int8(msg[6+i])) / 127
	}
	return t, true
}

func sendWiiUError(ws *wsConn, text string) {
	_ = ws.WriteBinary(append([]byte{wiiuMsgError}, text...))
}

func (s *wiiuSession) safely(f func()) (ok bool) {
	defer func() {
		if e := recover(); e != nil {
			log.Printf("wiiu: emulator stopped: %v\n%s", e, debug.Stack())
			s.failed = fmt.Sprint(e)
			s.c = nil
			sendWiiUError(s.ws, "The Wii U emulator ran into a problem with this program and stopped ("+s.failed+").")
			ok = false
		}
	}()
	f()
	return true
}

func (s *wiiuSession) run(rpx []byte, name string) {
	defer close(s.done)
	defer func() { s.c = nil }()
	var err error
	if !s.safely(func() { s.c, err = wiiucore.New(rpx, core.Options{SampleRate: s.rate, Name: name}) }) {
		return
	}
	if err != nil {
		sendWiiUError(s.ws, "That doesn't look like a Wii U program ("+err.Error()+").")
		return
	}
	hello, _ := json.Marshal(map[string]any{"session": randomID(), "fps": s.c.FPS()})
	_ = s.ws.WriteBinary(append([]byte{wiiuMsgReady}, hello...))
	for {
		var c any
		select {
		case c = <-s.cmd:
		case <-s.quit:
			return
		}
		switch c := c.(type) {
		case wiiuTick:
			s.tick(c)
			s.sendFrame()
		case wiiuReset:
			if s.c != nil {
				s.safely(func() { s.c.Reset() })
				s.last = nil
			}
		}
	}
}

func randomID() string {
	b := make([]byte, 12)
	_, _ = rand.Read(b)
	return hex.EncodeToString(b)
}

func (s *wiiuSession) tick(t wiiuTick) {
	if s.c == nil {
		return
	}
	s.safely(func() {
		s.c.SetInput(0, t.buttons)
		if a, ok := s.c.(core.Analog); ok {
			for i, v := range t.sticks {
				a.SetAnalog(0, i, v)
			}
		}
		for i := 0; i < t.frames; i++ {
			s.c.RunFrame()
			s.sound = append(s.sound, s.c.TakeAudio()...)
		}
	})
}

func (s *wiiuSession) sendFrame() {
	w, h := 1280, 720
	var pix []uint32
	var rumble byte
	if s.c != nil {
		w, h = s.c.Width(), s.c.Height()
		pix = s.c.Screen()
		if r, ok := s.c.(interface{ Rumble() float32 }); ok {
			rumble = byte(math.Round(float64(min(1, max(0, r.Rumble()))) * 255))
		}
	}
	if pix == nil {
		pix = make([]uint32, w*h)
	}
	// the rows that changed
	kind, y0, y1 := byte(0), 0, h
	if w == s.lw && h == s.lh && len(s.last) == w*h {
		for y0 < h && slices.Equal(pix[y0*w:y0*w+w], s.last[y0*w:y0*w+w]) {
			y0++
		}
		if y0 == h {
			kind, y0, y1 = 1, 0, 0
		} else {
			for slices.Equal(pix[(y1-1)*w:y1*w], s.last[(y1-1)*w:y1*w]) {
				y1--
			}
			kind = 2
		}
	}
	n := len(s.sound) / 2
	size := 12 + (y1-y0)*w*4 + n*4
	if kind == 2 {
		size += 4
	}
	if cap(s.out) < size {
		s.out = make([]byte, size)
	}
	b := s.out[:size]
	b[0], b[1], b[2], b[3] = wiiuMsgFrame, 1, kind, rumble
	binary.LittleEndian.PutUint16(b[4:], uint16(w))
	binary.LittleEndian.PutUint16(b[6:], uint16(h))
	binary.LittleEndian.PutUint32(b[8:], uint32(n))
	p := b[12:]
	if kind == 2 {
		binary.LittleEndian.PutUint16(p, uint16(y0))
		binary.LittleEndian.PutUint16(p[2:], uint16(y1-y0))
		p = p[4:]
	}
	for i, c := range pix[y0*w : y1*w] {
		// 0x00RRGGBB -> ABGR
		binary.LittleEndian.PutUint32(p[i*4:], 0xff000000|c>>16&0xff|c&0xff00|(c&0xff)<<16)
	}
	p = p[(y1-y0)*w*4:]
	if kind != 1 {
		s.last = append(s.last[:0], pix...)
		s.lw, s.lh = w, h
	}
	for i, v := range s.sound[:n*2] {
		binary.LittleEndian.PutUint16(p[i*2:], uint16(int16(math.Round(float64(max(-1, min(1, v)))*32767))))
	}
	s.sound = s.sound[:0]
	_ = s.ws.WriteBinary(b)
}
