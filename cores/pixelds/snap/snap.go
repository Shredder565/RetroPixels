// Package snap saves a whole running machine (a graph of Go values) to
// bytes and loads it back: PixelDS's 3DS save states. It is RetroPixels'
// own code.
//
// Save walks everything reachable from a root pointer: numbers, strings,
// arrays, structs (unexported fields too), pointers (shared and cyclic ones
// stay shared), slices (slices sharing one array stay sharing it), maps and
// interfaces (their concrete types must be registered with Register).
// Functions and channels are not saved; a field tagged `snap:"-"` isn't
// either. Load writes the saved values into a machine of the same kind,
// normally a freshly made one: its functions and skipped fields stay as
// they are, and its objects are reused where the saved machine had one in
// the same place.
//
// Large data that the machine only reads, such as the game's files, can be
// left out with Options.Extern: slices into it are saved as positions and
// pointed back into the new machine's copy on Load.
package snap

import (
	"bufio"
	"bytes"
	"compress/flate"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"reflect"
	"sort"
	"sync"
	"unsafe"
)

var magic = []byte("PXSNAP1\n")

// Options for Save and Load.
type Options struct {
	// Extern: slices (of any type) whose contents are not saved. Load must
	// be given the new machine's slices in the same order.
	Extern []any
}

var (
	regMu  sync.RWMutex
	byType = map[reflect.Type]string{}
	byName = map[string]reflect.Type{}
)

// Register names the types that can be found in interfaces: pass a value
// (or a nil pointer) of each.
func Register(vals ...any) {
	regMu.Lock()
	defer regMu.Unlock()
	for _, v := range vals {
		t := reflect.TypeOf(v)
		n := typeName(t)
		byType[t] = n
		byName[n] = t
	}
}

func typeName(t reflect.Type) string {
	base := t
	for base.Kind() == reflect.Pointer {
		base = base.Elem()
	}
	return base.PkgPath() + ":" + t.String()
}

var errorType = reflect.TypeOf((*error)(nil)).Elem()

// ---------------- helpers ----------------

// at returns a settable value for v's memory (also for unexported fields).
func at(v reflect.Value) reflect.Value {
	return reflect.NewAt(v.Type(), unsafe.Pointer(v.UnsafeAddr())).Elem()
}

// fieldOf: struct field i of the addressable struct v, settable.
func fieldOf(v reflect.Value, i int) reflect.Value {
	return at(v.Field(i))
}

// copyOf: an addressable, settable copy of v.
func copyOf(v reflect.Value) reflect.Value {
	c := reflect.New(v.Type()).Elem()
	if v.CanInterface() {
		c.Set(v)
	} else {
		reflect.NewAt(v.Type(), unsafe.Pointer(c.UnsafeAddr())).Elem().Set(v)
	}
	return c
}

func skipped(f reflect.StructField) bool { return f.Tag.Get("snap") == "-" }

var (
	flatMu    sync.Mutex
	flatCache = map[reflect.Type]bool{}
)

// flat: values of t are plain bytes (no pointers, strings, slices, maps,
// interfaces, functions or channels), so they are saved as raw memory.
func flat(t reflect.Type) bool {
	flatMu.Lock()
	defer flatMu.Unlock()
	return flatLocked(t)
}

func flatLocked(t reflect.Type) bool {
	if v, ok := flatCache[t]; ok {
		return v
	}
	var r bool
	switch t.Kind() {
	case reflect.Bool, reflect.Int, reflect.Int8, reflect.Int16, reflect.Int32, reflect.Int64,
		reflect.Uint, reflect.Uint8, reflect.Uint16, reflect.Uint32, reflect.Uint64, reflect.Uintptr,
		reflect.Float32, reflect.Float64, reflect.Complex64, reflect.Complex128:
		r = true
	case reflect.Array:
		r = flatLocked(t.Elem())
	case reflect.Struct:
		r = true
		for i := 0; i < t.NumField(); i++ {
			if f := t.Field(i); skipped(f) || !flatLocked(f.Type) {
				r = false
				break
			}
		}
	}
	flatCache[t] = r
	return r
}

// hasRefs: values of t can hold pointers, slices, maps or interfaces, so
// the walk has to look inside them.
func hasRefs(t reflect.Type) bool {
	switch t.Kind() {
	case reflect.Pointer, reflect.Slice, reflect.Map, reflect.Interface:
		return true
	case reflect.Array:
		return t.Len() > 0 && hasRefs(t.Elem())
	case reflect.Struct:
		for i := 0; i < t.NumField(); i++ {
			if f := t.Field(i); !skipped(f) && hasRefs(f.Type) {
				return true
			}
		}
	}
	return false
}

func rawBytes(p unsafe.Pointer, n uintptr) []byte {
	if n == 0 {
		return nil
	}
	return unsafe.Slice((*byte)(p), n)
}

// ---------------- the first pass: what shares memory with what ----------------

type objKey struct {
	addr uintptr
	t    reflect.Type
}

type span struct {
	p          unsafe.Pointer // start, kept as a pointer for the garbage collector
	start, end uintptr
	t          reflect.Type // a pointer's target type, or a slice's element type
	slice      bool
	extern     int // for slices: the Extern index, or -1
}

type region struct {
	p          unsafe.Pointer
	start, end uintptr
	elem       reflect.Type
	extern     int
	externBase uintptr
}

type collector struct {
	seen   map[objKey]bool
	spans  []span
	err    error
	regs   []region
	visits map[uintptr]uintptr // slice arrays already walked: start -> end
}

func (c *collector) fail(format string, a ...any) {
	if c.err == nil {
		c.err = fmt.Errorf("snap: "+format, a...)
	}
}

func (c *collector) walk(v reflect.Value) {
	if c.err != nil {
		return
	}
	t := v.Type()
	switch t.Kind() {
	case reflect.Pointer:
		if v.IsNil() {
			return
		}
		k := objKey{v.Pointer(), t.Elem()}
		if c.seen[k] {
			return
		}
		c.seen[k] = true
		if sz := t.Elem().Size(); sz > 0 {
			c.spans = append(c.spans, span{start: k.addr, end: k.addr + sz, t: t.Elem(), extern: -1})
		}
		c.walk(v.Elem())
	case reflect.Struct:
		for i := 0; i < t.NumField(); i++ {
			if f := t.Field(i); !skipped(f) && hasRefs(f.Type) {
				c.walk(fieldOf(v, i))
			}
		}
	case reflect.Array:
		if hasRefs(t.Elem()) {
			for i := 0; i < v.Len(); i++ {
				c.walk(v.Index(i))
			}
		}
	case reflect.Slice:
		if v.IsNil() || v.Cap() == 0 || t.Elem().Size() == 0 {
			return
		}
		start := v.Pointer()
		end := start + uintptr(v.Cap())*t.Elem().Size()
		c.spans = append(c.spans, span{p: v.UnsafePointer(), start: start, end: end, t: t.Elem(), slice: true, extern: -1})
		if hasRefs(t.Elem()) {
			if e, ok := c.visits[start]; ok && e >= end {
				return
			}
			c.visits[start] = end
			full := v.Slice3(0, v.Cap(), v.Cap())
			for i := 0; i < full.Len(); i++ {
				c.walk(full.Index(i))
			}
		}
	case reflect.Map:
		if v.IsNil() {
			return
		}
		walkK, walkV := hasRefs(t.Key()), hasRefs(t.Elem())
		if !walkK && !walkV {
			return
		}
		it := v.MapRange()
		for it.Next() {
			if walkK {
				c.walk(copyOf(it.Key()))
			}
			if walkV {
				c.walk(copyOf(it.Value()))
			}
		}
	case reflect.Interface:
		if v.IsNil() || t == errorType {
			return
		}
		e := v.Elem()
		regMu.RLock()
		_, ok := byType[e.Type()]
		regMu.RUnlock()
		if !ok {
			c.fail("type %v (in an interface) isn't registered", e.Type())
			return
		}
		c.walk(copyOf(e))
	}
}

// plan merges the slices into the arrays they share and checks that no
// pointer points into the middle of something else.
func (c *collector) plan(externs []reflect.Value) {
	if c.err != nil {
		return
	}
	for i, x := range externs {
		if x.Len() == 0 {
			continue
		}
		s := x.Pointer()
		c.spans = append(c.spans, span{p: x.UnsafePointer(), start: s, end: s + uintptr(x.Cap())*x.Type().Elem().Size(), t: x.Type().Elem(), slice: true, extern: i})
	}
	sort.SliceStable(c.spans, func(i, j int) bool {
		a, b := c.spans[i], c.spans[j]
		if a.start != b.start {
			return a.start < b.start
		}
		return a.end > b.end // the bigger first
	})
	var obj *span // the pointer target that the current position is inside
	for i := range c.spans {
		s := c.spans[i]
		if !s.slice {
			if obj != nil && s.start < obj.end && !(s.start == obj.start && s.t == obj.t) {
				c.fail("a pointer to %v points inside a %v", s.t, obj.t)
				return
			}
			if n := len(c.regs); n > 0 && s.start < c.regs[n-1].end {
				c.fail("a pointer to %v points into a slice's array of %v", s.t, c.regs[n-1].elem)
				return
			}
			if obj == nil || s.end > obj.end {
				obj = &c.spans[i]
			}
			continue
		}
		if obj != nil && s.start < obj.end {
			c.fail("a slice of %v points into a %v", s.t, obj.t)
			return
		}
		if n := len(c.regs); n > 0 && s.start < c.regs[n-1].end {
			r := &c.regs[n-1]
			if r.elem != s.t {
				c.fail("slices of %v and %v share memory", r.elem, s.t)
				return
			}
			if s.extern >= 0 {
				if r.extern >= 0 && r.extern != s.extern {
					c.fail("two Extern slices overlap")
					return
				}
				r.extern, r.externBase = s.extern, s.start
			}
			if s.end > r.end {
				r.end = s.end
			}
			continue
		}
		r := region{p: s.p, start: s.start, end: s.end, elem: s.t, extern: s.extern}
		if s.extern >= 0 {
			r.externBase = s.start
		}
		c.regs = append(c.regs, r)
	}
	for _, r := range c.regs {
		if r.extern >= 0 {
			x := externs[r.extern]
			if r.start < r.externBase || r.end > r.externBase+uintptr(x.Cap())*x.Type().Elem().Size() {
				c.fail("a slice reaches past the end of Extern slice %d", r.extern)
				return
			}
		}
	}
}

// ---------------- writing ----------------

type writer struct {
	w      *out
	c      *collector
	objs   map[objKey]uint64
	regIDs map[int]uint64 // region index -> id in the stream
	nextID uint64
	nextRg uint64
}

// out: a small buffered writer with varints.
type out struct {
	w   io.Writer
	b   []byte
	err error
}

func (b *out) flush() {
	if len(b.b) > 0 && b.err == nil {
		_, b.err = b.w.Write(b.b)
	}
	b.b = b.b[:0]
}

func (b *out) bytes(p []byte) {
	if len(p) > 1<<16 {
		b.flush()
		if b.err == nil {
			_, b.err = b.w.Write(p)
		}
		return
	}
	b.b = append(b.b, p...)
	if len(b.b) > 1<<16 {
		b.flush()
	}
}

func (b *out) uvarint(v uint64) {
	b.b = binary.AppendUvarint(b.b, v)
	if len(b.b) > 1<<16 {
		b.flush()
	}
}

func (b *out) varint(v int64) {
	b.b = binary.AppendVarint(b.b, v)
	if len(b.b) > 1<<16 {
		b.flush()
	}
}

func (b *out) str(s string) {
	b.uvarint(uint64(len(s)))
	b.bytes([]byte(s))
}

func (w *writer) region(p uintptr) (int, bool) {
	rs := w.c.regs
	i := sort.Search(len(rs), func(i int) bool { return rs[i].end > p })
	if i < len(rs) && rs[i].start <= p {
		return i, true
	}
	return 0, false
}

func (w *writer) value(v reflect.Value) error {
	t := v.Type()
	if flat(t) {
		w.w.bytes(rawBytes(unsafe.Pointer(v.UnsafeAddr()), t.Size()))
		return w.w.err
	}
	switch t.Kind() {
	case reflect.String:
		w.w.str(v.String())
	case reflect.Array:
		for i := 0; i < v.Len(); i++ {
			if err := w.value(v.Index(i)); err != nil {
				return err
			}
		}
	case reflect.Struct:
		for i := 0; i < t.NumField(); i++ {
			if skipped(t.Field(i)) {
				continue
			}
			if err := w.value(fieldOf(v, i)); err != nil {
				return err
			}
		}
	case reflect.Pointer:
		if v.IsNil() {
			w.w.uvarint(0)
			return nil
		}
		k := objKey{v.Pointer(), t.Elem()}
		if id, ok := w.objs[k]; ok {
			w.w.uvarint(1)
			w.w.uvarint(id)
			return nil
		}
		w.objs[k] = w.nextID
		w.nextID++
		w.w.uvarint(2)
		return w.value(v.Elem())
	case reflect.Slice:
		if v.IsNil() {
			w.w.uvarint(0)
			return nil
		}
		if v.Cap() == 0 || t.Elem().Size() == 0 {
			w.w.uvarint(1)
			w.w.uvarint(uint64(v.Len()))
			return nil
		}
		ri, ok := w.region(v.Pointer())
		if !ok {
			return fmt.Errorf("snap: a slice of %v wasn't seen before", t.Elem())
		}
		r := w.c.regs[ri]
		size := t.Elem().Size()
		switch {
		case r.extern >= 0:
			w.w.uvarint(2)
			w.w.uvarint(uint64(r.extern))
			w.w.uvarint(uint64((v.Pointer() - r.externBase) / size))
		default:
			id, done := w.regIDs[ri]
			if done {
				w.w.uvarint(4)
				w.w.uvarint(id)
			} else {
				id = w.nextRg
				w.nextRg++
				w.regIDs[ri] = id
				n := int((r.end - r.start) / size)
				w.w.uvarint(3)
				w.w.uvarint(uint64(n))
				arr := reflect.SliceAt(t.Elem(), r.p, n)
				if flat(t.Elem()) {
					w.w.bytes(rawBytes(r.p, uintptr(n)*size))
				} else {
					for i := 0; i < n; i++ {
						if err := w.value(arr.Index(i)); err != nil {
							return err
						}
					}
				}
			}
			w.w.uvarint(uint64((v.Pointer() - r.start) / size))
		}
		w.w.uvarint(uint64(v.Len()))
		w.w.uvarint(uint64(v.Cap()))
	case reflect.Map:
		if v.IsNil() {
			w.w.uvarint(0)
			return nil
		}
		w.w.uvarint(1)
		w.w.uvarint(uint64(v.Len()))
		it := v.MapRange()
		for it.Next() {
			if err := w.value(copyOf(it.Key())); err != nil {
				return err
			}
			if err := w.value(copyOf(it.Value())); err != nil {
				return err
			}
		}
	case reflect.Interface:
		if v.IsNil() {
			w.w.uvarint(0)
			return nil
		}
		if t == errorType {
			w.w.uvarint(1)
			w.w.str(v.Elem().Interface().(error).Error())
			return nil
		}
		e := v.Elem()
		regMu.RLock()
		name := byType[e.Type()]
		regMu.RUnlock()
		w.w.uvarint(2)
		w.w.str(name)
		return w.value(copyOf(e))
	case reflect.Func, reflect.Chan, reflect.UnsafePointer:
		// not saved
	default:
		return fmt.Errorf("snap: can't save a %v", t)
	}
	return w.w.err
}

// Save saves everything reachable from root (a non-nil pointer).
func Save(root any, o Options) (data []byte, err error) {
	defer func() {
		if p := recover(); p != nil {
			data, err = nil, fmt.Errorf("snap: couldn't save: %v", p)
		}
	}()
	rv := reflect.ValueOf(root)
	if rv.Kind() != reflect.Pointer || rv.IsNil() {
		return nil, errors.New("snap: Save needs a non-nil pointer")
	}
	var externs []reflect.Value
	for _, x := range o.Extern {
		xv := reflect.ValueOf(x)
		if xv.Kind() != reflect.Slice {
			return nil, errors.New("snap: Extern holds slices only")
		}
		externs = append(externs, xv)
	}
	c := &collector{seen: map[objKey]bool{}, visits: map[uintptr]uintptr{}}
	c.walk(rv)
	c.plan(externs)
	if c.err != nil {
		return nil, c.err
	}
	var buf bytes.Buffer
	buf.Write(magic)
	fw, _ := flate.NewWriter(&buf, flate.BestSpeed)
	w := &writer{w: &out{w: fw}, c: c, objs: map[objKey]uint64{}, regIDs: map[int]uint64{}}
	if err := w.value(rv); err != nil {
		return nil, err
	}
	w.w.flush()
	if w.w.err != nil {
		return nil, w.w.err
	}
	if err := fw.Close(); err != nil {
		return nil, err
	}
	return buf.Bytes(), nil
}

// ---------------- reading ----------------

type reader struct {
	in      *bufio.Reader
	objs    []reflect.Value // id -> pointer
	claimed map[objKey]bool
	regs    []reflect.Value // id -> the region's whole array, as a slice
	regUsed map[uintptr]bool
	externs []reflect.Value
}

var errCorrupt = errors.New("snap: the saved state is damaged or from another version")

func (r *reader) readFull(p []byte) error {
	if _, err := io.ReadFull(r.in, p); err != nil {
		return errCorrupt
	}
	return nil
}

func (r *reader) uvarint() (uint64, error) {
	v, err := binary.ReadUvarint(r.in)
	if err != nil {
		return 0, errCorrupt
	}
	return v, nil
}

func (r *reader) str() (string, error) {
	n, err := r.uvarint()
	if err != nil || n > 1<<31 {
		return "", errCorrupt
	}
	b := make([]byte, n)
	if err := r.readFull(b); err != nil {
		return "", err
	}
	return string(b), nil
}

// value reads into v (addressable, settable).
func (r *reader) value(v reflect.Value) error {
	t := v.Type()
	if flat(t) {
		return r.readFull(rawBytes(unsafe.Pointer(v.UnsafeAddr()), t.Size()))
	}
	switch t.Kind() {
	case reflect.String:
		s, err := r.str()
		if err != nil {
			return err
		}
		v.SetString(s)
	case reflect.Array:
		for i := 0; i < v.Len(); i++ {
			if err := r.value(v.Index(i)); err != nil {
				return err
			}
		}
	case reflect.Struct:
		for i := 0; i < t.NumField(); i++ {
			if skipped(t.Field(i)) {
				continue
			}
			if err := r.value(fieldOf(v, i)); err != nil {
				return err
			}
		}
	case reflect.Pointer:
		tag, err := r.uvarint()
		if err != nil {
			return err
		}
		switch tag {
		case 0:
			v.SetZero()
		case 1:
			id, err := r.uvarint()
			if err != nil || id >= uint64(len(r.objs)) || r.objs[id].Type() != t {
				return errCorrupt
			}
			v.Set(r.objs[id])
		case 2:
			p := v
			if !p.IsNil() && !r.claimed[objKey{p.Pointer(), t.Elem()}] {
				p = copyOf(p)
			} else {
				p = reflect.New(t.Elem())
			}
			r.claimed[objKey{p.Pointer(), t.Elem()}] = true
			r.objs = append(r.objs, p)
			if err := r.value(p.Elem()); err != nil {
				return err
			}
			v.Set(p)
		default:
			return errCorrupt
		}
	case reflect.Slice:
		tag, err := r.uvarint()
		if err != nil {
			return err
		}
		size := t.Elem().Size()
		var base unsafe.Pointer
		var avail int // elements from base to the end of its array
		switch tag {
		case 0:
			v.SetZero()
			return nil
		case 1:
			n, err := r.uvarint()
			if err != nil || n > 1<<31 {
				return errCorrupt
			}
			v.Set(reflect.MakeSlice(t, int(n), int(n)))
			return nil
		case 2:
			i, err1 := r.uvarint()
			off, err2 := r.uvarint()
			if err1 != nil || err2 != nil || i >= uint64(len(r.externs)) {
				return errCorrupt
			}
			x := r.externs[i]
			if x.Type().Elem() != t.Elem() || off > uint64(x.Cap()) {
				return errCorrupt
			}
			base = unsafe.Add(x.UnsafePointer(), uintptr(off)*size)
			avail = x.Cap() - int(off)
		case 3, 4:
			var arr reflect.Value
			if tag == 3 {
				n, err := r.uvarint()
				if err != nil || n > 1<<34/uint64(max(size, 1)) {
					return errCorrupt
				}
				// reuse the new machine's own array when it is the same size
				if !v.IsNil() && v.Cap() == int(n) && !r.regUsed[v.Pointer()] && flat(t.Elem()) {
					arr = reflect.SliceAt(t.Elem(), v.UnsafePointer(), int(n))
				} else {
					arr = reflect.MakeSlice(t, int(n), int(n))
				}
				r.regUsed[arr.Pointer()] = true
				r.regs = append(r.regs, arr)
				if flat(t.Elem()) {
					if err := r.readFull(rawBytes(arr.UnsafePointer(), uintptr(n)*size)); err != nil {
						return err
					}
				} else {
					for i := 0; i < int(n); i++ {
						if err := r.value(arr.Index(i)); err != nil {
							return err
						}
					}
				}
			} else {
				id, err := r.uvarint()
				if err != nil || id >= uint64(len(r.regs)) {
					return errCorrupt
				}
				arr = r.regs[id]
				if arr.Type().Elem() != t.Elem() {
					return errCorrupt
				}
			}
			off, err := r.uvarint()
			if err != nil || off > uint64(arr.Len()) {
				return errCorrupt
			}
			base = unsafe.Add(arr.UnsafePointer(), uintptr(off)*size)
			avail = arr.Len() - int(off)
		default:
			return errCorrupt
		}
		ln, err1 := r.uvarint()
		cp, err2 := r.uvarint()
		if err1 != nil || err2 != nil || ln > cp || cp > uint64(avail) {
			return errCorrupt
		}
		v.Set(reflect.SliceAt(t.Elem(), base, int(cp)).Slice(0, int(ln)))
	case reflect.Map:
		tag, err := r.uvarint()
		if err != nil {
			return err
		}
		if tag == 0 {
			v.SetZero()
			return nil
		}
		n, err := r.uvarint()
		if err != nil || n > 1<<28 {
			return errCorrupt
		}
		m := reflect.MakeMapWithSize(t, int(n))
		for i := 0; i < int(n); i++ {
			k := reflect.New(t.Key()).Elem()
			e := reflect.New(t.Elem()).Elem()
			if err := r.value(k); err != nil {
				return err
			}
			if err := r.value(e); err != nil {
				return err
			}
			m.SetMapIndex(k, e)
		}
		v.Set(m)
	case reflect.Interface:
		tag, err := r.uvarint()
		if err != nil {
			return err
		}
		switch tag {
		case 0:
			v.SetZero()
		case 1:
			s, err := r.str()
			if err != nil || t != errorType {
				return errCorrupt
			}
			v.Set(reflect.ValueOf(errors.New(s)))
		case 2:
			name, err := r.str()
			if err != nil {
				return err
			}
			regMu.RLock()
			ct, ok := byName[name]
			regMu.RUnlock()
			if !ok || !ct.Implements(t) {
				return fmt.Errorf("snap: unknown type %q in the saved state", name)
			}
			tmp := reflect.New(ct).Elem()
			if !v.IsNil() && v.Elem().Type() == ct && ct.Kind() == reflect.Pointer {
				tmp.Set(v.Elem())
			}
			if err := r.value(tmp); err != nil {
				return err
			}
			v.Set(tmp)
		default:
			return errCorrupt
		}
	case reflect.Func, reflect.Chan, reflect.UnsafePointer:
		// kept as the new machine has them
	default:
		return fmt.Errorf("snap: can't load a %v", t)
	}
	return nil
}

// Load reads a state made by Save into root, a pointer to a machine of the
// same kind (normally a new one). o.Extern gives the new machine's slices
// for those left out when saving, in the same order.
func Load(data []byte, root any, o Options) (err error) {
	defer func() {
		if p := recover(); p != nil {
			err = errCorrupt
		}
	}()
	if !bytes.HasPrefix(data, magic) {
		return errCorrupt
	}
	rv := reflect.ValueOf(root)
	if rv.Kind() != reflect.Pointer || rv.IsNil() {
		return errors.New("snap: Load needs a non-nil pointer")
	}
	r := &reader{in: bufio.NewReaderSize(flate.NewReader(bytes.NewReader(data[len(magic):])), 1<<16), claimed: map[objKey]bool{}, regUsed: map[uintptr]bool{}}
	for _, x := range o.Extern {
		r.externs = append(r.externs, reflect.ValueOf(x))
	}
	// the root itself is reused in place
	slot := reflect.New(rv.Type())
	slot.Elem().Set(rv)
	if err := r.value(slot.Elem()); err != nil {
		return err
	}
	if slot.Elem().Pointer() != rv.Pointer() {
		return errCorrupt
	}
	return nil
}
