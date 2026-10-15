package apbridge

// The SNI Client's side: the usb2snes WebSocket protocol, with the running
// Super Nintendo game as the one device.

import (
	"encoding/json"
	"errors"
	"fmt"
	"log"
	"net"
	"net/http"
	"net/url"
	"strconv"
	"strings"

	"retropixels/internal/websocket"
)

// SNIOriginOK lets in programs (no Origin) and pages on this computer, but not
// websites, which could otherwise reach the game from any browser tab.
func SNIOriginOK(o string) bool {
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

// ServeSNIHTTP takes a usb2snes WebSocket connection.
func (h *Hub) ServeSNIHTTP(w http.ResponseWriter, r *http.Request) {
	if !SNIOriginOK(r.Header.Get("Origin")) {
		http.Error(w, "forbidden", http.StatusForbidden)
		return
	}
	ws, err := websocket.Upgrade(w, r, 16<<20)
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

func (h *Hub) snesRunning() (*Game, bool) {
	g, _ := h.Current()
	return g, g != nil && g.Core == "snes"
}

func (h *Hub) serveSNI(ws *websocket.Conn) {
	defer ws.Close()
	cl := h.addClient(SNIClient)
	defer h.removeClient(cl)
	results := func(r ...string) error {
		if r == nil {
			r = []string{}
		}
		b, _ := json.Marshal(map[string]any{"Results": r})
		return ws.WriteText(b)
	}
	for {
		op, msg, err := ws.ReadMessage()
		if err != nil {
			return
		}
		if op != websocket.OpText {
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
			err = results(h.Name + " (usb2snes/SNI-compatible)")
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
			if !errors.Is(err, websocket.ErrClosed) {
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
		if total > MaxRead {
			return nil, errors.New("usb2snes: request too large")
		}
		out = append(out, [2]int64{a, n})
	}
	return out, nil
}

// SNIRegion is one part of the SNES address space as SNI (and the FX Pak)
// lays it out, and the memory domain it is.
type SNIRegion struct {
	Start, End int64
	Domain     string
}

// SNIRegions: the address space the SNI Client reads and writes. A Super
// Nintendo Target has to answer these domains (offsets from Start), each
// laid out as BizHawk's bsnes core lays it out.
var SNIRegions = []SNIRegion{
	{0x000000, 0xE00000, "CARTROM"},
	{0xE00000, 0xF00000, "CARTRAM"},
	{0xF50000, 0xF70000, "WRAM"},
	{0xF70000, 0xF80000, "VRAM"},
	{0xF80000, 0xF90000, "APURAM"},
	{0xF90000, 0xF90200, "CGRAM"},
	{0xF90200, 0xF90420, "OAM"},
}

// SNISplit cuts [a, a+n) into reads by region; domain "" = nothing there.
func SNISplit(a, n int64) []Op {
	var out []Op
	for n > 0 {
		var reg *SNIRegion
		next := a + n
		for i := range SNIRegions {
			r := &SNIRegions[i]
			if a >= r.Start && a < r.End {
				reg = r
				break
			}
			if r.Start > a && r.Start < next {
				next = r.Start
			}
		}
		if reg != nil && reg.End < next {
			next = reg.End
		}
		op := Op{T: "read", A: a, N: next - a, Pad: true}
		if reg != nil {
			op.D, op.A = reg.Domain, a-reg.Start
		}
		out = append(out, op)
		n -= next - a
		a = next
	}
	return out
}

func (h *Hub) sniRead(q sniReq) ([]byte, error) {
	pairs, err := sniPairs(q)
	if err != nil {
		return nil, err
	}
	if _, ok := h.snesRunning(); !ok {
		return nil, ErrNoGame
	}
	var ops []Op
	for _, p := range pairs {
		ops = append(ops, SNISplit(p[0], p[1])...)
	}
	var send []Op
	for _, o := range ops {
		if o.D != "" {
			send = append(send, o)
		}
	}
	var res []Res
	if len(send) > 0 {
		if res, err = h.Run(send); err != nil {
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

func (h *Hub) sniWrite(ws *websocket.Conn, q sniReq) error {
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
		if op != websocket.OpBinary {
			return errors.New("usb2snes: expected the data to write")
		}
		data = append(data, msg...)
	}
	if _, ok := h.snesRunning(); !ok {
		return ErrNoGame
	}
	var ops []Op
	off := int64(0)
	for _, p := range pairs {
		for _, o := range SNISplit(p[0], p[1]) {
			if o.D != "" {
				ops = append(ops, Op{T: "write", D: o.D, A: o.A, V: data[off : off+o.N], Pad: true})
			}
			off += o.N
		}
	}
	if len(ops) == 0 {
		return nil
	}
	_, err = h.Run(ops)
	return err
}
