package x11

import (
	"encoding/binary"
	"errors"
	"fmt"
)

// Request opcodes used here.
const (
	opCreateWindow           = 1
	opChangeWindowAttributes = 2
	opGetWindowAttributes    = 3
	opDestroyWindow          = 4
	opChangeSaveSet          = 6
	opReparentWindow         = 7
	opMapWindow              = 8
	opUnmapWindow            = 10
	opConfigureWindow        = 12
	opGetGeometry            = 14
	opQueryTree              = 15
	opInternAtom             = 16
	opGetAtomName            = 17
	opChangeProperty         = 18
	opDeleteProperty         = 19
	opGetProperty            = 20
	opSendEvent              = 25
	opTranslateCoordinates   = 40
	opSetInputFocus          = 42
	opGetInputFocus          = 43
	opQueryExtension         = 98
	opKillClient             = 113
)

var opNames = map[uint8]string{
	opCreateWindow: "CreateWindow", opChangeWindowAttributes: "ChangeWindowAttributes",
	opGetWindowAttributes: "GetWindowAttributes", opDestroyWindow: "DestroyWindow",
	opChangeSaveSet: "ChangeSaveSet", opReparentWindow: "ReparentWindow", opMapWindow: "MapWindow",
	opUnmapWindow: "UnmapWindow", opConfigureWindow: "ConfigureWindow", opGetGeometry: "GetGeometry",
	opQueryTree: "QueryTree", opInternAtom: "InternAtom", opGetAtomName: "GetAtomName",
	opChangeProperty: "ChangeProperty", opDeleteProperty: "DeleteProperty", opGetProperty: "GetProperty",
	opSendEvent: "SendEvent", opTranslateCoordinates: "TranslateCoordinates",
	opSetInputFocus: "SetInputFocus", opGetInputFocus: "GetInputFocus",
	opQueryExtension: "QueryExtension", opKillClient: "KillClient",
}

var le = binary.LittleEndian

// Window attribute bits for WindowAttributes.Mask.
const (
	CWBackPixmap       = 1 << 0
	CWBackPixel        = 1 << 1
	CWBorderPixmap     = 1 << 2
	CWBorderPixel      = 1 << 3
	CWBitGravity       = 1 << 4
	CWWinGravity       = 1 << 5
	CWBackingStore     = 1 << 6
	CWBackingPlanes    = 1 << 7
	CWBackingPixel     = 1 << 8
	CWOverrideRedirect = 1 << 9
	CWSaveUnder        = 1 << 10
	CWEventMask        = 1 << 11
	CWDontPropagate    = 1 << 12
	CWColormap         = 1 << 13
	CWCursor           = 1 << 14
)

// WindowAttributes are the settable attributes of a window; Mask says
// which fields are used.
type WindowAttributes struct {
	Mask             uint32
	BackPixmap       uint32
	BackPixel        uint32
	BorderPixmap     uint32
	BorderPixel      uint32
	BitGravity       uint8
	WinGravity       uint8
	BackingStore     uint8
	BackingPlanes    uint32
	BackingPixel     uint32
	OverrideRedirect bool
	SaveUnder        bool
	EventMask        uint32
	DontPropagate    uint32
	Colormap         Colormap
	Cursor           uint32
}

func b2u(b bool) uint32 {
	if b {
		return 1
	}
	return 0
}

func (a *WindowAttributes) encode(r *request) {
	if a == nil {
		r.u32(0)
		return
	}
	r.u32(a.Mask & 0x7fff)
	vals := [15]uint32{a.BackPixmap, a.BackPixel, a.BorderPixmap, a.BorderPixel, uint32(a.BitGravity),
		uint32(a.WinGravity), uint32(a.BackingStore), a.BackingPlanes, a.BackingPixel, b2u(a.OverrideRedirect),
		b2u(a.SaveUnder), a.EventMask, a.DontPropagate, uint32(a.Colormap), a.Cursor}
	for i, v := range vals {
		if a.Mask&(1<<i) != 0 {
			r.u32(v)
		}
	}
}

// Window classes.
const (
	CopyFromParent = 0
	InputOutput    = 1
	InputOnly      = 2
)

// CreateWindow creates an InputOutput window with the parent's depth and
// visual and returns its id. The window starts unmapped.
func (c *Conn) CreateWindow(parent Window, x, y int16, width, height, border uint16, attr *WindowAttributes) (Window, error) {
	id, err := c.NewID()
	if err != nil {
		return 0, err
	}
	if width == 0 {
		width = 1
	}
	if height == 0 {
		height = 1
	}
	r := newRequest(opCreateWindow, CopyFromParent).u32(id).u32(uint32(parent)).
		i16(x).i16(y).u16(width).u16(height).u16(border).u16(InputOutput).u32(CopyFromParent)
	attr.encode(r)
	return Window(id), c.sendVoid(r)
}

// ChangeWindowAttributes sets the attributes in attr.Mask.
func (c *Conn) ChangeWindowAttributes(w Window, attr *WindowAttributes) error {
	r := newRequest(opChangeWindowAttributes, 0).u32(uint32(w))
	attr.encode(r)
	return c.sendVoid(r)
}

// SelectInput sets the events this connection gets for w.
func (c *Conn) SelectInput(w Window, mask uint32) error {
	return c.ChangeWindowAttributes(w, &WindowAttributes{Mask: CWEventMask, EventMask: mask})
}

// Map states.
const (
	IsUnmapped   = 0
	IsUnviewable = 1
	IsViewable   = 2
)

// Attributes is the reply to GetWindowAttributes.
type Attributes struct {
	Visual           VisualID
	Class            uint16
	MapState         uint8 // IsUnmapped, IsUnviewable or IsViewable
	OverrideRedirect bool
	Colormap         Colormap
	AllEventMasks    uint32
	YourEventMask    uint32
}

// GetWindowAttributes returns w's attributes and map state.
func (c *Conn) GetWindowAttributes(w Window) (Attributes, error) {
	b, err := c.roundTrip(newRequest(opGetWindowAttributes, 0).u32(uint32(w)))
	if err != nil {
		return Attributes{}, err
	}
	if len(b) < 44 {
		return Attributes{}, errShort
	}
	return Attributes{
		Visual:           VisualID(le.Uint32(b[8:])),
		Class:            le.Uint16(b[12:]),
		MapState:         b[26],
		OverrideRedirect: b[27] != 0,
		Colormap:         Colormap(le.Uint32(b[28:])),
		AllEventMasks:    le.Uint32(b[32:]),
		YourEventMask:    le.Uint32(b[36:]),
	}, nil
}

var errShort = errors.New("x11: short reply")

// DestroyWindow destroys w and its children.
func (c *Conn) DestroyWindow(w Window) error {
	return c.sendVoid(newRequest(opDestroyWindow, 0).u32(uint32(w)))
}

// Save-set modes.
const (
	SetModeInsert = 0
	SetModeDelete = 1
)

// ChangeSaveSet adds w (another client's window) to or removes it from
// this connection's save-set: when this connection closes, windows in it
// that sit inside our windows are put back on the root instead of being
// destroyed with them.
func (c *Conn) ChangeSaveSet(mode uint8, w Window) error {
	return c.sendVoid(newRequest(opChangeSaveSet, mode).u32(uint32(w)))
}

// ReparentWindow moves w into parent at (x, y) of the parent's inside.
// A mapped window is unmapped and mapped again by the server.
func (c *Conn) ReparentWindow(w, parent Window, x, y int16) error {
	return c.sendVoid(newRequest(opReparentWindow, 0).u32(uint32(w)).u32(uint32(parent)).i16(x).i16(y))
}

// MapWindow shows w (once its ancestors are mapped).
func (c *Conn) MapWindow(w Window) error {
	return c.sendVoid(newRequest(opMapWindow, 0).u32(uint32(w)))
}

// UnmapWindow hides w.
func (c *Conn) UnmapWindow(w Window) error {
	return c.sendVoid(newRequest(opUnmapWindow, 0).u32(uint32(w)))
}

// ConfigureWindow mask bits.
const (
	ConfigX           = 1 << 0
	ConfigY           = 1 << 1
	ConfigWidth       = 1 << 2
	ConfigHeight      = 1 << 3
	ConfigBorderWidth = 1 << 4
	ConfigSibling     = 1 << 5
	ConfigStackMode   = 1 << 6
)

// Stack modes.
const (
	Above    = 0
	Below    = 1
	TopIf    = 2
	BottomIf = 3
	Opposite = 4
)

// WindowChanges are the values of ConfigureWindow; Mask says which are
// used.
type WindowChanges struct {
	Mask          uint16
	X, Y          int32
	Width, Height uint32
	BorderWidth   uint32
	Sibling       Window
	StackMode     uint8
}

// ConfigureWindow changes w's place, size, border or stacking.
func (c *Conn) ConfigureWindow(w Window, ch WindowChanges) error {
	r := newRequest(opConfigureWindow, 0).u32(uint32(w)).u16(ch.Mask & 0x7f).pad(2)
	vals := [7]uint32{uint32(ch.X), uint32(ch.Y), ch.Width, ch.Height, ch.BorderWidth, uint32(ch.Sibling), uint32(ch.StackMode)}
	for i, v := range vals {
		if ch.Mask&(1<<i) != 0 {
			r.u32(v)
		}
	}
	return c.sendVoid(r)
}

// MoveResizeWindow places w at (x, y) with the given size.
func (c *Conn) MoveResizeWindow(w Window, x, y int, width, height int) error {
	if width < 1 {
		width = 1
	}
	if height < 1 {
		height = 1
	}
	return c.ConfigureWindow(w, WindowChanges{Mask: ConfigX | ConfigY | ConfigWidth | ConfigHeight,
		X: int32(x), Y: int32(y), Width: uint32(width), Height: uint32(height)})
}

// RaiseWindow puts w on top of its siblings.
func (c *Conn) RaiseWindow(w Window) error {
	return c.ConfigureWindow(w, WindowChanges{Mask: ConfigStackMode, StackMode: Above})
}

// Geometry is the reply to GetGeometry; X and Y are relative to the
// parent's inside.
type Geometry struct {
	Root          Window
	Depth         uint8
	X, Y          int16
	Width, Height uint16
	BorderWidth   uint16
}

// GetGeometry returns w's size and place in its parent.
func (c *Conn) GetGeometry(w Window) (Geometry, error) {
	b, err := c.roundTrip(newRequest(opGetGeometry, 0).u32(uint32(w)))
	if err != nil {
		return Geometry{}, err
	}
	return Geometry{Depth: b[1], Root: Window(le.Uint32(b[8:])), X: int16(le.Uint16(b[12:])), Y: int16(le.Uint16(b[14:])),
		Width: le.Uint16(b[16:]), Height: le.Uint16(b[18:]), BorderWidth: le.Uint16(b[20:])}, nil
}

// QueryTree returns w's root, parent and children (bottom to top).
func (c *Conn) QueryTree(w Window) (root, parent Window, children []Window, err error) {
	b, err := c.roundTrip(newRequest(opQueryTree, 0).u32(uint32(w)))
	if err != nil {
		return 0, 0, nil, err
	}
	n := int(le.Uint16(b[16:]))
	if len(b) < 32+4*n {
		return 0, 0, nil, errShort
	}
	children = make([]Window, n)
	for i := range children {
		children[i] = Window(le.Uint32(b[32+4*i:]))
	}
	return Window(le.Uint32(b[8:])), Window(le.Uint32(b[12:])), children, nil
}

// InternAtom returns the atom for name, creating it unless onlyIfExists
// (then a missing atom is None). Atoms are cached.
func (c *Conn) InternAtom(name string, onlyIfExists bool) (Atom, error) {
	c.amu.Lock()
	a, ok := c.atoms[name]
	c.amu.Unlock()
	if ok {
		return a, nil
	}
	b, err := c.roundTrip(newRequest(opInternAtom, uint8(b2u(onlyIfExists))).u16(uint16(len(name))).pad(2).bytes([]byte(name)))
	if err != nil {
		return 0, err
	}
	a = Atom(le.Uint32(b[8:]))
	if a != None {
		c.amu.Lock()
		c.atoms[name] = a
		c.atomNames[a] = name
		c.amu.Unlock()
	}
	return a, nil
}

// Atom is InternAtom(name, false).
func (c *Conn) Atom(name string) (Atom, error) { return c.InternAtom(name, false) }

// Atoms interns several atoms with one round trip.
func (c *Conn) Atoms(names ...string) ([]Atom, error) {
	out := make([]Atom, len(names))
	chans := make([]chan reply, len(names))
	c.amu.Lock()
	for i, n := range names {
		out[i] = c.atoms[n]
	}
	c.amu.Unlock()
	for i, n := range names {
		if out[i] != None {
			continue
		}
		_, ch, err := c.send(newRequest(opInternAtom, 0).u16(uint16(len(n))).pad(2).bytes([]byte(n)), true)
		if err != nil {
			return nil, err
		}
		chans[i] = ch
	}
	var first error
	for i, ch := range chans {
		if ch == nil {
			continue
		}
		b, err := c.wait(ch)
		if err != nil {
			if first == nil {
				first = err
			}
			continue
		}
		out[i] = Atom(le.Uint32(b[8:]))
		c.amu.Lock()
		c.atoms[names[i]] = out[i]
		c.atomNames[out[i]] = names[i]
		c.amu.Unlock()
	}
	return out, first
}

// AtomName returns the name of an atom.
func (c *Conn) AtomName(a Atom) (string, error) {
	c.amu.Lock()
	n, ok := c.atomNames[a]
	c.amu.Unlock()
	if ok {
		return n, nil
	}
	b, err := c.roundTrip(newRequest(opGetAtomName, 0).u32(uint32(a)))
	if err != nil {
		return "", err
	}
	l := int(le.Uint16(b[8:]))
	if len(b) < 32+l {
		return "", errShort
	}
	n = string(b[32 : 32+l])
	c.amu.Lock()
	c.atomNames[a] = n
	c.atoms[n] = a
	c.amu.Unlock()
	return n, nil
}

// Property modes.
const (
	PropModeReplace = 0
	PropModePrepend = 1
	PropModeAppend  = 2
)

// ChangeProperty sets a property. format is 8, 16 or 32 (bits per item)
// and data is little-endian items of that size.
func (c *Conn) ChangeProperty(w Window, prop, typ Atom, format uint8, mode uint8, data []byte) error {
	if format != 8 && format != 16 && format != 32 {
		return fmt.Errorf("x11: bad property format %d", format)
	}
	if len(data)%(int(format)/8) != 0 {
		return fmt.Errorf("x11: property data is not a whole number of %d-bit items", format)
	}
	r := newRequest(opChangeProperty, mode).u32(uint32(w)).u32(uint32(prop)).u32(uint32(typ)).
		u8(format).pad(3).u32(uint32(len(data) / (int(format) / 8))).bytes(data)
	return c.sendVoid(r)
}

// DeleteProperty removes a property.
func (c *Conn) DeleteProperty(w Window, prop Atom) error {
	return c.sendVoid(newRequest(opDeleteProperty, 0).u32(uint32(w)).u32(uint32(prop)))
}

// Property is a property value. Type is None when the window does not
// have the property.
type Property struct {
	Type   Atom
	Format uint8
	Value  []byte
}

// Uint32s reads a format-32 value (CARDINAL, WINDOW, ATOM lists).
func (p Property) Uint32s() []uint32 {
	if p.Format != 32 {
		return nil
	}
	v := make([]uint32, len(p.Value)/4)
	for i := range v {
		v[i] = le.Uint32(p.Value[4*i:])
	}
	return v
}

// GetPropertyPart is the raw GetProperty request: offset and length are in
// 4-byte units; bytesAfter says how much is left.
func (c *Conn) GetPropertyPart(w Window, prop, typ Atom, offset, length uint32, del bool) (p Property, bytesAfter uint32, err error) {
	b, err := c.roundTrip(newRequest(opGetProperty, uint8(b2u(del))).u32(uint32(w)).u32(uint32(prop)).
		u32(uint32(typ)).u32(offset).u32(length))
	if err != nil {
		return Property{}, 0, err
	}
	p.Format = b[1]
	p.Type = Atom(le.Uint32(b[8:]))
	bytesAfter = le.Uint32(b[12:])
	n := int(le.Uint32(b[16:])) * int(p.Format) / 8
	if len(b) < 32+n {
		return Property{}, 0, errShort
	}
	p.Value = b[32 : 32+n]
	return p, bytesAfter, nil
}

// GetProperty reads a whole property of any type (typ None) or of typ (a
// property of another type comes back with Value empty and its Type).
func (c *Conn) GetProperty(w Window, prop, typ Atom) (Property, error) {
	const chunk = 16384 // 64 KiB per reply
	var out Property
	var off uint32
	for {
		p, after, err := c.GetPropertyPart(w, prop, typ, off, chunk, false)
		if err != nil {
			return Property{}, err
		}
		if off == 0 {
			out.Type, out.Format = p.Type, p.Format
		}
		out.Value = append(out.Value, p.Value...)
		if after == 0 || len(p.Value) == 0 || p.Type == None {
			return out, nil
		}
		off += uint32(len(p.Value) / 4)
		if len(out.Value) > 64<<20 {
			return Property{}, errors.New("x11: property too large")
		}
	}
}

// SendEvent sends a 32-byte event to dest. With mask 0 it goes to the
// client that created dest; otherwise to every client selecting a bit of
// mask on dest (or the first ancestor with propagate).
func (c *Conn) SendEvent(dest Window, propagate bool, mask uint32, event [32]byte) error {
	r := newRequest(opSendEvent, uint8(b2u(propagate))).u32(uint32(dest)).u32(mask)
	r.b = append(r.b, event[:]...)
	return c.sendVoid(r)
}

// ClientMessageEventBytes encodes a format-32 ClientMessage for SendEvent.
func ClientMessageEventBytes(w Window, typ Atom, data [5]uint32) [32]byte {
	var e [32]byte
	e[0] = ClientMessage
	e[1] = 32
	le.PutUint32(e[4:], uint32(w))
	le.PutUint32(e[8:], uint32(typ))
	for i, v := range data {
		le.PutUint32(e[12+4*i:], v)
	}
	return e
}

// SendClientMessage sends a format-32 ClientMessage about window w to
// dest (often w itself, or the root for the window manager).
func (c *Conn) SendClientMessage(dest, w Window, typ Atom, mask uint32, data [5]uint32) error {
	return c.SendEvent(dest, false, mask, ClientMessageEventBytes(w, typ, data))
}

// TranslateCoordinates maps (x, y) in src to dst, and names dst's child
// that contains the point.
func (c *Conn) TranslateCoordinates(src, dst Window, x, y int16) (dx, dy int16, child Window, err error) {
	b, err := c.roundTrip(newRequest(opTranslateCoordinates, 0).u32(uint32(src)).u32(uint32(dst)).i16(x).i16(y))
	if err != nil {
		return 0, 0, 0, err
	}
	return int16(le.Uint16(b[12:])), int16(le.Uint16(b[14:])), Window(le.Uint32(b[8:])), nil
}

// Focus revert modes, and the special focus windows.
const (
	RevertToNone        = 0
	RevertToPointerRoot = 1
	RevertToParent      = 2

	PointerRoot = 1
)

// SetInputFocus gives the keyboard focus to w.
func (c *Conn) SetInputFocus(w Window, revertTo uint8, time uint32) error {
	return c.sendVoid(newRequest(opSetInputFocus, revertTo).u32(uint32(w)).u32(time))
}

// GetInputFocus returns the window with the keyboard focus.
func (c *Conn) GetInputFocus() (Window, error) {
	b, err := c.roundTrip(newRequest(opGetInputFocus, 0))
	if err != nil {
		return 0, err
	}
	return Window(le.Uint32(b[8:])), nil
}

// Extension is the reply to QueryExtension.
type Extension struct {
	Present                             bool
	MajorOpcode, FirstEvent, FirstError uint8
}

// QueryExtension asks whether the server has the named extension.
func (c *Conn) QueryExtension(name string) (Extension, error) {
	b, err := c.roundTrip(newRequest(opQueryExtension, 0).u16(uint16(len(name))).pad(2).bytes([]byte(name)))
	if err != nil {
		return Extension{}, err
	}
	return Extension{Present: b[8] != 0, MajorOpcode: b[9], FirstEvent: b[10], FirstError: b[11]}, nil
}

// KillClient disconnects the client that owns resource id (a window of
// it), destroying its windows. A last resort for a hung program.
func (c *Conn) KillClient(resource uint32) error {
	return c.sendVoid(newRequest(opKillClient, 0).u32(resource))
}
