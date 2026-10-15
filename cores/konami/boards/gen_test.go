package boards

// Tests that need no ROM sets: generated sets shaped like the real ones
// (every chip file the game lists, by name and size, filled with
// pseudo-random bytes and a reset vector into the program ROM), so the
// loader accepts them and the processors run whatever the bytes say. They
// are compared with the JS boards in Node (when node is installed), and
// used for the save-state and speed checks.

import (
	"archive/zip"
	"bytes"
	"encoding/binary"
	"hash/fnv"
	"os"
	"os/exec"
	"path/filepath"
	"slices"
	"testing"
	"time"

	"retropixels/core"
	"retropixels/cores/arcade"
)

func fill(b []byte, seed uint64) {
	s := seed | 1
	for i := range b {
		s ^= s << 13
		s ^= s >> 7
		s ^= s << 17
		b[i] = byte(s >> 24)
	}
}

// genSet makes a ROM set for one game: name -> bytes. An even seed makes the
// processors run "poker" programs instead of random bytes (see poke_test.go).
func genSet(t testing.TB, board, id string, seed uint64) map[string][]byte {
	var game *arcade.Game
	for _, e := range arcade.AllGames() {
		if e.Board.ID == board && e.Game.ID == id {
			game = e.Game
		}
	}
	if game == nil {
		t.Fatalf("no game %s/%s", board, id)
	}
	set := map[string][]byte{}
	for i, f := range game.Files {
		if f.Optional {
			continue
		}
		b := make([]byte, f.Size)
		fill(b, seed+uint64(i)*7919)
		set[f.Name] = b
	}
	if seed%2 == 0 {
		pokeSet(set, game, board, seed)
		return set
	}
	switch board {
	case "tmnt": // 68000 vectors: stack at the top of work RAM, start at 0x400
		be := func(name string) []byte { return set[name] }
		for _, f := range game.Files {
			if f.Role == "cpu_even" {
				copy(be(f.Name), []byte{0x00, 0x40, 0x00, 0x04}) // SP 0x064000, PC 0x400 (high bytes)
			}
			if f.Role == "cpu_odd" {
				copy(be(f.Name), []byte{0x06, 0x00, 0x00, 0x00})
			}
		}
	case "tmnt2":
		for _, f := range game.Files {
			if f.Role == "cpu" && f.Offset == 0 {
				if f.Lane == 0 {
					copy(set[f.Name], []byte{0x00, 0x80, 0x00, 0x04}) // SP 0x108000, PC 0x400
				} else {
					copy(set[f.Name], []byte{0x10, 0x00, 0x00, 0x00})
				}
			}
		}
	}
	return set
}

func writeZip(t testing.TB, dir, name string, set map[string][]byte) string {
	var buf bytes.Buffer
	zw := zip.NewWriter(&buf)
	names := make([]string, 0, len(set))
	for n := range set {
		names = append(names, n)
	}
	slices.Sort(names)
	for _, n := range names {
		w, err := zw.CreateHeader(&zip.FileHeader{Name: n, Method: zip.Store})
		if err != nil {
			t.Fatal(err)
		}
		w.Write(set[n])
	}
	zw.Close()
	p := filepath.Join(dir, name+".zip")
	if err := os.WriteFile(p, buf.Bytes(), 0o644); err != nil {
		t.Fatal(err)
	}
	return p
}

var genGames = []struct{ board, id string }{
	{"tmnt", "tmnt"}, {"tmnt", "tmht2p"}, {"tmnt2", "tmnt2"}, {"tmnt2", "tmnt22pu"}, {"simpsons", "simpsons"}, {"simpsons", "simpsons2p"},
}

func TestGeneratedSetsMatchJS(t *testing.T) {
	node, err := exec.LookPath("node")
	if err != nil {
		t.Skip("node not found")
	}
	frames := 240
	if testing.Short() {
		frames = 60
	}
	dir := t.TempDir()
	for _, g := range genGames {
		for _, seed := range []uint64{1, 2, 4} {
			name := g.id
			t.Run(name+"-"+string(rune('0'+seed)), func(t *testing.T) {
				zp := writeZip(t, filepath.Join(dir), name, genSet(t, g.board, g.id, seed*1000002+seed%2))
				res := compareRun(t, node, zp, nil, frames, seed)
				t.Logf("%s: %d frames identical (%d different pictures), %d samples (peak %.3f), max diff %g", res.desc, res.frames, res.distinct, res.samples, res.peak, res.maxDiff)
			})
		}
	}
}

func openSet(t testing.TB, set map[string][]byte, name string, sram []byte) *arcade.Core {
	var names []string
	var sizes []int
	var data [][]byte
	for n, b := range set {
		names, sizes, data = append(names, n), append(sizes, len(b)), append(data, b)
	}
	c, err := arcade.NewFromFiles(names, sizes, data, name+".zip", core.Options{SampleRate: 48000, SRAM: sram})
	if err != nil {
		t.Fatal(err)
	}
	return c
}

// realOrGenerated: the real sets when RP_KONAMI_ROMS names them, else generated ones.
func realOrGenerated(t testing.TB) map[string]map[string][]byte {
	out := map[string]map[string][]byte{}
	if dir := os.Getenv("RP_KONAMI_ROMS"); dir != "" {
		for _, id := range []string{"tmnt", "tmnt2", "simpsons"} {
			data, err := os.ReadFile(filepath.Join(dir, id+".zip"))
			if err != nil {
				continue
			}
			names, _, contents, err := arcade.ReadZip(data)
			if err != nil {
				t.Fatal(err)
			}
			set := map[string][]byte{}
			for i, n := range names {
				set[n] = contents[i]
			}
			out[id] = set
		}
	}
	for _, id := range []string{"tmnt", "tmnt2", "simpsons"} {
		if out[id] == nil {
			out[id] = genSet(t, id, id, 78)
		}
	}
	return out
}

// A state saved mid-game and loaded into a newly made core carries on
// exactly as the game would have.
func TestSaveStateRoundTrip(t *testing.T) {
	for id, set := range realOrGenerated(t) {
		t.Run(id, func(t *testing.T) {
			a := openSet(t, set, id, nil)
			ins := script(fnv.New64a().Sum64()+uint64(len(id)), 1400, a.Info().Buttons)
			step := func(c *arcade.Core, in frameInput) ([]uint32, []float32) {
				for p, v := range in.pads {
					c.SetInput(p, v)
				}
				c.RunFrame()
				return c.Screen(), c.TakeAudio()
			}
			for f := 0; f < 1000; f++ {
				step(a, ins[f])
			}
			st, err := a.SaveState()
			if err != nil {
				t.Fatal(err)
			}
			b := openSet(t, set, id, nil)
			if err := b.LoadState(st); err != nil {
				t.Fatal(err)
			}
			// (the picture straight after loading is drawn again from the
			// state, as the web cores do, so it can differ from the one drawn
			// mid-frame; the state itself must be the same)
			if st1, _ := b.SaveState(); !bytes.Equal(st, st1) {
				t.Fatal("the loaded state saves differently")
			}
			for f := 1000; f < 1400; f++ {
				pa, sa := step(a, ins[f])
				pb, sb := step(b, ins[f])
				if !slices.Equal(pa, pb) || !slices.Equal(sa, sb) {
					t.Fatalf("frame %d differs after loading the state", f)
				}
			}
			if !bytes.Equal(a.SRAM(), b.SRAM()) {
				t.Fatal("EEPROM differs")
			}
			st2, _ := a.SaveState()
			st3, _ := b.SaveState()
			if !bytes.Equal(st2, st3) {
				t.Fatal("states differ after 400 more frames")
			}
			// damaged and foreign states are refused
			if b.LoadState(st[:len(st)/2]) == nil {
				t.Error("a cut-off state was accepted")
			}
			bad := append([]byte{}, st...)
			binary.LittleEndian.PutUint32(bad[6:], 3) // the game id's length
			if b.LoadState(bad) == nil {
				t.Error("a state for another game was accepted")
			}
		})
	}
}

// Every listed version of each game is found by its zip name and runs.
func TestEveryVersionLoads(t *testing.T) {
	for _, e := range arcade.AllGames() {
		switch e.Board.ID {
		case "tmnt", "tmnt2", "simpsons":
		default:
			continue
		}
		c := openSet(t, genSet(t, e.Board.ID, e.Game.ID, 5), e.Game.ID, nil)
		if c.GameID() != e.Game.ID {
			t.Errorf("%s opened as %s", e.Game.ID, c.GameID())
		}
		want := e.Game.Players
		if want == 0 {
			want = e.Board.Players
		}
		if c.Players() != want {
			t.Errorf("%s: %d players, want %d", e.Game.ID, c.Players(), want)
		}
		if (want == 4) != (len(c.Characters()) == 4) {
			t.Errorf("%s: characters %v", e.Game.ID, c.Characters())
		}
		for i := 0; i < 3; i++ {
			c.RunFrame()
		}
		if c.Width() != e.Board.Width || c.Height() != e.Board.Height || len(c.Screen()) != c.Width()*c.Height() {
			t.Errorf("%s: picture %dx%d", e.Game.ID, c.Width(), c.Height())
		}
	}
}

// A frame takes well under 16 ms on one core.
func BenchmarkFrame(b *testing.B) {
	for id, set := range realOrGenerated(b) {
		b.Run(id, func(b *testing.B) {
			c := openSet(b, set, id, nil)
			// into the game first (coins, starts, all players moving about)
			ins := script(7, 2400+b.N, c.Info().Buttons)
			for i := range ins {
				ins[i].reset = false
			}
			run := func(in frameInput) {
				for p, v := range in.pads {
					c.SetInput(p, v)
				}
				c.RunFrame()
				c.TakeAudio()
			}
			for i := 0; i < 2400; i++ {
				run(ins[i])
			}
			b.ResetTimer()
			start := time.Now()
			for i := 0; i < b.N; i++ {
				run(ins[2400+i])
			}
			b.ReportMetric(float64(time.Since(start).Microseconds())/1000/float64(b.N), "ms/frame")
		})
	}
}
