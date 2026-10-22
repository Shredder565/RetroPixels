package x11

import (
	"encoding/binary"
	"errors"
	"fmt"
)

// Event codes.
const (
	KeyPress         = 2
	KeyRelease       = 3
	ButtonPress      = 4
	ButtonRelease    = 5
	MotionNotify     = 6
	EnterNotify      = 7
	LeaveNotify      = 8
	FocusIn          = 9
	FocusOut         = 10
	Expose           = 12
	VisibilityNotify = 15
	CreateNotify     = 16
	DestroyNotify    = 17
	UnmapNotify      = 18
	MapNotify        = 19
	MapRequest       = 20
	ReparentNotify   = 21
	ConfigureNotify  = 22
	ConfigureRequest = 23
	PropertyNotify   = 28
	ClientMessage    = 33
	MappingNotify    = 34
	GenericEvent     = 35
)

// Event masks (for CreateWindow/ChangeWindowAttributes and SendEvent).
const (
	NoEventMask              = 0
	KeyPressMask             = 1 << 0
	KeyReleaseMask           = 1 << 1
	ButtonPressMask          = 1 << 2
	ButtonReleaseMask        = 1 << 3
	EnterWindowMask          = 1 << 4
	LeaveWindowMask          = 1 << 5
	PointerMotionMask        = 1 << 6
	ExposureMask             = 1 << 15
	VisibilityChangeMask     = 1 << 16
	StructureNotifyMask      = 1 << 17
	ResizeRedirectMask       = 1 << 18
	SubstructureNotifyMask   = 1 << 19
	SubstructureRedirectMask = 1 << 20
	FocusChangeMask          = 1 << 21
	PropertyChangeMask       = 1 << 22
)

// Event is an event from the server. The concrete types are the *Event
// structs below; anything else arrives as RawEvent.
type Event interface {
	// Code is the event code, without the "sent by SendEvent" bit.
	Code() uint8
}

// header holds what every event has.
type header struct {
	code uint8
	// Synthetic is true for events another client sent with SendEvent.
	Synthetic bool
}

func (h header) Code() uint8 { return h.code }

// RawEvent is an event this package does not decode.
type RawEvent struct {
	header
	Data []byte // the whole event, 32 bytes or more
}

// KeyEvent is KeyPress/KeyRelease; ButtonPress/ButtonRelease/MotionNotify
// and Enter/LeaveNotify use the same layout.
type KeyEvent struct {
	header
	Detail             uint8 // keycode or button
	Time               uint32
	Root, Event, Child Window
	RootX, RootY       int16
	X, Y               int16
	State              uint16
	SameScreen         bool
}

// FocusEvent is FocusIn or FocusOut.
type FocusEvent struct {
	header
	Detail uint8
	Window Window
	Mode   uint8
}

// ExposeEvent asks for part of a window to be drawn again.
type ExposeEvent struct {
	header
	Window              Window
	X, Y, Width, Height uint16
	Count               uint16
}

// CreateNotifyEvent: a window was created under Parent.
type CreateNotifyEvent struct {
	header
	Parent, Window        Window
	X, Y                  int16
	Width, Height, Border uint16
	OverrideRedirect      bool
}

// DestroyNotifyEvent: Window was destroyed.
type DestroyNotifyEvent struct {
	header
	Event, Window Window
}

// UnmapNotifyEvent: Window was unmapped.
type UnmapNotifyEvent struct {
	header
	Event, Window Window
	FromConfigure bool
}

// MapNotifyEvent: Window was mapped.
type MapNotifyEvent struct {
	header
	Event, Window    Window
	OverrideRedirect bool
}

// MapRequestEvent: a client asked to map Window (for the parent's
// SubstructureRedirect holder).
type MapRequestEvent struct {
	header
	Parent, Window Window
}

// ReparentNotifyEvent: Window got a new Parent.
type ReparentNotifyEvent struct {
	header
	Event, Window, Parent Window
	X, Y                  int16
	OverrideRedirect      bool
}

// ConfigureNotifyEvent: Window's size, place, border or stacking changed.
type ConfigureNotifyEvent struct {
	header
	Event, Window, Above  Window
	X, Y                  int16
	Width, Height, Border uint16
	OverrideRedirect      bool
}

// ConfigureRequestEvent: a client asked to configure Window (for the
// parent's SubstructureRedirect holder).
type ConfigureRequestEvent struct {
	header
	StackMode             uint8
	Parent, Window, Above Window
	X, Y                  int16
	Width, Height, Border uint16
	ValueMask             uint16
}

// PropertyNotifyEvent: a property of Window changed (State 0) or was
// deleted (State 1).
type PropertyNotifyEvent struct {
	header
	Window Window
	Atom   Atom
	Time   uint32
	State  uint8
}

// ClientMessageEvent is a message one client sends another through
// SendEvent (WM_PROTOCOLS, _NET_ACTIVE_WINDOW, our own messages).
type ClientMessageEvent struct {
	header
	Format uint8 // 8, 16 or 32
	Window Window
	Type   Atom
	Data   [20]byte
}

// Data32 is Data read as five 32-bit values (Format 32).
func (e *ClientMessageEvent) Data32() [5]uint32 {
	var v [5]uint32
	for i := range v {
		v[i] = binary.LittleEndian.Uint32(e.Data[4*i:])
	}
	return v
}

func parseEvent(b []byte) Event {
	le := binary.LittleEndian
	h := header{code: b[0] & 0x7f, Synthetic: b[0]&0x80 != 0}
	u16 := func(o int) uint16 { return le.Uint16(b[o:]) }
	i16 := func(o int) int16 { return int16(le.Uint16(b[o:])) }
	win := func(o int) Window { return Window(le.Uint32(b[o:])) }
	switch h.code {
	case KeyPress, KeyRelease, ButtonPress, ButtonRelease, MotionNotify, EnterNotify, LeaveNotify:
		return &KeyEvent{header: h, Detail: b[1], Time: le.Uint32(b[4:]), Root: win(8), Event: win(12),
			Child: win(16), RootX: i16(20), RootY: i16(22), X: i16(24), Y: i16(26), State: u16(28), SameScreen: b[30] != 0}
	case FocusIn, FocusOut:
		return &FocusEvent{header: h, Detail: b[1], Window: win(4), Mode: b[8]}
	case Expose:
		return &ExposeEvent{header: h, Window: win(4), X: u16(8), Y: u16(10), Width: u16(12), Height: u16(14), Count: u16(16)}
	case CreateNotify:
		return &CreateNotifyEvent{header: h, Parent: win(4), Window: win(8), X: i16(12), Y: i16(14),
			Width: u16(16), Height: u16(18), Border: u16(20), OverrideRedirect: b[22] != 0}
	case DestroyNotify:
		return &DestroyNotifyEvent{header: h, Event: win(4), Window: win(8)}
	case UnmapNotify:
		return &UnmapNotifyEvent{header: h, Event: win(4), Window: win(8), FromConfigure: b[12] != 0}
	case MapNotify:
		return &MapNotifyEvent{header: h, Event: win(4), Window: win(8), OverrideRedirect: b[12] != 0}
	case MapRequest:
		return &MapRequestEvent{header: h, Parent: win(4), Window: win(8)}
	case ReparentNotify:
		return &ReparentNotifyEvent{header: h, Event: win(4), Window: win(8), Parent: win(12),
			X: i16(16), Y: i16(18), OverrideRedirect: b[20] != 0}
	case ConfigureNotify:
		return &ConfigureNotifyEvent{header: h, Event: win(4), Window: win(8), Above: win(12), X: i16(16), Y: i16(18),
			Width: u16(20), Height: u16(22), Border: u16(24), OverrideRedirect: b[26] != 0}
	case ConfigureRequest:
		return &ConfigureRequestEvent{header: h, StackMode: b[1], Parent: win(4), Window: win(8), Above: win(12),
			X: i16(16), Y: i16(18), Width: u16(20), Height: u16(22), Border: u16(24), ValueMask: u16(26)}
	case PropertyNotify:
		return &PropertyNotifyEvent{header: h, Window: win(4), Atom: Atom(le.Uint32(b[8:])), Time: le.Uint32(b[12:]), State: b[16]}
	case ClientMessage:
		e := &ClientMessageEvent{header: h, Format: b[1], Window: win(4), Type: Atom(le.Uint32(b[8:]))}
		copy(e.Data[:], b[12:32])
		return e
	}
	return &RawEvent{header: h, Data: b}
}

// Error codes.
const (
	BadRequest        = 1
	BadValue          = 2
	BadWindow         = 3
	BadPixmap         = 4
	BadAtom           = 5
	BadCursor         = 6
	BadFont           = 7
	BadMatch          = 8
	BadDrawable       = 9
	BadAccess         = 10
	BadAlloc          = 11
	BadColormap       = 12
	BadGC             = 13
	BadIDChoice       = 14
	BadName           = 15
	BadLength         = 16
	BadImplementation = 17
)

var errorNames = map[uint8]string{
	BadRequest: "BadRequest", BadValue: "BadValue", BadWindow: "BadWindow", BadPixmap: "BadPixmap",
	BadAtom: "BadAtom", BadCursor: "BadCursor", BadFont: "BadFont", BadMatch: "BadMatch",
	BadDrawable: "BadDrawable", BadAccess: "BadAccess", BadAlloc: "BadAlloc", BadColormap: "BadColormap",
	BadGC: "BadGC", BadIDChoice: "BadIDChoice", BadName: "BadName", BadLength: "BadLength",
	BadImplementation: "BadImplementation",
}

// Error is an error the X server reported for a request.
type Error struct {
	Code     uint8
	Sequence uint16
	BadValue uint32 // the bad resource id or value
	Minor    uint16
	Major    uint8 // the request's opcode
	seq      uint64
}

func (e *Error) Error() string {
	name := errorNames[e.Code]
	if name == "" {
		name = fmt.Sprintf("error %d", e.Code)
	}
	op := opNames[e.Major]
	if op == "" {
		op = fmt.Sprintf("request %d", e.Major)
	}
	return fmt.Sprintf("x11: %s in %s (value 0x%x)", name, op, e.BadValue)
}

func parseError(b []byte) *Error {
	le := binary.LittleEndian
	return &Error{Code: b[1], Sequence: le.Uint16(b[2:]), BadValue: le.Uint32(b[4:]), Minor: le.Uint16(b[8:]), Major: b[10]}
}

// IsError reports whether err is an X error with the given code, such as
// BadWindow for a window that is gone.
func IsError(err error, code uint8) bool {
	var e *Error
	return errors.As(err, &e) && e.Code == code
}

// WindowGone reports whether err says a window no longer exists
// (BadWindow, or BadDrawable from GetGeometry).
func WindowGone(err error) bool {
	return IsError(err, BadWindow) || IsError(err, BadDrawable)
}
