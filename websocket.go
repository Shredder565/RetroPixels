package main

// A small WebSocket server (RFC 6455), written for RetroPixels: the page talks
// to the Nintendo 64 emulator running inside this program through it.

import (
	"bufio"
	"crypto/sha1"
	"encoding/base64"
	"encoding/binary"
	"errors"
	"io"
	"net"
	"net/http"
	"strings"
	"sync"
	"time"
)

const (
	wsContinuation = 0
	wsText         = 1
	wsBinary       = 2
	wsClose        = 8
	wsPing         = 9
	wsPong         = 10

	wsGUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
)

var errWSClosed = errors.New("websocket closed")

type wsConn struct {
	c       net.Conn
	r       *bufio.Reader
	wmu     sync.Mutex
	w       *bufio.Writer
	maxSize int // largest message accepted
	closed  bool
}

// headerHas reports whether a comma-separated header contains a token.
func headerHas(h http.Header, name, token string) bool {
	for _, v := range h.Values(name) {
		for _, p := range strings.Split(v, ",") {
			if strings.EqualFold(strings.TrimSpace(p), token) {
				return true
			}
		}
	}
	return false
}

// wsAccept is the Sec-WebSocket-Accept answer for a client key.
func wsAccept(key string) string {
	h := sha1.Sum([]byte(key + wsGUID))
	return base64.StdEncoding.EncodeToString(h[:])
}

// wsUpgrade answers the opening handshake and takes over the connection.
func wsUpgrade(w http.ResponseWriter, r *http.Request, maxSize int) (*wsConn, error) {
	key := r.Header.Get("Sec-WebSocket-Key")
	if r.Method != http.MethodGet || !headerHas(r.Header, "Connection", "upgrade") ||
		!headerHas(r.Header, "Upgrade", "websocket") || key == "" {
		http.Error(w, "expected a WebSocket", http.StatusBadRequest)
		return nil, errors.New("not a websocket request")
	}
	if r.Header.Get("Sec-WebSocket-Version") != "13" {
		w.Header().Set("Sec-WebSocket-Version", "13")
		http.Error(w, "unsupported WebSocket version", http.StatusUpgradeRequired)
		return nil, errors.New("unsupported websocket version")
	}
	hj, ok := w.(http.Hijacker)
	if !ok {
		http.Error(w, "can't upgrade", http.StatusInternalServerError)
		return nil, errors.New("connection can't be hijacked")
	}
	c, brw, err := hj.Hijack()
	if err != nil {
		return nil, err
	}
	_ = c.SetDeadline(time.Time{})
	ws := &wsConn{c: c, r: brw.Reader, w: bufio.NewWriterSize(c, 64<<10), maxSize: maxSize}
	resp := "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + wsAccept(key) + "\r\n\r\n"
	if _, err := ws.w.WriteString(resp); err != nil {
		c.Close()
		return nil, err
	}
	if err := ws.w.Flush(); err != nil {
		c.Close()
		return nil, err
	}
	return ws, nil
}

// readFrame reads one frame and unmasks it.
func (ws *wsConn) readFrame(limit int) (fin bool, op byte, data []byte, err error) {
	var h [2]byte
	if _, err = io.ReadFull(ws.r, h[:]); err != nil {
		return
	}
	fin = h[0]&0x80 != 0
	if h[0]&0x70 != 0 {
		return false, 0, nil, errors.New("websocket: unexpected extension bits")
	}
	op = h[0] & 15
	masked := h[1]&0x80 != 0
	n := uint64(h[1] & 0x7f)
	switch n {
	case 126:
		var b [2]byte
		if _, err = io.ReadFull(ws.r, b[:]); err != nil {
			return
		}
		n = uint64(binary.BigEndian.Uint16(b[:]))
	case 127:
		var b [8]byte
		if _, err = io.ReadFull(ws.r, b[:]); err != nil {
			return
		}
		n = binary.BigEndian.Uint64(b[:])
	}
	if op >= 8 && (n > 125 || !fin) {
		return false, 0, nil, errors.New("websocket: bad control frame")
	}
	if n > uint64(limit) {
		return false, 0, nil, errors.New("websocket: message too large")
	}
	if !masked {
		return false, 0, nil, errors.New("websocket: client frames must be masked")
	}
	var mask [4]byte
	if _, err = io.ReadFull(ws.r, mask[:]); err != nil {
		return
	}
	data = make([]byte, n)
	if _, err = io.ReadFull(ws.r, data); err != nil {
		return
	}
	for i := range data {
		data[i] ^= mask[i&3]
	}
	return
}

// ReadMessage returns the next text or binary message, putting fragmented
// messages back together and answering pings and close on the way.
func (ws *wsConn) ReadMessage() (op byte, msg []byte, err error) {
	var buf []byte
	started := false
	for {
		limit := ws.maxSize - len(buf)
		if limit < 125 {
			limit = 125
		}
		fin, fop, data, err := ws.readFrame(limit)
		if err != nil {
			return 0, nil, err
		}
		switch fop {
		case wsPing:
			if err := ws.write(wsPong, data); err != nil {
				return 0, nil, err
			}
			continue
		case wsPong:
			continue
		case wsClose:
			code := []byte{}
			if len(data) >= 2 {
				code = data[:2]
			}
			_ = ws.write(wsClose, code)
			ws.Close()
			return 0, nil, errWSClosed
		case wsText, wsBinary:
			if started {
				return 0, nil, errors.New("websocket: new message inside a fragmented one")
			}
			started, op, buf = true, fop, data
		case wsContinuation:
			if !started {
				return 0, nil, errors.New("websocket: continuation without a start")
			}
			if len(buf)+len(data) > ws.maxSize {
				return 0, nil, errors.New("websocket: message too large")
			}
			buf = append(buf, data...)
		default:
			return 0, nil, errors.New("websocket: unknown frame type")
		}
		if fin {
			return op, buf, nil
		}
	}
}

// write sends one unmasked frame (servers never mask).
func (ws *wsConn) write(op byte, data []byte) error {
	ws.wmu.Lock()
	defer ws.wmu.Unlock()
	if ws.closed {
		return errWSClosed
	}
	var h [10]byte
	h[0] = 0x80 | op
	n := len(data)
	hl := 2
	switch {
	case n < 126:
		h[1] = byte(n)
	case n < 1<<16:
		h[1] = 126
		binary.BigEndian.PutUint16(h[2:], uint16(n))
		hl = 4
	default:
		h[1] = 127
		binary.BigEndian.PutUint64(h[2:], uint64(n))
		hl = 10
	}
	if _, err := ws.w.Write(h[:hl]); err != nil {
		return err
	}
	if _, err := ws.w.Write(data); err != nil {
		return err
	}
	return ws.w.Flush()
}

// WriteBinary sends a binary message.
func (ws *wsConn) WriteBinary(b []byte) error { return ws.write(wsBinary, b) }

// Close shuts the connection (after a close frame if one wasn't sent yet).
func (ws *wsConn) Close() {
	ws.wmu.Lock()
	defer ws.wmu.Unlock()
	if !ws.closed {
		ws.closed = true
		_ = ws.c.Close()
	}
}

// CloseWith sends a close frame with a status code, then closes.
func (ws *wsConn) CloseWith(code uint16) {
	var b [2]byte
	binary.BigEndian.PutUint16(b[:], code)
	_ = ws.write(wsClose, b[:])
	ws.Close()
}
