package main

// Archipelago (archipelago.gg) support: RetroPixels takes the place of the
// emulator-side programs Archipelago's own clients talk to, so the normal
// Archipelago clients can read and write the running game's memory.
//
//   - BizHawk Client: it connects over TCP to 127.0.0.1:43055-43059, where
//     BizHawk would be running Archipelago's connector script, and sends JSON
//     lists of requests, one line each. RetroPixels answers them the same way.
//   - SNI Client (Super Nintendo): it talks the usb2snes WebSocket protocol to
//     ws://localhost:23074. RetroPixels serves that address itself, with the
//     game as the one SNES device.
//
// Game memory lives in the page (RetroPixels' emulators run there), except for
// the Nintendo 64, which runs in this program. Requests go to the page over
// /api/ap/ws as small batches of memory operations that the page carries out
// between two frames, so a batch sees and changes the game all at once.
//
// Everything listens on the loopback address only.

import (
	"bufio"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"log"
	"net"
	"net/http"
	"net/url"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"
)

const (
	bizhawkFirstPort = 43055 // the ports Archipelago's BizHawk Client tries, in order
	bizhawkPorts     = 5
	sniPort          = 23074 // SNI's usb2snes address, the SNI Client's default
	apScriptVersion  = 1     // the connector script version the BizHawk Client expects
	apCallTimeout    = 4 * time.Second
	apMaxRead        = 16 << 20
)

// apOp is one memory operation for the page (or the Nintendo 64 emulator).
type apOp struct {
	T    string `json:"t"`              // read | write | guard | size | msg | lock | unlock
	D    string `json:"d,omitempty"`    // memory domain, BizHawk's names
	A    int64  `json:"a,omitempty"`    // address in the domain
	N    int64  `json:"n,omitempty"`    // bytes to read
	V    []byte `json:"v,omitempty"`    // bytes to write / expected (base64 in JSON)
	Pad  bool   `json:"pad,omitempty"`  // read: bytes past the domain's end read as 0 instead of failing
	Text string `json:"text,omitempty"` // msg
}

// apRes is one operation's answer. The page stops at a failed guard, so a
// batch's answers can be fewer than its operations.
type apRes struct {
	V   []byte `json:"v,omitempty"`
	OK  bool   `json:"ok,omitempty"`
	N   int64  `json:"n,omitempty"`
	Err string `json:"err,omitempty"`
}

// apGame is what the page says about the game it's running.
type apGame struct {
	Core   string `json:"core"`   // RetroPixels' system name (gb, snes, ...)
	System string `json:"system"` // BizHawk's system name (GB, GBC, SNES, ...)
	Title  string `json:"title"`
	Hash   string `json:"hash"`
	N64    string `json:"n64"` // the Nintendo 64 session, which runs here
}

type apPage struct {
	ws      *wsConn
	mu      sync.Mutex
	nextID  uint64
	pending map[uint64]chan []apRes
	closed  chan struct{}
}

type apClient struct {
	kind  string // "BizHawk Client" | "SNI Client"
	since time.Time
	live  bool // has talked to us (BizHawk) / attached (SNI)
}

type apHub struct {
	mu      sync.Mutex
	pages   map[*apPage]bool
	page    *apPage // the page running the game
	game    *apGame
	clients map[*apClient]bool
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

var ap = newAPHub(bizhawkFirstPort, sniPort)

func newAPHub(bizhawkBase, sni int) *apHub {
	return &apHub{pages: map[*apPage]bool{}, clients: map[*apClient]bool{}, bhConns: map[net.Conn]bool{},
		wake: make(chan struct{}, 1), bizhawkBase: bizhawkBase, sniPort: sni}
}

// loopback reports whether a listen address is on this computer only.
func loopback(addr string) bool {
	h, _, err := net.SplitHostPort(addr)
	if err != nil {
		return false
	}
	ip := net.ParseIP(h)
	return ip != nil && ip.IsLoopback()
}

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

func (h *apHub) servePage(ws *wsConn) {
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
		gone := h.page == p
		if gone {
			h.page, h.game = nil, nil
		}
		h.mu.Unlock()
		if gone {
			h.broadcast()
			h.poke()
		}
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
			h.mu.Lock()
			h.page, h.game = p, &g
			h.mu.Unlock()
			h.broadcast()
			h.poke()
		case "nogame":
			h.mu.Lock()
			if h.page == p {
				h.page, h.game = nil, nil
			}
			h.mu.Unlock()
			h.broadcast()
			h.poke()
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
	return p.ws.write(wsText, b)
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
	case <-time.After(apCallTimeout):
		return nil, errors.New("the game didn't answer in time")
	}
}

// current is the running game and where its memory is.
func (h *apHub) current() (*apPage, *apGame) {
	h.mu.Lock()
	defer h.mu.Unlock()
	return h.page, h.game
}

var errNoGame = errors.New("no game is running in RetroPixels")

// run carries out a batch on the running game.
func (h *apHub) run(ops []apOp) ([]apRes, error) {
	p, g := h.current()
	if p == nil || g == nil {
		return nil, errNoGame
	}
	if g.Core == "n64" {
		return runN64Ops(g.N64, p, ops)
	}
	return p.call(ops)
}

// ---------------- status for the page ----------------

type apStatus struct {
	Ev          string   `json:"ev"`
	Clients     []string `json:"clients"`
	BizHawkPort int      `json:"bizhawkPort"`
	BizHawkErr  string   `json:"bizhawkErr,omitempty"`
	SNIPort     int      `json:"sniPort"`
	SNIErr      string   `json:"sniErr,omitempty"`
	Game        bool     `json:"game"`
}

func (h *apHub) statusLocked() apStatus {
	st := apStatus{Ev: "status", Clients: []string{}, BizHawkPort: h.bizhawkPort, BizHawkErr: h.bizhawkErr, SNIErr: h.sniErr, Game: h.game != nil}
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

func (h *apHub) sendStatus(p *apPage) {
	h.mu.Lock()
	st := h.statusLocked()
	h.mu.Unlock()
	_ = p.send(st)
}

func (h *apHub) broadcast() {
	h.mu.Lock()
	st := h.statusLocked()
	pages := make([]*apPage, 0, len(h.pages))
	for p := range h.pages {
		pages = append(pages, p)
	}
	h.mu.Unlock()
	for _, p := range pages {
		_ = p.send(st)
	}
}

func (h *apHub) addClient(kind string) *apClient {
	c := &apClient{kind: kind, since: time.Now()}
	h.mu.Lock()
	h.clients[c] = true
	h.mu.Unlock()
	return c
}

func (h *apHub) setLive(c *apClient) {
	h.mu.Lock()
	was := c.live
	c.live = true
	h.mu.Unlock()
	if !was {
		h.broadcast()
	}
}

func (h *apHub) removeClient(c *apClient) {
	h.mu.Lock()
	delete(h.clients, c)
	was := c.live
	h.mu.Unlock()
	if was {
		h.broadcast()
	}
}

// ---------------- listeners ----------------

// start begins serving (once). The SNI address is served while RetroPixels
// runs: the SNI Client starts SNI itself when nobody answers there, and SNI
// would then hold the address. The BizHawk ports are served only while a game
// is running, so RetroPixels doesn't stand in the way of BizHawk otherwise.
// A port that's taken is tried again every few seconds.
func (h *apHub) start() {
	h.mu.Lock()
	if h.started {
		h.mu.Unlock()
		return
	}
	h.started = true
	h.mu.Unlock()
	go h.sniLoop()
	go h.bizhawkLoop()
}

// poke wakes the BizHawk listener's loop after the game changed.
func (h *apHub) poke() {
	select {
	case h.wake <- struct{}{}:
	default:
	}
}

func (h *apHub) sniLoop() {
	for {
		ln, err := net.Listen("tcp4", fmt.Sprintf("127.0.0.1:%d", h.sniPort))
		h.mu.Lock()
		h.sniOK, h.sniErr = err == nil, ""
		if err != nil {
			h.sniErr = fmt.Sprintf("port %d is in use (is SNI running?)", h.sniPort)
		}
		h.mu.Unlock()
		h.broadcast()
		if err != nil {
			time.Sleep(5 * time.Second)
			continue
		}
		srv := &http.Server{Handler: http.HandlerFunc(h.serveSNIHTTP), ReadHeaderTimeout: 10 * time.Second}
		// "localhost" can mean the IPv6 loopback address too; serve it there as well when we can.
		if ln6, err := net.Listen("tcp6", fmt.Sprintf("[::1]:%d", h.sniPort)); err == nil {
			go func() { _ = srv.Serve(ln6) }()
		}
		_ = srv.Serve(ln)
		_ = srv.Close()
		time.Sleep(time.Second)
	}
}

func (h *apHub) bizhawkLoop() {
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
			for i := 0; i < bizhawkPorts; i++ {
				if ln, err = net.Listen("tcp4", fmt.Sprintf("127.0.0.1:%d", h.bizhawkBase+i)); err == nil {
					break
				}
			}
			h.mu.Lock()
			h.bizhawkPort, h.bizhawkErr = 0, ""
			if ln != nil {
				h.bizhawkPort = ln.Addr().(*net.TCPAddr).Port
			} else {
				h.bizhawkErr = fmt.Sprintf("ports %d-%d are in use (is BizHawk running?)", h.bizhawkBase, h.bizhawkBase+bizhawkPorts-1)
			}
			h.mu.Unlock()
			h.broadcast()
			if ln != nil {
				go func(ln net.Listener) {
					for {
						c, err := ln.Accept()
						if err != nil {
							return
						}
						go h.serveBizHawk(c)
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
			h.broadcast()
		}
		select {
		case <-h.wake:
		case <-retry.C:
		case <-time.After(time.Second):
		}
	}
}

// ---------------- BizHawk Client ----------------

type bhReq struct {
	Type         string          `json:"type"`
	Domain       string          `json:"domain"`
	Address      int64           `json:"address"`
	Size         int64           `json:"size"`
	Value        json.RawMessage `json:"value"`
	ExpectedData string          `json:"expected_data"`
	Message      string          `json:"message"`
}

func (h *apHub) serveBizHawk(c net.Conn) {
	h.mu.Lock()
	h.bhConns[c] = true
	h.mu.Unlock()
	defer func() {
		c.Close()
		h.mu.Lock()
		delete(h.bhConns, c)
		h.mu.Unlock()
	}()
	cl := h.addClient("BizHawk Client")
	defer h.removeClient(cl)
	r := bufio.NewReaderSize(c, 64<<10)
	w := bufio.NewWriter(c)
	for {
		_ = c.SetReadDeadline(time.Now().Add(2 * time.Minute))
		line, err := readLimitedLine(r, 32<<20)
		if err != nil {
			return
		}
		h.setLive(cl)
		var out []byte
		if strings.TrimSpace(line) == "VERSION" {
			out = []byte(strconv.Itoa(apScriptVersion))
		} else {
			out = h.bizhawkBatch([]byte(line))
		}
		_ = c.SetWriteDeadline(time.Now().Add(10 * time.Second))
		if _, err := w.Write(append(out, '\n')); err != nil {
			return
		}
		if err := w.Flush(); err != nil {
			return
		}
	}
}

// readLimitedLine reads up to a newline, refusing absurdly long lines.
func readLimitedLine(r *bufio.Reader, limit int) (string, error) {
	var b []byte
	for {
		part, isPrefix, err := r.ReadLine()
		if err != nil {
			return "", err
		}
		b = append(b, part...)
		if len(b) > limit {
			return "", errors.New("line too long")
		}
		if !isPrefix {
			return string(b), nil
		}
	}
}

func bhErr(text string) map[string]any { return map[string]any{"type": "ERROR", "err": text} }

// bizhawkBatch answers one message: a JSON list of requests, answered in order.
// A failed GUARD stops the rest of the list, which all get the failed answer.
func (h *apHub) bizhawkBatch(line []byte) []byte {
	var reqs []bhReq
	if err := json.Unmarshal(line, &reqs); err != nil {
		b, _ := json.Marshal([]any{bhErr("the request isn't a JSON list of requests")})
		return b
	}
	_, g := h.current()
	var ops []apOp
	opFor := make([]int, len(reqs)) // index into ops, or -1
	pre := make([]map[string]any, len(reqs))
	for i, q := range reqs {
		opFor[i] = -1
		switch q.Type {
		case "PING":
			pre[i] = map[string]any{"type": "PONG"}
		case "SYSTEM":
			sys := "NULL"
			if g != nil {
				sys = g.System
			}
			pre[i] = map[string]any{"type": "SYSTEM_RESPONSE", "value": sys}
		case "HASH":
			hash := ""
			if g != nil {
				hash = g.Hash
			}
			pre[i] = map[string]any{"type": "HASH_RESPONSE", "value": hash}
		case "PREFERRED_CORES":
			// RetroPixels' memory domains are named after these cores' domains.
			pre[i] = map[string]any{"type": "PREFERRED_CORES_RESPONSE", "value": map[string]string{
				"NES": "NesHawk", "SNES": "BSNESv115+", "GB": "Gambatte", "GBC": "Gambatte", "SGB": "Gambatte", "DGB": "GambatteLink",
				"PCE": "TurboNyma", "PCECD": "TurboNyma", "SGX": "TurboNyma"}}
		case "SET_MESSAGE_INTERVAL":
			pre[i] = map[string]any{"type": "SET_MESSAGE_INTERVAL_RESPONSE"}
		case "MEMORY_SIZE":
			opFor[i] = len(ops)
			ops = append(ops, apOp{T: "size", D: q.Domain})
		case "READ":
			if q.Size < 0 || q.Size > apMaxRead {
				pre[i] = bhErr("bad read size")
				continue
			}
			opFor[i] = len(ops)
			ops = append(ops, apOp{T: "read", D: q.Domain, A: q.Address, N: q.Size})
		case "WRITE", "GUARD":
			var s string
			if q.Type == "WRITE" {
				_ = json.Unmarshal(q.Value, &s)
			} else {
				s = q.ExpectedData
			}
			data, err := base64.StdEncoding.DecodeString(s)
			if err != nil {
				pre[i] = bhErr("the data isn't base64")
				continue
			}
			t := "write"
			if q.Type == "GUARD" {
				t = "guard"
			}
			opFor[i] = len(ops)
			ops = append(ops, apOp{T: t, D: q.Domain, A: q.Address, V: data})
		case "DISPLAY_MESSAGE":
			opFor[i] = len(ops)
			ops = append(ops, apOp{T: "msg", Text: q.Message})
		case "LOCK", "UNLOCK":
			opFor[i] = len(ops)
			ops = append(ops, apOp{T: strings.ToLower(q.Type)})
		default:
			pre[i] = bhErr("Unknown command: " + q.Type)
		}
	}
	var res []apRes
	var runErr error
	if len(ops) > 0 {
		res, runErr = h.run(ops)
	}
	out := make([]any, len(reqs))
	var failed map[string]any
	for i, q := range reqs {
		if failed != nil {
			out[i] = failed
			continue
		}
		if opFor[i] < 0 {
			out[i] = pre[i]
			continue
		}
		if runErr != nil {
			out[i] = bhErr(runErr.Error())
			continue
		}
		k := opFor[i]
		if k >= len(res) {
			out[i] = bhErr("the request wasn't carried out")
			continue
		}
		r := res[k]
		if r.Err != "" {
			out[i] = bhErr(r.Err)
			continue
		}
		switch q.Type {
		case "MEMORY_SIZE":
			out[i] = map[string]any{"type": "MEMORY_SIZE_RESPONSE", "value": r.N}
		case "READ":
			out[i] = map[string]any{"type": "READ_RESPONSE", "value": base64.StdEncoding.EncodeToString(r.V)}
		case "WRITE":
			out[i] = map[string]any{"type": "WRITE_RESPONSE"}
		case "GUARD":
			out[i] = map[string]any{"type": "GUARD_RESPONSE", "value": r.OK, "address": q.Address}
			if !r.OK {
				failed = out[i].(map[string]any)
			}
		case "DISPLAY_MESSAGE":
			out[i] = map[string]any{"type": "DISPLAY_MESSAGE_RESPONSE"}
		case "LOCK":
			out[i] = map[string]any{"type": "LOCKED"}
		case "UNLOCK":
			out[i] = map[string]any{"type": "UNLOCKED"}
		}
	}
	b, _ := json.Marshal(out)
	return b
}

// ---------------- SNI Client (usb2snes protocol) ----------------

// sniOriginOK lets in programs (no Origin) and pages on this computer, but not
// websites, which could otherwise reach the game from any browser tab.
func sniOriginOK(o string) bool {
	if o == "" || o == "null" || strings.HasPrefix(o, "file://") {
		return true
	}
	u, err := url.Parse(o)
	if err != nil {
		return false
	}
	h := u.Hostname()
	if h == "localhost" {
		return true
	}
	ip := net.ParseIP(h)
	return ip != nil && ip.IsLoopback()
}

func (h *apHub) serveSNIHTTP(w http.ResponseWriter, r *http.Request) {
	if !sniOriginOK(r.Header.Get("Origin")) {
		http.Error(w, "forbidden", http.StatusForbidden)
		return
	}
	ws, err := wsUpgrade(w, r, 16<<20)
	if err != nil {
		return
	}
	h.serveSNI(ws)
}

type sniReq struct {
	Opcode   string   `json:"Opcode"`
	Space    string   `json:"Space"`
	Operands []string `json:"Operands"`
}

const sniDevice = "RetroPixels SNES"

func (h *apHub) snesRunning() (*apGame, bool) {
	_, g := h.current()
	return g, g != nil && g.Core == "snes"
}

func (h *apHub) serveSNI(ws *wsConn) {
	defer ws.Close()
	cl := h.addClient("SNI Client")
	defer h.removeClient(cl)
	results := func(r ...string) error {
		if r == nil {
			r = []string{}
		}
		b, _ := json.Marshal(map[string]any{"Results": r})
		return ws.write(wsText, b)
	}
	for {
		op, msg, err := ws.ReadMessage()
		if err != nil {
			return
		}
		if op != wsText {
			continue // stray data
		}
		var q sniReq
		if json.Unmarshal(msg, &q) != nil {
			return
		}
		switch q.Opcode {
		case "DeviceList":
			var devs []string
			if _, ok := h.snesRunning(); ok {
				devs = []string{sniDevice}
			}
			err = results(devs...)
		case "AppVersion":
			err = results("RetroPixels " + appVersion + " (usb2snes/SNI-compatible)")
		case "Name":
		case "Attach":
			if _, ok := h.snesRunning(); !ok {
				return
			}
			h.setLive(cl)
		case "Info":
			g, ok := h.snesRunning()
			if !ok {
				return
			}
			err = results("1.0.0", "RetroPixels", g.Title, "NO_FILE_CMD", "NO_CONTROL_CMD")
		case "GetAddress":
			var data []byte
			data, err = h.sniRead(q)
			if err == nil {
				err = ws.WriteBinary(data)
			}
		case "PutAddress":
			err = h.sniWrite(ws, q)
		default:
			err = fmt.Errorf("usb2snes: %s isn't supported", q.Opcode)
		}
		if err != nil {
			if !errors.Is(err, errWSClosed) {
				log.Printf("archipelago: SNI connection: %v", err)
			}
			return
		}
	}
}

// sniPairs reads the hex address/size pairs of GetAddress and PutAddress.
func sniPairs(q sniReq) ([][2]int64, error) {
	if q.Space != "" && q.Space != "SNES" {
		return nil, fmt.Errorf("usb2snes: the %s address space isn't supported", q.Space)
	}
	if len(q.Operands) == 0 || len(q.Operands)%2 != 0 {
		return nil, errors.New("usb2snes: expected address and size pairs")
	}
	var out [][2]int64
	var total int64
	for i := 0; i < len(q.Operands); i += 2 {
		a, err1 := strconv.ParseInt(strings.TrimPrefix(q.Operands[i], "0x"), 16, 64)
		n, err2 := strconv.ParseInt(strings.TrimPrefix(q.Operands[i+1], "0x"), 16, 64)
		if err1 != nil || err2 != nil || a < 0 || n < 0 {
			return nil, errors.New("usb2snes: bad address or size")
		}
		total += n
		if total > apMaxRead {
			return nil, errors.New("usb2snes: request too large")
		}
		out = append(out, [2]int64{a, n})
	}
	return out, nil
}

// The SNES address space as SNI (and the FX Pak) lays it out.
type sniRegion struct {
	start, end int64
	domain     string
}

var sniRegions = []sniRegion{
	{0x000000, 0xE00000, "CARTROM"},
	{0xE00000, 0xF00000, "CARTRAM"},
	{0xF50000, 0xF70000, "WRAM"},
	{0xF70000, 0xF80000, "VRAM"},
	{0xF80000, 0xF90000, "APURAM"},
	{0xF90000, 0xF90200, "CGRAM"},
	{0xF90200, 0xF90420, "OAM"},
}

// sniSplit cuts [a, a+n) into pieces by region; domain "" = nothing there.
func sniSplit(a, n int64) []apOp {
	var out []apOp
	for n > 0 {
		var reg *sniRegion
		next := a + n
		for i := range sniRegions {
			r := &sniRegions[i]
			if a >= r.start && a < r.end {
				reg = r
				break
			}
			if r.start > a && r.start < next {
				next = r.start
			}
		}
		if reg != nil && reg.end < next {
			next = reg.end
		}
		op := apOp{T: "read", A: a, N: next - a, Pad: true}
		if reg != nil {
			op.D, op.A = reg.domain, a-reg.start
		}
		out = append(out, op)
		n -= next - a
		a = next
	}
	return out
}

func (h *apHub) sniRead(q sniReq) ([]byte, error) {
	pairs, err := sniPairs(q)
	if err != nil {
		return nil, err
	}
	if _, ok := h.snesRunning(); !ok {
		return nil, errNoGame
	}
	var ops []apOp
	for _, p := range pairs {
		ops = append(ops, sniSplit(p[0], p[1])...)
	}
	var send []apOp
	for _, o := range ops {
		if o.D != "" {
			send = append(send, o)
		}
	}
	var res []apRes
	if len(send) > 0 {
		if res, err = h.run(send); err != nil {
			return nil, err
		}
	}
	var out []byte
	k := 0
	for _, o := range ops {
		piece := make([]byte, o.N)
		if o.D != "" {
			if k < len(res) && res[k].Err == "" {
				copy(piece, res[k].V)
			}
			k++
		}
		out = append(out, piece...)
	}
	return out, nil
}

func (h *apHub) sniWrite(ws *wsConn, q sniReq) error {
	pairs, err := sniPairs(q)
	if err != nil {
		return err
	}
	var total int64
	for _, p := range pairs {
		total += p[1]
	}
	// The data follows in one or more binary messages.
	var data []byte
	for int64(len(data)) < total {
		op, msg, err := ws.ReadMessage()
		if err != nil {
			return err
		}
		if op != wsBinary {
			return errors.New("usb2snes: expected the data to write")
		}
		data = append(data, msg...)
	}
	if _, ok := h.snesRunning(); !ok {
		return errNoGame
	}
	var ops []apOp
	off := int64(0)
	for _, p := range pairs {
		for _, o := range sniSplit(p[0], p[1]) {
			if o.D != "" {
				ops = append(ops, apOp{T: "write", D: o.D, A: o.A, V: data[off : off+o.N], Pad: true})
			}
			off += o.N
		}
	}
	if len(ops) == 0 {
		return nil
	}
	_, err = h.run(ops)
	return err
}
