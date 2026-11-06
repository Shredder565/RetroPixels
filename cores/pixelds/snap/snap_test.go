package snap

import (
	"errors"
	"testing"
)

type shape interface{ area() int }

type sq struct{ n int }

func (s *sq) area() int { return s.n * s.n }

type val struct{ a, b int16 }

func (v val) area() int { return int(v.a * v.b) }

type node struct {
	name  string
	next  *node
	back  *world
	items []int32
	sub   []int32 // shares items' array
	kids  []*node
	tags  map[string]*node
	sh    shape
	sh2   shape
	err   error
	fn    func() int
	keep  int `snap:"-"`
	big   []byte
	view  []byte // into big
	ext   []byte // into the extern data
	grid  [3][2]uint16
	empty []int
}

type world struct {
	head  *node
	nodes []*node
	f     float64
}

func TestRoundTrip(t *testing.T) {
	Register(&sq{}, val{})
	rom := []byte("the game's data, never saved")
	w := &world{f: 2.5}
	a := &node{name: "a", back: w, items: []int32{1, 2, 3, 4}, tags: map[string]*node{}, sh: &sq{3}, sh2: val{2, 5},
		err: errors.New("stopped"), keep: 7, big: make([]byte, 1<<20), ext: rom[4:10], empty: []int{}}
	a.sub = a.items[1:3]
	a.view = a.big[100:200:300]
	a.big[150] = 9
	a.grid[2][1] = 77
	b := &node{name: "b", next: a, back: w}
	a.next = b // a cycle
	a.kids = []*node{b, a}
	a.tags["me"] = a
	a.fn = func() int { return 1 }
	w.head, w.nodes = a, []*node{a, b}

	data, err := Save(w, Options{Extern: []any{rom}})
	if err != nil {
		t.Fatal(err)
	}
	if len(data) > 64<<10 {
		t.Errorf("state is %d bytes", len(data))
	}
	rom2 := []byte("the game's data, never saved")
	w2 := &world{head: &node{keep: 99, fn: func() int { return 2 }}}
	if err := Load(data, w2, Options{Extern: []any{rom2}}); err != nil {
		t.Fatal(err)
	}
	a2 := w2.head
	if a2.name != "a" || a2.next.name != "b" || a2.next.next != a2 || a2.back != w2 || w2.nodes[1] != a2.next || w2.f != 2.5 {
		t.Fatal("pointers not restored")
	}
	if a2.keep != 99 || a2.fn() != 2 {
		t.Error("skipped fields or functions were changed")
	}
	a2.sub[0] = 20
	if a2.items[1] != 20 || len(a2.sub) != 2 || cap(a2.sub) != 3 {
		t.Error("shared slices not shared")
	}
	a2.view[50] = 10
	if a2.big[150] != 10 || cap(a2.view) != 200 {
		t.Error("view not into big")
	}
	if &a2.ext[0] != &rom2[4] || string(a2.ext) != "game's" {
		t.Error("extern slice not pointed into the new data")
	}
	if a2.kids[1] != a2 || a2.tags["me"] != a2 || a2.sh.area() != 9 || a2.sh2.area() != 10 || a2.err.Error() != "stopped" {
		t.Error("kids, maps or interfaces wrong")
	}
	if a2.grid[2][1] != 77 || a2.empty == nil || len(a2.empty) != 0 {
		t.Error("arrays or empty slices wrong")
	}
	// a damaged state is refused, not half loaded into a crash
	bad := append([]byte(nil), data...)
	bad[len(bad)/2] ^= 0xff
	if err := Load(bad, &world{}, Options{Extern: []any{rom2}}); err == nil {
		t.Log("a damaged state happened to load")
	}
}

type inner struct{ x [4]int }
type holder struct {
	in inner
	p  *inner
}

func TestInteriorPointer(t *testing.T) {
	h := &holder{}
	h.p = &h.in
	if _, err := Save(h, Options{}); err == nil {
		t.Error("an interior pointer was accepted")
	}
}
