package apbridge

// The BizHawk Client's side: the protocol Archipelago's connector script
// (connector_bizhawk_generic.lua) speaks, one JSON list of requests per line.

import (
	"bufio"
	"encoding/base64"
	"encoding/json"
	"errors"
	"net"
	"strconv"
	"strings"
	"time"
)

type bhReq struct {
	Type         string          `json:"type"`
	Domain       string          `json:"domain"`
	Address      int64           `json:"address"`
	Size         int64           `json:"size"`
	Value        json.RawMessage `json:"value"`
	ExpectedData string          `json:"expected_data"`
	Message      string          `json:"message"`
}

// ServeBizHawk answers one BizHawk Client connection until it closes.
func (h *Hub) ServeBizHawk(c net.Conn) {
	h.mu.Lock()
	h.bhConns[c] = true
	h.mu.Unlock()
	defer func() {
		c.Close()
		h.mu.Lock()
		delete(h.bhConns, c)
		h.mu.Unlock()
	}()
	cl := h.addClient(BizHawkClient)
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
			out = []byte(strconv.Itoa(ScriptVersion))
		} else {
			out = h.BizHawkBatch([]byte(line))
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

// BizHawkBatch answers one message: a JSON list of requests, answered in order.
// A failed GUARD stops the rest of the list, which all get the failed answer.
func (h *Hub) BizHawkBatch(line []byte) []byte {
	var reqs []bhReq
	if err := json.Unmarshal(line, &reqs); err != nil {
		b, _ := json.Marshal([]any{bhErr("the request isn't a JSON list of requests")})
		return b
	}
	g, _ := h.Current()
	var ops []Op
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
			ops = append(ops, Op{T: "size", D: q.Domain})
		case "READ":
			if q.Size < 0 || q.Size > MaxRead {
				pre[i] = bhErr("bad read size")
				continue
			}
			opFor[i] = len(ops)
			ops = append(ops, Op{T: "read", D: q.Domain, A: q.Address, N: q.Size})
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
			ops = append(ops, Op{T: t, D: q.Domain, A: q.Address, V: data})
		case "DISPLAY_MESSAGE":
			opFor[i] = len(ops)
			ops = append(ops, Op{T: "msg", Text: q.Message})
		case "LOCK", "UNLOCK":
			opFor[i] = len(ops)
			ops = append(ops, Op{T: strings.ToLower(q.Type)})
		default:
			pre[i] = bhErr("Unknown command: " + q.Type)
		}
	}
	var res []Res
	var runErr error
	if len(ops) > 0 {
		res, runErr = h.Run(ops)
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
