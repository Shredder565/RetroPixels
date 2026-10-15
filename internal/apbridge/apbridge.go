// Package apbridge lets Archipelago's own clients (archipelago.gg) reach the
// game RetroPixels is running: RetroPixels takes the place of the
// emulator-side programs those clients talk to.
//
//   - BizHawk Client: it connects over TCP to 127.0.0.1:43055-43059, where
//     BizHawk would be running Archipelago's connector script, and sends JSON
//     lists of requests, one line each. The Hub answers them the same way.
//   - SNI Client (Super Nintendo): it talks the usb2snes WebSocket protocol to
//     ws://localhost:23074. The Hub serves that address itself, with the game
//     as the one SNES device.
//
// The Hub turns the clients' requests into small batches of memory operations
// (Op) and hands each batch to the running game's Target, which carries it out
// between two frames, so a batch sees and changes the game all at once.
// RetroPixels.exe's Target is the web page running the game (or its Nintendo
// 64 emulator); RetroPixels Native's is the emulation thread, using Exec on
// the core's memory (core.Memory).
//
// Everything listens on the loopback address only.
package apbridge

import (
	"errors"
	"fmt"
	"net"
	"net/http"
	"sort"
	"sync"
	"time"
)

const (
	BizHawkFirstPort = 43055 // the ports Archipelago's BizHawk Client tries, in order
	BizHawkPorts     = 5
	SNIPort          = 23074 // SNI's usb2snes address, the SNI Client's default
	ScriptVersion    = 1     // the connector script version the BizHawk Client expects
	CallTimeout      = 4 * time.Second
	MaxRead          = 16 << 20

	BizHawkClient = "BizHawk Client"
	SNIClient     = "SNI Client"
)

// Op is one memory operation, as the page receives it (JSON).
type Op struct {
	T    string `json:"t"`              // read | write | guard | size | msg | lock | unlock
	D    string `json:"d,omitempty"`    // memory domain, BizHawk's names
	A    int64  `json:"a,omitempty"`    // address in the domain
	N    int64  `json:"n,omitempty"`    // bytes to read
	V    []byte `json:"v,omitempty"`    // bytes to write / expected (base64 in JSON)
	Pad  bool   `json:"pad,omitempty"`  // read / write: bytes past the domain's end read as 0 (are dropped) instead of failing
	Text string `json:"text,omitempty"` // msg
}

// Res is one operation's answer. A batch stops at a failed guard, so its
// answers can be fewer than its operations.
type Res struct {
	V   []byte `json:"v,omitempty"`
	OK  bool   `json:"ok,omitempty"`
	N   int64  `json:"n,omitempty"`
	Err string `json:"err,omitempty"`
}

// Game is the game running.
type Game struct {
	Core   string // RetroPixels' system id (gb, snes, ...)
	System string // BizHawk's system name (GB, GBC, GBA, NES, SNES, N64, GEN, SMS)
	Title  string
	Hash   string // SHA-1 of the ROM in upper-case hex, as BizHawk gives it
}

// Target carries out batches of operations on the running game.
type Target interface {
	Run(ops []Op) ([]Res, error)
}

// ErrNoGame: nothing is running.
var ErrNoGame = errors.New("no game is running in RetroPixels")

// SystemFor is BizHawk's name for each RetroPixels system Archipelago's
// clients can use ("gb" is "GB" or, for Game Boy Color games, "GBC").
var SystemFor = map[string]string{
	"gb": "GB", "gba": "GBA", "nes": "NES", "snes": "SNES", "n64": "N64", "segaMD": "GEN", "sms": "SMS",
}

// ClientFor is the Archipelago client a system's games use.
func ClientFor(system string) string {
	if system == "snes" {
		return SNIClient
	}
	return BizHawkClient
}

// Status is what the indicator shows.
type Status struct {
	Clients     []string `json:"clients"` // connected clients, by kind
	BizHawkPort int      `json:"bizhawkPort"`
	BizHawkErr  string   `json:"bizhawkErr,omitempty"`
	SNIPort     int      `json:"sniPort"`
	SNIErr      string   `json:"sniErr,omitempty"`
	Game        bool     `json:"game"`
}

type client struct {
	kind  string // BizHawkClient | SNIClient
	since time.Time
	live  bool // has talked to us (BizHawk) / attached (SNI)
}

// Hub serves Archipelago's clients for one program.
type Hub struct {
	// Name is the program and its version, for the SNI Client ("RetroPixels 2.8.0").
	Name string
	// OnChange is called (on any goroutine, with nothing locked) when the
	// status changes: a client came or went, a port opened or closed.
	OnChange func()

	mu      sync.Mutex
	game    *Game
	target  Target
	clients map[*client]bool
	bhConns map[net.Conn]bool
	started bool
	wake    chan struct{}

	bizhawkBase int // first BizHawk port (43055)
	sniPort     int // the usb2snes port (23074)

	bizhawkPort int
	bizhawkErr  string
	sniErr      string
	sniOK       bool
}

// NewHub makes a hub for the given ports (BizHawkFirstPort, SNIPort).
func NewHub(bizhawkBase, sniPort int) *Hub {
	return &Hub{clients: map[*client]bool{}, bhConns: map[net.Conn]bool{},
		wake: make(chan struct{}, 1), bizhawkBase: bizhawkBase, sniPort: sniPort, Name: "RetroPixels"}
}

// Loopback reports whether a listen address is on this computer only.
func Loopback(addr string) bool {
	h, _, err := net.SplitHostPort(addr)
	if err != nil {
		return false
	}
	ip := net.ParseIP(h)
	return ip != nil && ip.IsLoopback()
}

// SetGame says which game is running and where its memory is (nil, nil: none).
func (h *Hub) SetGame(g *Game, t Target) {
	h.mu.Lock()
	h.game, h.target = g, t
	if g == nil || t == nil {
		h.game, h.target = nil, nil
	}
	h.mu.Unlock()
	h.changed()
	h.poke()
}

// ClearGame forgets the game if t is still the one running it.
func (h *Hub) ClearGame(t Target) {
	h.mu.Lock()
	gone := h.target == t && t != nil
	if gone {
		h.game, h.target = nil, nil
	}
	h.mu.Unlock()
	if gone {
		h.changed()
		h.poke()
	}
}

// Current is the running game and its target (nil when none).
func (h *Hub) Current() (*Game, Target) {
	h.mu.Lock()
	defer h.mu.Unlock()
	return h.game, h.target
}

// Run carries out a batch on the running game.
func (h *Hub) Run(ops []Op) ([]Res, error) {
	g, t := h.Current()
	if g == nil || t == nil {
		return nil, ErrNoGame
	}
	return t.Run(ops)
}

// Status is the current status.
func (h *Hub) Status() Status {
	h.mu.Lock()
	defer h.mu.Unlock()
	st := Status{Clients: []string{}, BizHawkPort: h.bizhawkPort, BizHawkErr: h.bizhawkErr, SNIErr: h.sniErr, Game: h.game != nil}
	if h.sniOK {
		st.SNIPort = h.sniPort
	}
	seen := map[string]bool{}
	for c := range h.clients {
		if c.live && !seen[c.kind] {
			seen[c.kind] = true
			st.Clients = append(st.Clients, c.kind)
		}
	}
	sort.Strings(st.Clients)
	return st
}

func (h *Hub) changed() {
	if f := h.OnChange; f != nil {
		f()
	}
}

func (h *Hub) addClient(kind string) *client {
	c := &client{kind: kind, since: time.Now()}
	h.mu.Lock()
	h.clients[c] = true
	h.mu.Unlock()
	return c
}

func (h *Hub) setLive(c *client) {
	h.mu.Lock()
	was := c.live
	c.live = true
	h.mu.Unlock()
	if !was {
		h.changed()
	}
}

func (h *Hub) removeClient(c *client) {
	h.mu.Lock()
	delete(h.clients, c)
	was := c.live
	h.mu.Unlock()
	if was {
		h.changed()
	}
}

// ---------------- listeners ----------------

// Start begins serving (once). With sni, the SNI address is served from now
// on: the SNI Client starts SNI itself when nobody answers there, and SNI
// would then hold the address. The BizHawk ports are served only while a game
// is running, so RetroPixels doesn't stand in the way of BizHawk otherwise.
// A port that's taken is tried again every few seconds.
func (h *Hub) Start(sni bool) {
	h.mu.Lock()
	if h.started {
		h.mu.Unlock()
		return
	}
	h.started = true
	h.mu.Unlock()
	if sni {
		go h.sniLoop()
	}
	go h.bizhawkLoop()
}

// poke wakes the BizHawk listener's loop after the game changed.
func (h *Hub) poke() {
	select {
	case h.wake <- struct{}{}:
	default:
	}
}

func (h *Hub) sniLoop() {
	for {
		ln, err := net.Listen("tcp4", fmt.Sprintf("127.0.0.1:%d", h.sniPort))
		h.mu.Lock()
		h.sniOK, h.sniErr = err == nil, ""
		if err != nil {
			h.sniErr = fmt.Sprintf("port %d is in use (is SNI running?)", h.sniPort)
		}
		h.mu.Unlock()
		h.changed()
		if err != nil {
			time.Sleep(5 * time.Second)
			continue
		}
		srv := &http.Server{Handler: http.HandlerFunc(h.ServeSNIHTTP), ReadHeaderTimeout: 10 * time.Second}
		// "localhost" can mean the IPv6 loopback address too; serve it there as well when we can.
		if ln6, err := net.Listen("tcp6", fmt.Sprintf("[::1]:%d", h.sniPort)); err == nil {
			go func() { _ = srv.Serve(ln6) }()
		}
		_ = srv.Serve(ln)
		_ = srv.Close()
		time.Sleep(time.Second)
	}
}

func (h *Hub) bizhawkLoop() {
	var ln net.Listener
	var idle time.Time // when the game went away
	retry := time.NewTicker(5 * time.Second)
	defer retry.Stop()
	for {
		h.mu.Lock()
		want := h.game != nil
		h.mu.Unlock()
		now := time.Now()
		if want {
			idle = time.Time{}
		} else if idle.IsZero() {
			idle = now
		}
		switch {
		case want && ln == nil:
			var err error
			for i := 0; i < BizHawkPorts; i++ {
				if ln, err = net.Listen("tcp4", fmt.Sprintf("127.0.0.1:%d", h.bizhawkBase+i)); err == nil {
					break
				}
			}
			h.mu.Lock()
			h.bizhawkPort, h.bizhawkErr = 0, ""
			if ln != nil {
				h.bizhawkPort = ln.Addr().(*net.TCPAddr).Port
			} else {
				h.bizhawkErr = fmt.Sprintf("ports %d-%d are in use (is BizHawk running?)", h.bizhawkBase, h.bizhawkBase+BizHawkPorts-1)
			}
			h.mu.Unlock()
			h.changed()
			if ln != nil {
				go func(ln net.Listener) {
					for {
						c, err := ln.Accept()
						if err != nil {
							return
						}
						go h.ServeBizHawk(c)
					}
				}(ln)
			}
		case !want && ln != nil && now.Sub(idle) > 4*time.Second:
			// The game has gone (not just restarted): stop answering, and
			// hang up on the clients so they look for BizHawk again.
			ln.Close()
			ln = nil
			h.mu.Lock()
			h.bizhawkPort = 0
			for c := range h.bhConns {
				c.Close()
			}
			h.mu.Unlock()
			h.changed()
		}
		select {
		case <-h.wake:
		case <-retry.C:
		case <-time.After(time.Second):
		}
	}
}
