package main

// The engine's protocol: the Android app starts this program and talks to it
// over its standard input (requests) and standard output (replies), one
// reply for each request, in order. Every message is
//
//	u32 length (little-endian; the bytes that follow), u8 type, body
//
// Types:
//
//	'J'  JSON, both ways: a request {"cmd": "open", ...}; its reply
//	     {"ok": true, ...} or {"ok": false, "error": "...", ...}
//	'R'  run frames (app to engine), answered by
//	'F'  a frame: the picture, the sound and the rumble
//
// The engine sends one 'J' message first, unasked: {"ok": true, "hello":
// "RetroPixels engine", "protocol": 1, "version": "2.14.0"}.
//
// 'R' body, little-endian:
//
//	u8  frames to run (0 = only read the input)
//	u8  flags: 1 = fast-forward (sound from the first frame only)
//	f32 volume 0..1
//	u32 touch buttons: bit i = Info.Buttons[i] held on the touch controls (player 1)
//	u8  touch axes: bit 0 = axis 0 below is used, bit 1 = axis 1 (player 1's
//	    left stick: the N64's analog stick, the Atari paddle)
//	f32 axis 0, f32 axis 1
//	u8  controllers, then for each (in player order):
//	    u16 buttons (bit i = standard gamepad button i), f32 x 4 axes
//	u8  keys held (or tapped since the last run), then for each: u8 length, name
//
// 'F' body:
//
//	u8  0 = running, 1 = the emulator stopped (the message says why)
//	u16 width, u16 height (of the picture, when there is one)
//	f32 rumble 0..1
//	u8  flags: 1 = a picture follows the sound
//	u16 length, message (UTF-8; "" or something to show the player)
//	u32 samples, then that many s16 (stereo, interleaved, at the open's rate)
//	the picture: width x height pixels, 4 bytes each, R G B 255 (Android's
//	ARGB_8888 bitmap layout, for Bitmap.copyPixelsFromBuffer)

import (
	"bufio"
	"encoding/binary"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"math"
)

const protocolVersion = 1

const maxMessage = 64 << 20

const (
	msgJSON  = 'J'
	msgRun   = 'R'
	msgFrame = 'F'
)

func readMessage(r *bufio.Reader, buf []byte) (byte, []byte, error) {
	var hdr [4]byte
	if _, err := io.ReadFull(r, hdr[:]); err != nil {
		return 0, buf, err
	}
	n := binary.LittleEndian.Uint32(hdr[:])
	if n < 1 || n > maxMessage {
		return 0, buf, fmt.Errorf("bad message length %d", n)
	}
	if cap(buf) < int(n) {
		buf = make([]byte, n)
	}
	buf = buf[:n]
	if _, err := io.ReadFull(r, buf); err != nil {
		return 0, buf, err
	}
	return buf[0], buf[1:], nil
}

func writeMessage(w *bufio.Writer, typ byte, body []byte) error {
	var hdr [5]byte
	binary.LittleEndian.PutUint32(hdr[:], uint32(len(body)+1))
	hdr[4] = typ
	w.Write(hdr[:])
	w.Write(body)
	return w.Flush()
}

func writeJSON(w *bufio.Writer, v any) error {
	b, err := json.Marshal(v)
	if err != nil {
		return err
	}
	return writeMessage(w, msgJSON, b)
}

// runRequest is a decoded 'R' body.
type runRequest struct {
	frames    int
	fast      bool
	volume    float32
	touch     uint32
	touchAxes uint8
	axis      [2]float32
	pads      []padIn
	keys      map[string]bool
}

type padIn struct {
	buttons uint16
	axes    [4]float32
}

var errShort = errors.New("run request too short")

type reader struct {
	b   []byte
	err error
}

func (r *reader) take(n int) []byte {
	if r.err != nil || len(r.b) < n {
		r.err = errShort
		return make([]byte, n)
	}
	out := r.b[:n]
	r.b = r.b[n:]
	return out
}

func (r *reader) u8() uint8   { return r.take(1)[0] }
func (r *reader) u16() uint16 { return binary.LittleEndian.Uint16(r.take(2)) }
func (r *reader) u32() uint32 { return binary.LittleEndian.Uint32(r.take(4)) }
func (r *reader) f32() float32 {
	v := math.Float32frombits(r.u32())
	if v != v || math.IsInf(float64(v), 0) {
		return 0
	}
	return v
}

func decodeRun(b []byte, keys map[string]bool) (runRequest, error) {
	r := &reader{b: b}
	var q runRequest
	q.frames = int(r.u8())
	q.fast = r.u8()&1 != 0
	q.volume = r.f32()
	q.touch = r.u32()
	q.touchAxes = r.u8()
	q.axis[0], q.axis[1] = r.f32(), r.f32()
	n := int(r.u8())
	for i := 0; i < n && r.err == nil; i++ {
		var p padIn
		p.buttons = r.u16()
		for a := range p.axes {
			p.axes[a] = r.f32()
		}
		q.pads = append(q.pads, p)
	}
	clear(keys)
	n = int(r.u8())
	for i := 0; i < n && r.err == nil; i++ {
		l := int(r.u8())
		keys[string(r.take(l))] = true
	}
	q.keys = keys
	if q.frames > 16 {
		q.frames = 16
	}
	return q, r.err
}

// encodeRun makes an 'R' body (for tests and the reference tool).
func encodeRun(q runRequest) []byte {
	var out []byte
	put32 := func(v uint32) { out = binary.LittleEndian.AppendUint32(out, v) }
	putf := func(v float32) { put32(math.Float32bits(v)) }
	flags := byte(0)
	if q.fast {
		flags = 1
	}
	out = append(out, byte(q.frames), flags)
	putf(q.volume)
	put32(q.touch)
	out = append(out, q.touchAxes)
	putf(q.axis[0])
	putf(q.axis[1])
	out = append(out, byte(len(q.pads)))
	for _, p := range q.pads {
		out = binary.LittleEndian.AppendUint16(out, p.buttons)
		for _, a := range p.axes {
			putf(a)
		}
	}
	out = append(out, byte(len(q.keys)))
	for k := range q.keys {
		out = append(out, byte(len(k)))
		out = append(out, k...)
	}
	return out
}

// frameReply is what an 'F' body carries.
type frameReply struct {
	failed  bool
	w, h    int
	rumble  float32
	message string
	pcm     []int16
	pix     []uint32 // nil: no picture
}

// appendFrame encodes an 'F' body into out (reused between frames).
func appendFrame(out []byte, f *frameReply) []byte {
	out = out[:0]
	st := byte(0)
	if f.failed {
		st = 1
	}
	flags := byte(0)
	if f.pix != nil {
		flags = 1
	}
	out = append(out, st)
	out = binary.LittleEndian.AppendUint16(out, uint16(f.w))
	out = binary.LittleEndian.AppendUint16(out, uint16(f.h))
	out = binary.LittleEndian.AppendUint32(out, math.Float32bits(f.rumble))
	out = append(out, flags)
	msg := f.message
	if len(msg) > 4000 {
		msg = msg[:4000]
	}
	out = binary.LittleEndian.AppendUint16(out, uint16(len(msg)))
	out = append(out, msg...)
	out = binary.LittleEndian.AppendUint32(out, uint32(len(f.pcm)))
	for _, s := range f.pcm {
		out = binary.LittleEndian.AppendUint16(out, uint16(s))
	}
	if f.pix != nil {
		out = appendRGBA(out, f.pix)
	}
	return out
}

// appendRGBA writes 0x00RRGGBB pixels as R, G, B, 255 bytes.
func appendRGBA(out []byte, pix []uint32) []byte {
	n := len(out)
	need := n + len(pix)*4
	if cap(out) < need {
		grown := make([]byte, n, need+need/4)
		copy(grown, out)
		out = grown
	}
	out = out[:need]
	d := out[n:]
	for i, p := range pix {
		d[i*4] = byte(p >> 16)
		d[i*4+1] = byte(p >> 8)
		d[i*4+2] = byte(p)
		d[i*4+3] = 255
	}
	return out
}
