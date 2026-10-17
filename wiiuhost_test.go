package main

import (
	"encoding/binary"
	"encoding/json"
	"net/http/httptest"
	"os"
	"strings"
	"testing"
)

func wiiuStart(rate int, name string, rpx []byte) []byte {
	b := []byte{wiiuMsgStart, 0, 0, 0, 0, 0, 0, 0, 0}
	binary.LittleEndian.PutUint32(b[1:], uint32(rate))
	binary.LittleEndian.PutUint32(b[5:], uint32(len(name)))
	b = append(b, name...)
	return append(b, rpx...)
}

// RetroPixels' own OSScreen test program, through the page's connection: the
// first picture comes whole, then only the rows that changed (the frame count), and two presses of
// A end the program.
func TestWiiUSession(t *testing.T) {
	rpx, err := os.ReadFile("wiiu/testdata/screen.rpx")
	if err != nil {
		t.Fatal(err)
	}
	token = "test-token"
	srv := httptest.NewServer(newMux())
	defer srv.Close()
	addr := strings.TrimPrefix(srv.URL, "http://")
	if c, status := wsDial(t, addr, "/api/wiiu/ws?token=wrong", nil); c != nil || !strings.HasPrefix(status, "403") {
		t.Fatalf("wrong token: %s", status)
	}
	c, status := wsDial(t, addr, "/api/wiiu/ws?token=test-token", map[string]string{"Origin": "http://127.0.0.1:47821"})
	if c == nil {
		t.Fatal(status)
	}
	c.frame(true, wsBinary, wiiuStart(48000, "screen.rpx", rpx))
	_, msg := c.read(t)
	if msg[0] != wiiuMsgReady {
		t.Fatalf("expected ready, got %x %q", msg[0], msg[1:])
	}
	var hello struct {
		Session string
		FPS     float64
	}
	if err := json.Unmarshal(msg[1:], &hello); err != nil || hello.Session == "" || hello.FPS < 59.9 {
		t.Fatalf("hello %s: %v", msg[1:], err)
	}

	tick := func(buttons uint32) []byte {
		b := make([]byte, 10)
		b[0], b[1] = wiiuMsgTick, 1
		binary.LittleEndian.PutUint32(b[2:], buttons)
		c.frame(true, wsBinary, b)
		_, msg := c.read(t)
		if msg[0] != wiiuMsgFrame {
			t.Fatalf("message %x: %q", msg[0], msg[1:])
		}
		return msg
	}
	sound, full, same := 0, 0, 0
	for i := 0; i < 30; i++ {
		msg := tick(0)
		w, h := binary.LittleEndian.Uint16(msg[4:]), binary.LittleEndian.Uint16(msg[6:])
		n := int(binary.LittleEndian.Uint32(msg[8:]))
		sound += n
		want := 12 + n*4
		if msg[2] == 2 {
			y0, rows := binary.LittleEndian.Uint16(msg[12:]), binary.LittleEndian.Uint16(msg[14:])
			if y0 == 0 || int(y0)+int(rows) > 720 {
				t.Errorf("rows %d+%d", y0, rows)
			}
			want += 4 + int(rows)*int(w)*4
			same++
		} else if msg[2] == 0 {
			want += int(w) * int(h) * 4
			full++
			if w != 1280 || h != 720 {
				t.Fatalf("picture %dx%d", w, h)
			}
			// the first lines are text on dark blue (0x102040)
			if px := binary.LittleEndian.Uint32(msg[12+(600*1280+10)*4:]); px != 0xff402010 {
				t.Errorf("pixel %08x", px)
			}
		} else if msg[2] == 1 {
			same++
		}
		if len(msg) != want {
			t.Fatalf("frame is %d bytes, want %d", len(msg), want)
		}
	}
	if sound < 700 || full == 0 {
		t.Fatalf("sound %d, pictures %d (%d unchanged)", sound, full, same)
	}
	for i := 0; i < 2; i++ {
		tick(1 << 0) // A
		tick(0)
	}
	for i := 0; i < 5; i++ {
		tick(0)
	}
}
