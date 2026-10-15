package main

import (
	"bufio"
	"bytes"
	"encoding/binary"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"
)

// A tiny WebSocket client for the tests.
type wsTestClient struct {
	c net.Conn
	r *bufio.Reader
}

func wsDial(t *testing.T, addr, path string, header map[string]string) (*wsTestClient, string) {
	t.Helper()
	c, err := net.Dial("tcp", addr)
	if err != nil {
		t.Fatal(err)
	}
	_ = c.SetDeadline(time.Now().Add(60 * time.Second))
	key := "dGhlIHNhbXBsZSBub25jZQ=="
	req := "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1:47821\r\nUpgrade: websocket\r\nConnection: keep-alive, Upgrade\r\n" +
		"Sec-WebSocket-Key: " + key + "\r\nSec-WebSocket-Version: 13\r\n"
	for k, v := range header {
		req += k + ": " + v + "\r\n"
	}
	if _, err := io.WriteString(c, req+"\r\n"); err != nil {
		t.Fatal(err)
	}
	r := bufio.NewReader(c)
	resp, err := http.ReadResponse(r, nil)
	if err != nil {
		t.Fatal(err)
	}
	if resp.StatusCode != 101 {
		c.Close()
		return nil, resp.Status
	}
	if got := resp.Header.Get("Sec-WebSocket-Accept"); got != "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=" {
		t.Fatalf("accept = %q", got)
	}
	return &wsTestClient{c: c, r: r}, resp.Status
}

func (w *wsTestClient) frame(fin bool, op byte, data []byte) {
	var b bytes.Buffer
	h := op
	if fin {
		h |= 0x80
	}
	b.WriteByte(h)
	switch n := len(data); {
	case n < 126:
		b.WriteByte(0x80 | byte(n))
	case n < 1<<16:
		b.WriteByte(0x80 | 126)
		_ = binary.Write(&b, binary.BigEndian, uint16(n))
	default:
		b.WriteByte(0x80 | 127)
		_ = binary.Write(&b, binary.BigEndian, uint64(n))
	}
	mask := []byte{0x12, 0x34, 0x56, 0x78}
	b.Write(mask)
	for i, c := range data {
		b.WriteByte(c ^ mask[i&3])
	}
	_, _ = w.c.Write(b.Bytes())
}

func (w *wsTestClient) read(t *testing.T) (byte, []byte) {
	t.Helper()
	var h [2]byte
	if _, err := io.ReadFull(w.r, h[:]); err != nil {
		t.Fatal(err)
	}
	if h[1]&0x80 != 0 {
		t.Fatal("server frames must not be masked")
	}
	n := uint64(h[1] & 0x7f)
	if n == 126 {
		var b [2]byte
		_, _ = io.ReadFull(w.r, b[:])
		n = uint64(binary.BigEndian.Uint16(b[:]))
	} else if n == 127 {
		var b [8]byte
		_, _ = io.ReadFull(w.r, b[:])
		n = binary.BigEndian.Uint64(b[:])
	}
	data := make([]byte, n)
	if _, err := io.ReadFull(w.r, data); err != nil {
		t.Fatal(err)
	}
	return h[0] & 15, data
}

func TestWebSocketEcho(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		ws, err := wsUpgrade(w, r, 1<<20)
		if err != nil {
			return
		}
		defer ws.Close()
		for {
			op, msg, err := ws.ReadMessage()
			if err != nil {
				return
			}
			_ = ws.Write(op, msg)
		}
	}))
	defer srv.Close()
	c, status := wsDial(t, strings.TrimPrefix(srv.URL, "http://"), "/", nil)
	if c == nil {
		t.Fatal(status)
	}

	// short, medium (16-bit length) and long (64-bit length) messages
	for _, n := range []int{5, 300, 70000} {
		msg := bytes.Repeat([]byte{byte(n)}, n)
		c.frame(true, wsBinary, msg)
		op, got := c.read(t)
		if op != wsBinary || !bytes.Equal(got, msg) {
			t.Fatalf("echo of %d bytes: op %d, %d bytes", n, op, len(got))
		}
	}

	// a fragmented text message with a ping in the middle
	c.frame(false, wsText, []byte("Hel"))
	c.frame(true, wsPing, []byte("hi"))
	c.frame(false, wsContinuation, []byte("lo, "))
	c.frame(true, wsContinuation, []byte("N64"))
	op, got := c.read(t)
	if op != wsPong || string(got) != "hi" {
		t.Fatalf("pong: op %d %q", op, got)
	}
	op, got = c.read(t)
	if op != wsText || string(got) != "Hello, N64" {
		t.Fatalf("fragmented: op %d %q", op, got)
	}

	// close is answered
	c.frame(true, wsClose, []byte{0x03, 0xe8})
	op, got = c.read(t)
	if op != wsClose || !bytes.Equal(got, []byte{0x03, 0xe8}) {
		t.Fatalf("close: op %d %v", op, got)
	}
}

func TestWebSocketRejects(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		ws, err := wsUpgrade(w, r, 100)
		if err != nil {
			return
		}
		defer ws.Close()
		_, _, err = ws.ReadMessage()
		_ = ws.WriteBinary([]byte(fmt.Sprint(err)))
	}))
	defer srv.Close()
	addr := strings.TrimPrefix(srv.URL, "http://")

	// unmasked client frames are refused
	c, _ := wsDial(t, addr, "/", nil)
	_, _ = c.c.Write([]byte{0x82, 0x01, 0x00})
	if _, got := c.read(t); !strings.Contains(string(got), "masked") {
		t.Fatalf("unmasked: %q", got)
	}
	// too large
	c, _ = wsDial(t, addr, "/", nil)
	c.frame(true, wsBinary, make([]byte, 500))
	if _, got := c.read(t); !strings.Contains(string(got), "too large") {
		t.Fatalf("too large: %q", got)
	}
	// plain HTTP is not upgraded
	resp, err := http.Get(srv.URL)
	if err != nil {
		t.Fatal(err)
	}
	resp.Body.Close()
	if resp.StatusCode != http.StatusBadRequest {
		t.Fatalf("plain GET: %s", resp.Status)
	}
}

func TestResampler(t *testing.T) {
	var r n64Resampler
	in := make([]int16, 2*320) // 320 stereo frames at 32 kHz = 10 ms
	for i := range in {
		in[i] = 1000
	}
	var out []int16
	for i := 0; i < 100; i++ {
		out = r.run(in, 32000, 48000, out)
	}
	// one second in -> about 48000 frames out
	if n := len(out) / 2; n < 47990 || n > 48010 {
		t.Fatalf("got %d frames", n)
	}
	if out[len(out)-1] != 1000 {
		t.Fatalf("level %d", out[len(out)-1])
	}
}
