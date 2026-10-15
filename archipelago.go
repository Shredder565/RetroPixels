package main

// Archipelago (archipelago.gg) support: RetroPixels takes the place of the
// emulator-side programs Archipelago's own clients talk to, so the normal
// Archipelago clients can read and write the running game's memory.
// internal/apbridge serves the clients (the BizHawk Client on
// 127.0.0.1:43055-43059, the SNI Client's usb2snes address on port 23074).
//
// Game memory lives in the page (RetroPixels' emulators run there), except for
// the Nintendo 64, which runs in this program. Requests go to the page over
// /api/ap/ws as small batches of memory operations that the page carries out
// between two frames, so a batch sees and changes the game all at once.

import (
	"encoding/json"
	"errors"
	"net/http"
	"sync"
	"time"

	"retropixels/internal/apbridge"
)

type apOp = apbridge.Op
type apRes = apbridge.Res

// apGame is what the page says about the game it's running.
type apGame struct {
	Core   string `json:"core"`   // RetroPixels' system name (gb, snes, ...)
	System string `json:"system"` // BizHawk's system name (GB, GBC, SNES, ...)
	Title  string `json:"title"`
	Hash   string `json:"hash"`
	N64    string `json:"n64"` // the Nintendo 64 session, which runs here
}

// apPage is a page's connection; the page running the game is the hub's Target.
type apPage struct {
	ws      *wsConn
	mu      sync.Mutex
	nextID  uint64
	pending map[uint64]chan []apRes
	closed  chan struct{}
	game    *apGame
}

// apWeb: the hub, and the pages that hear its status.
type apWeb struct {
	*apbridge.Hub
	mu    sync.Mutex
	pages map[*apPage]bool
}

var ap = newAPWeb(apbridge.BizHawkFirstPort, apbridge.SNIPort)

func newAPWeb(bizhawkBase, sni int) *apWeb {
	w := &apWeb{Hub: apbridge.NewHub(bizhawkBase, sni), pages: map[*apPage]bool{}}
	w.Hub.Name = "RetroPixels " + appVersion
	w.Hub.OnChange = w.broadcast
	return w
}

func (h *apWeb) start() { h.Hub.Start(true) }

// ---------------- the page's connection ----------------

func registerAPRoutes(mux *http.ServeMux) {
	registerAPPatchRoutes(mux)
	mux.HandleFunc("/api/ap/ws", func(w http.ResponseWriter, r *http.Request) {
		if !hostOK[r.Host] || token == "" || r.URL.Query().Get("token") != token {
			http.Error(w, "forbidden", http.StatusForbidden)
			return
		}
		if o := r.Header.Get("Origin"); o != "" && o != "http://"+r.Host {
			http.Error(w, "forbidden", http.StatusForbidden)
			return
		}
		ws, err := wsUpgrade(w, r, 64<<20)
		if err != nil {
			return
		}
		ap.servePage(ws)
	})
}

func (h *apWeb) servePage(ws *wsConn) {
	p := &apPage{ws: ws, pending: map[uint64]chan []apRes{}, closed: make(chan struct{})}
	h.mu.Lock()
	h.pages[p] = true
	h.mu.Unlock()
	h.sendStatus(p)
	defer func() {
		close(p.closed)
		ws.Close()
		h.mu.Lock()
		delete(h.pages, p)
		h.mu.Unlock()
		h.ClearGame(p)
	}()
	for {
		op, msg, err := ws.ReadMessage()
		if err != nil {
			return
		}
		if op != wsText {
			continue
		}
		var m struct {
			ID   uint64          `json:"id"`
			Res  []apRes         `json:"res"`
			Ev   string          `json:"ev"`
			Game json.RawMessage `json:"game"`
		}
		if json.Unmarshal(msg, &m) != nil {
			continue
		}
		switch m.Ev {
		case "game":
			var g apGame
			if json.Unmarshal(m.Game, &g) != nil {
				continue
			}
			p.mu.Lock()
			p.game = &g
			p.mu.Unlock()
			h.SetGame(&apbridge.Game{Core: g.Core, System: g.System, Title: g.Title, Hash: g.Hash}, p)
		case "nogame":
			h.ClearGame(p)
		case "":
			p.mu.Lock()
			ch := p.pending[m.ID]
			delete(p.pending, m.ID)
			p.mu.Unlock()
			if ch != nil {
				ch <- m.Res
			}
		}
	}
}

func (p *apPage) send(v any) error {
	b, _ := json.Marshal(v)
	return p.ws.Write(wsText, b)
}

// Run carries out a batch on the page's game: in the page, or for a Nintendo
// 64 game in its emulator here.
func (p *apPage) Run(ops []apOp) ([]apRes, error) {
	p.mu.Lock()
	g := p.game
	p.mu.Unlock()
	if g != nil && g.Core == "n64" {
		return runN64Ops(g.N64, p, ops)
	}
	return p.call(ops)
}

// call has the page carry out a batch of operations.
func (p *apPage) call(ops []apOp) ([]apRes, error) {
	ch := make(chan []apRes, 1)
	p.mu.Lock()
	p.nextID++
	id := p.nextID
	p.pending[id] = ch
	p.mu.Unlock()
	defer func() {
		p.mu.Lock()
		delete(p.pending, id)
		p.mu.Unlock()
	}()
	if err := p.send(map[string]any{"id": id, "ops": ops}); err != nil {
		return nil, errors.New("the game's page closed")
	}
	select {
	case res := <-ch:
		return res, nil
	case <-p.closed:
		return nil, errors.New("the game's page closed")
	case <-time.After(apbridge.CallTimeout):
		return nil, errors.New("the game didn't answer in time")
	}
}

var errNoGame = apbridge.ErrNoGame

// ---------------- status for the page ----------------

type apStatus struct {
	Ev string `json:"ev"`
	apbridge.Status
}

func (h *apWeb) status() apStatus { return apStatus{Ev: "status", Status: h.Status()} }

func (h *apWeb) sendStatus(p *apPage) { _ = p.send(h.status()) }

func (h *apWeb) broadcast() {
	st := h.status()
	h.mu.Lock()
	pages := make([]*apPage, 0, len(h.pages))
	for p := range h.pages {
		pages = append(pages, p)
	}
	h.mu.Unlock()
	for _, p := range pages {
		_ = p.send(st)
	}
}
