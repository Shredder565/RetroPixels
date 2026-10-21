// Package x11 is a small X11 protocol client in pure Go (no cgo, no Xlib).
// It speaks the core protocol over the display's Unix socket (or TCP) with
// MIT-MAGIC-COOKIE-1 authentication, and has the requests RetroPixels needs
// to find, dock, move and close other programs' windows: atoms,
// properties, QueryTree, CreateWindow, ReparentWindow, ConfigureWindow,
// Map/Unmap, SendEvent, focus and the window events.
//
// A Conn is safe for use from several goroutines. Requests with a reply
// wait for it; requests without one return at once, and any error the
// server reports for them is returned by the next Sync.
package x11

import (
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"net"
	"sync"
	"sync/atomic"
	"time"
)

// Window, Atom and the other resource ids of the protocol.
type (
	Window   uint32
	Atom     uint32
	Colormap uint32
	VisualID uint32
)

// None is the empty resource id; CurrentTime the "now" timestamp.
const (
	None        = 0
	CurrentTime = 0
)

// Setup is what the server tells us when we connect.
type Setup struct {
	ProtocolMajor, ProtocolMinor uint16
	Vendor                       string
	ReleaseNumber                uint32
	ResourceIDBase               uint32
	ResourceIDMask               uint32
	MaxRequestLength             uint16 // in 4-byte units
	MinKeycode, MaxKeycode       uint8
	Screens                      []Screen
}

// Screen is one screen (root window) of the display.
type Screen struct {
	Root            Window
	DefaultColormap Colormap
	WhitePixel      uint32
	BlackPixel      uint32
	Width, Height   uint16 // pixels
	RootVisual      VisualID
	RootDepth       uint8
}

// Conn is a connection to an X server.
type Conn struct {
	nc      net.Conn
	Setup   Setup
	Display Display

	wmu  sync.Mutex // serialises writes and the sequence counter
	seq  atomic.Uint64
	wbuf []byte

	idmu   sync.Mutex
	nextID uint32

	pmu      sync.Mutex
	pending  map[uint64]chan reply
	asyncErr []*Error
	readErr  error

	evmu    sync.Mutex
	evq     []Event
	evWake  chan struct{}
	events  chan Event
	closed  chan struct{}
	closeMu sync.Once

	amu       sync.Mutex
	atoms     map[string]Atom
	atomNames map[Atom]string
}

type reply struct {
	data []byte
	err  error
}

// ErrClosed is returned for requests on a closed connection.
var ErrClosed = errors.New("x11: connection closed")

// Dial connects to the named display ("" means $DISPLAY), using the
// cookie for it in the Xauthority file when there is one.
func Dial(name string) (*Conn, error) {
	d, err := ParseDisplay(name)
	if err != nil {
		return nil, err
	}
	an, ad := findAuth(d, AuthorityFile())
	return DialDisplay(d, an, ad)
}

// DialDisplay connects to d with the given authorization (empty for none).
func DialDisplay(d Display, authName string, authData []byte) (*Conn, error) {
	nc, err := d.dial(5 * time.Second)
	if err != nil {
		return nil, fmt.Errorf("x11: connect to display :%d: %w", d.Number, err)
	}
	c, err := newConn(nc, authName, authData)
	if err != nil {
		nc.Close()
		return nil, err
	}
	c.Display = d
	return c, nil
}

func pad4(n int) int { return (4 - n%4) % 4 }

func newConn(nc net.Conn, authName string, authData []byte) (*Conn, error) {
	// Connection setup, little-endian.
	b := []byte{'l', 0}
	b = binary.LittleEndian.AppendUint16(b, 11)
	b = binary.LittleEndian.AppendUint16(b, 0)
	b = binary.LittleEndian.AppendUint16(b, uint16(len(authName)))
	b = binary.LittleEndian.AppendUint16(b, uint16(len(authData)))
	b = append(b, 0, 0)
	b = append(b, authName...)
	b = append(b, make([]byte, pad4(len(authName)))...)
	b = append(b, authData...)
	b = append(b, make([]byte, pad4(len(authData)))...)
	nc.SetDeadline(time.Now().Add(10 * time.Second))
	if _, err := nc.Write(b); err != nil {
		return nil, fmt.Errorf("x11: setup: %w", err)
	}
	var hdr [8]byte
	if _, err := io.ReadFull(nc, hdr[:]); err != nil {
		return nil, fmt.Errorf("x11: setup: %w", err)
	}
	body := make([]byte, int(binary.LittleEndian.Uint16(hdr[6:]))*4)
	if _, err := io.ReadFull(nc, body); err != nil {
		return nil, fmt.Errorf("x11: setup: %w", err)
	}
	nc.SetDeadline(time.Time{})
	switch hdr[0] {
	case 0: // Failed
		n := int(hdr[1])
		if n > len(body) {
			n = len(body)
		}
		return nil, fmt.Errorf("x11: server refused the connection: %s", trimReason(body[:n]))
	case 2: // Authenticate
		return nil, fmt.Errorf("x11: server wants more authentication: %s", trimReason(body))
	case 1:
	default:
		return nil, fmt.Errorf("x11: bad setup reply %d", hdr[0])
	}
	s, err := parseSetup(hdr, body)
	if err != nil {
		return nil, err
	}
	c := &Conn{
		nc:        nc,
		Setup:     s,
		pending:   map[uint64]chan reply{},
		evWake:    make(chan struct{}, 1),
		events:    make(chan Event),
		closed:    make(chan struct{}),
		atoms:     map[string]Atom{},
		atomNames: map[Atom]string{},
	}
	go c.readLoop()
	go c.eventPump()
	return c, nil
}

func trimReason(b []byte) string {
	for len(b) > 0 && (b[len(b)-1] == 0 || b[len(b)-1] == '\n') {
		b = b[:len(b)-1]
	}
	if len(b) == 0 {
		return "(no reason given)"
	}
	return string(b)
}

func parseSetup(hdr [8]byte, b []byte) (Setup, error) {
	le := binary.LittleEndian
	bad := errors.New("x11: short setup reply")
	if len(b) < 32 {
		return Setup{}, bad
	}
	s := Setup{
		ProtocolMajor:    le.Uint16(hdr[2:]),
		ProtocolMinor:    le.Uint16(hdr[4:]),
		ReleaseNumber:    le.Uint32(b[0:]),
		ResourceIDBase:   le.Uint32(b[4:]),
		ResourceIDMask:   le.Uint32(b[8:]),
		MaxRequestLength: le.Uint16(b[18:]),
		MinKeycode:       b[26],
		MaxKeycode:       b[27],
	}
	vlen := int(le.Uint16(b[16:]))
	nscreens := int(b[20])
	nformats := int(b[21])
	off := 32
	if off+vlen > len(b) {
		return s, bad
	}
	s.Vendor = string(b[off : off+vlen])
	off += vlen + pad4(vlen) + 8*nformats
	for i := 0; i < nscreens; i++ {
		if off+40 > len(b) {
			return s, bad
		}
		sc := Screen{
			Root:            Window(le.Uint32(b[off:])),
			DefaultColormap: Colormap(le.Uint32(b[off+4:])),
			WhitePixel:      le.Uint32(b[off+8:]),
			BlackPixel:      le.Uint32(b[off+12:]),
			Width:           le.Uint16(b[off+20:]),
			Height:          le.Uint16(b[off+22:]),
			RootVisual:      VisualID(le.Uint32(b[off+32:])),
			RootDepth:       b[off+38],
		}
		ndepths := int(b[off+39])
		off += 40
		for j := 0; j < ndepths; j++ {
			if off+8 > len(b) {
				return s, bad
			}
			nvis := int(le.Uint16(b[off+2:]))
			off += 8 + 24*nvis
		}
		s.Screens = append(s.Screens, sc)
	}
	if len(s.Screens) == 0 {
		return s, errors.New("x11: server has no screens")
	}
	return s, nil
}

// DefaultScreen is the screen the display name chose (or the first).
func (c *Conn) DefaultScreen() *Screen {
	if c.Display.Screen < len(c.Setup.Screens) {
		return &c.Setup.Screens[c.Display.Screen]
	}
	return &c.Setup.Screens[0]
}

// Root is the default screen's root window.
func (c *Conn) Root() Window { return c.DefaultScreen().Root }

// Close closes the connection. Windows this connection created are
// destroyed by the server.
func (c *Conn) Close() error {
	var err error
	c.closeMu.Do(func() {
		err = c.nc.Close()
		close(c.closed)
	})
	return err
}

// NewID allocates a resource id for a new window or other resource.
func (c *Conn) NewID() (uint32, error) {
	c.idmu.Lock()
	defer c.idmu.Unlock()
	mask := c.Setup.ResourceIDMask
	if mask == 0 {
		return 0, errors.New("x11: no resource ids")
	}
	step := mask & -mask
	if c.nextID > mask || c.nextID&^mask != 0 {
		return 0, errors.New("x11: out of resource ids")
	}
	id := c.Setup.ResourceIDBase | c.nextID
	c.nextID += step
	return id, nil
}

// request is a request being encoded.
type request struct{ b []byte }

func newRequest(opcode, data byte) *request {
	return &request{b: []byte{opcode, data, 0, 0}}
}

func (r *request) u8(v uint8) *request   { r.b = append(r.b, v); return r }
func (r *request) u16(v uint16) *request { r.b = binary.LittleEndian.AppendUint16(r.b, v); return r }
func (r *request) u32(v uint32) *request { r.b = binary.LittleEndian.AppendUint32(r.b, v); return r }
func (r *request) i16(v int16) *request  { return r.u16(uint16(v)) }
func (r *request) pad(n int) *request    { r.b = append(r.b, make([]byte, n)...); return r }
func (r *request) bytes(p []byte) *request {
	r.b = append(r.b, p...)
	return r.pad(pad4(len(p)))
}

// send writes a request and returns its sequence number and, for a
// request with a reply, the channel the reply arrives on.
func (c *Conn) send(r *request, hasReply bool) (uint64, chan reply, error) {
	r.pad(pad4(len(r.b)))
	n := len(r.b) / 4
	if n > int(c.Setup.MaxRequestLength) || n > 0xffff {
		return 0, nil, fmt.Errorf("x11: request %d is too long (%d units)", r.b[0], n)
	}
	binary.LittleEndian.PutUint16(r.b[2:], uint16(n))
	var ch chan reply
	c.wmu.Lock()
	defer c.wmu.Unlock()
	select {
	case <-c.closed:
		return 0, nil, ErrClosed
	default:
	}
	seq := c.seq.Load() + 1
	if hasReply {
		ch = make(chan reply, 1)
		c.pmu.Lock()
		if c.readErr != nil {
			err := c.readErr
			c.pmu.Unlock()
			return 0, nil, err
		}
		c.pending[seq] = ch
		c.pmu.Unlock()
	}
	c.seq.Store(seq)
	if _, err := c.nc.Write(r.b); err != nil {
		if hasReply {
			c.pmu.Lock()
			delete(c.pending, seq)
			c.pmu.Unlock()
		}
		return 0, nil, fmt.Errorf("x11: write: %w", err)
	}
	return seq, ch, nil
}

// sendVoid sends a request that has no reply.
func (c *Conn) sendVoid(r *request) error {
	_, _, err := c.send(r, false)
	return err
}

// roundTrip sends a request and waits for its reply.
func (c *Conn) roundTrip(r *request) ([]byte, error) {
	_, ch, err := c.send(r, true)
	if err != nil {
		return nil, err
	}
	return c.wait(ch)
}

func (c *Conn) wait(ch chan reply) ([]byte, error) {
	rep := <-ch
	return rep.data, rep.err
}

// fullSeq widens the 16-bit sequence number the server sends to our
// 64-bit counter: the latest request we sent with those low bits.
func fullSeq(sent uint64, s16 uint16) uint64 {
	full := sent&^0xffff | uint64(s16)
	if full > sent && full >= 0x10000 {
		full -= 0x10000
	}
	return full
}

func (c *Conn) readLoop() {
	var err error
	for {
		buf := make([]byte, 32)
		if _, err = io.ReadFull(c.nc, buf); err != nil {
			break
		}
		switch buf[0] {
		case 0:
			e := parseError(buf)
			seq := fullSeq(c.seq.Load(), e.Sequence)
			c.pmu.Lock()
			if ch, ok := c.pending[seq]; ok {
				delete(c.pending, seq)
				ch <- reply{err: e}
			} else {
				e.seq = seq
				if len(c.asyncErr) < 256 {
					c.asyncErr = append(c.asyncErr, e)
				}
			}
			c.pmu.Unlock()
		case 1:
			extra := int(binary.LittleEndian.Uint32(buf[4:])) * 4
			if extra > 0 {
				buf = append(buf, make([]byte, extra)...)
				if _, err = io.ReadFull(c.nc, buf[32:]); err != nil {
					break
				}
			}
			seq := fullSeq(c.seq.Load(), binary.LittleEndian.Uint16(buf[2:]))
			c.pmu.Lock()
			if ch, ok := c.pending[seq]; ok {
				delete(c.pending, seq)
				ch <- reply{data: buf}
			}
			c.pmu.Unlock()
		default:
			if buf[0]&0x7f == GenericEvent {
				extra := int(binary.LittleEndian.Uint32(buf[4:])) * 4
				if extra > 0 {
					buf = append(buf, make([]byte, extra)...)
					if _, err = io.ReadFull(c.nc, buf[32:]); err != nil {
						break
					}
				}
			}
			c.pushEvent(parseEvent(buf))
		}
		if err != nil {
			break
		}
	}
	select {
	case <-c.closed:
		err = ErrClosed
	default:
		err = fmt.Errorf("x11: connection lost: %w", err)
	}
	c.pmu.Lock()
	c.readErr = err
	for s, ch := range c.pending {
		ch <- reply{err: err}
		delete(c.pending, s)
	}
	c.pmu.Unlock()
	c.Close()
}

func (c *Conn) pushEvent(e Event) {
	c.evmu.Lock()
	c.evq = append(c.evq, e)
	c.evmu.Unlock()
	select {
	case c.evWake <- struct{}{}:
	default:
	}
}

// eventPump moves events from the unbounded queue to the Events channel,
// so a slow reader never blocks the replies.
func (c *Conn) eventPump() {
	defer close(c.events)
	for {
		c.evmu.Lock()
		var e Event
		if len(c.evq) > 0 {
			e = c.evq[0]
			c.evq[0] = nil
			c.evq = c.evq[1:]
		}
		c.evmu.Unlock()
		if e == nil {
			select {
			case <-c.evWake:
				continue
			case <-c.closed:
				return
			}
		}
		select {
		case c.events <- e:
		case <-c.closed:
			return
		}
	}
}

// Events delivers the events the server sends, in order. It is closed when
// the connection closes.
func (c *Conn) Events() <-chan Event { return c.events }

// WaitEvent waits up to timeout for the first event match accepts; events
// it skips are dropped. It returns nil on timeout or when the connection
// closes.
func (c *Conn) WaitEvent(timeout time.Duration, match func(Event) bool) Event {
	t := time.NewTimer(timeout)
	defer t.Stop()
	for {
		select {
		case e, ok := <-c.events:
			if !ok {
				return nil
			}
			if match == nil || match(e) {
				return e
			}
		case <-t.C:
			return nil
		}
	}
}

// Sync waits until the server has handled every request sent so far and
// returns the first error it reported for a request without a reply since
// the last Sync (nil if none).
func (c *Conn) Sync() error {
	_, ch, err := c.send(newRequest(opGetInputFocus, 0), true)
	if err != nil {
		return err
	}
	if _, err := c.wait(ch); err != nil {
		return err
	}
	c.pmu.Lock()
	errs := c.asyncErr
	c.asyncErr = nil
	c.pmu.Unlock()
	if len(errs) > 0 {
		return errs[0]
	}
	return nil
}
