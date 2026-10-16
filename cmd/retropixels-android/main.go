// Command retropixels-android is the emulator engine of RetroPixels for
// Android: a headless program, built without cgo for each Android CPU and
// packed into the app as librpengine.so, which the app starts and drives
// over its standard input and output (proto.go). It runs the same cores, and
// opens games the same way (internal/games), as RetroPixels-Native.exe.
//
//	retropixels-android -dir <app files folder>
//	retropixels-android -reference <game> -frames N   (prints what the core makes, for tests)
package main

import (
	"bufio"
	"crypto/sha1"
	"encoding/hex"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"runtime/debug"
	"strings"
	"time"

	"retropixels/internal/apatch"
	"retropixels/internal/games"
	_ "retropixels/internal/games/allcores"
)

// version is set at build time: -ldflags "-X main.version=2.14.0".
var version = "dev"

func main() {
	dir := flag.String("dir", "", "the folder for saves, chip programs and kept Archipelago ROMs")
	ref := flag.String("reference", "", "run this game straight on its core and print hashes of what it makes")
	frames := flag.Int("frames", 120, "frames for -reference")
	rate := flag.Int("rate", 48000, "sample rate for -reference")
	ver := flag.Bool("version", false, "print the version")
	tz := flag.String("tz", "", "the phone's time zone (\"Europe/London\"), for game clocks")
	tzOffset := flag.Int("tzoffset", 0, "the phone's time zone's offset from UTC now, in seconds (when -tz can't be loaded)")
	flag.Parse()
	setTimeZone(*tz, *tzOffset)
	games.ThisBuild = "RetroPixels for Android"
	if *ver {
		fmt.Println(version)
		return
	}
	if *ref != "" {
		if err := reference(*ref, *frames, *rate); err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
		return
	}
	if *dir == "" {
		*dir = "."
	}
	debug.SetGCPercent(400) // fewer collections between frames; the engine's heap is small
	e := &engine{st: games.Store{Dir: *dir}, out: bufio.NewWriterSize(os.Stdout, 1<<20), keys: map[string]bool{}}
	if err := e.serve(bufio.NewReaderSize(os.Stdin, 1<<16)); err != nil {
		fmt.Fprintln(os.Stderr, "engine:", err)
		os.Exit(1)
	}
}

// setTimeZone: games with a clock (Game Boy and Game Boy Advance cartridges)
// read the local time. Android keeps its time zones where Go doesn't look,
// so the app says which zone the phone is in and its offset now.
func setTimeZone(name string, offset int) {
	if name != "" {
		if loc, err := time.LoadLocation(name); err == nil {
			time.Local = loc
			return
		}
	}
	if name != "" || offset != 0 {
		time.Local = time.FixedZone("local", offset)
	}
}

type engine struct {
	st    games.Store
	out   *bufio.Writer
	game  *game
	keys  map[string]bool
	frame []byte
	// the last open, to restart the game after a battery save import
	lastOpen *request
}

// request is any 'J' request; each command uses some of the fields.
type request struct {
	Cmd  string `json:"cmd"`
	Path string `json:"path"`
	openOptions
	Bin    string           `json:"bin"`    // "open": the system for .bin files ("" = guess)
	APBase string           `json:"apBase"` // "open" of an Archipelago patch: the original ROM just chosen
	System string           `json:"system"` // "controls"
	Saved  games.ControlMap `json:"saved"`  // "controls", "setControls"
	Slot   int              `json:"slot"`
	ID     string           `json:"id"`
	Value  int              `json:"value"`
}

type reply map[string]any

func (e *engine) serve(in *bufio.Reader) error {
	if err := writeJSON(e.out, reply{"ok": true, "hello": "RetroPixels engine", "protocol": protocolVersion, "version": version}); err != nil {
		return err
	}
	var buf []byte
	for {
		typ, body, err := readMessage(in, buf)
		buf = body[:0:cap(body)]
		if err != nil {
			if e.game != nil {
				e.game.checkSRAM(true)
			}
			if errors.Is(err, io.EOF) || errors.Is(err, io.ErrUnexpectedEOF) {
				return nil // the app has gone
			}
			return err
		}
		switch typ {
		case msgRun:
			q, err := decodeRun(body, e.keys)
			var f *frameReply
			if err != nil {
				f = &frameReply{failed: true, message: err.Error()}
			} else if e.game == nil {
				f = &frameReply{failed: true, message: "no game is open"}
			} else {
				f = e.game.run(q)
			}
			e.frame = appendFrame(e.frame, f)
			if err := writeMessage(e.out, msgFrame, e.frame); err != nil {
				return err
			}
		case msgJSON:
			var r request
			var rep reply
			if err := json.Unmarshal(body, &r); err != nil {
				rep = fail(fmt.Errorf("bad request: %v", err))
			} else {
				rep = e.handle(&r)
			}
			if err := writeJSON(e.out, rep); err != nil {
				return err
			}
			if r.Cmd == "quit" {
				return nil
			}
		default:
			return fmt.Errorf("unknown message type %q", typ)
		}
	}
}

func fail(err error) reply { return reply{"ok": false, "error": err.Error()} }

func ok(kv ...any) reply {
	r := reply{"ok": true}
	for i := 0; i+1 < len(kv); i += 2 {
		r[kv[i].(string)] = kv[i+1]
	}
	return r
}

func (e *engine) handle(r *request) (rep reply) {
	defer func() {
		if p := recover(); p != nil {
			rep = fail(fmt.Errorf("the emulator stopped (%v)", p))
		}
	}()
	switch r.Cmd {
	case "quit":
		e.closeGame()
		return ok()
	case "systems":
		return e.systems()
	case "controls":
		sys := r.System
		if games.ByID(sys) == nil {
			return fail(fmt.Errorf("no system %q", sys))
		}
		return ok("system", sys, "buttons", nonNil(games.SystemButtons[sys]), "map", games.LoadMap(sys, r.Saved),
			"defaults", games.LoadMap(sys, nil), "note", games.SystemNotes[sys])
	case "open":
		return e.open(r)
	}
	g := e.game
	if g == nil {
		return fail(errors.New("no game is open"))
	}
	switch r.Cmd {
	case "close":
		err := e.closeGame()
		if err != nil {
			return fail(fmt.Errorf("couldn't write the battery save: %v", err))
		}
		return ok()
	case "flush":
		if err := g.checkSRAM(true); err != nil {
			return fail(fmt.Errorf("couldn't write the battery save: %v", err))
		}
		return ok()
	case "reset":
		g.reset()
		return ok()
	case "slots":
		return ok("slots", g.slots())
	case "save":
		if err := g.saveSlot(r.Slot); err != nil {
			return fail(err)
		}
		return ok("slots", g.slots())
	case "load":
		if err := g.loadSlot(r.Slot); err != nil {
			return fail(err)
		}
		return ok("canUndo", g.undo != nil)
	case "undo":
		if err := g.undoLoad(); err != nil {
			return fail(err)
		}
		return ok()
	case "deleteSlot":
		if err := g.st.DeleteSlot(g.g.System, g.g.Name, r.Slot); err != nil {
			return fail(err)
		}
		return ok("slots", g.slots())
	case "importState":
		b, err := readLimited(r.Path, 64<<20)
		if err == nil {
			err = g.importState(b)
		}
		if err != nil {
			return fail(err)
		}
		return ok()
	case "dips":
		return ok("dips", g.dips())
	case "dip":
		if err := g.setDip(r.ID, r.Value); err != nil {
			return fail(err)
		}
		return ok("dips", g.dips())
	case "seat":
		n := g.setSeat(r.Value)
		return ok("seat", n)
	case "setControls":
		g.cmap = games.LoadMap(g.g.System, r.Saved)
		return ok()
	case "exportSRAM":
		sr := g.core.SRAM()
		if sr == nil {
			return fail(errors.New("this game doesn't keep a battery save"))
		}
		if err := games.WriteFileAtomic(r.Path, sr); err != nil {
			return fail(err)
		}
		return ok("size", len(sr))
	case "importSRAM":
		return e.importSRAM(r)
	}
	return fail(fmt.Errorf("unknown command %q", r.Cmd))
}

func nonNil(s []string) []string {
	if s == nil {
		return []string{}
	}
	return s
}

func (e *engine) systems() reply {
	type sys struct {
		ID    string   `json:"id"`
		Label string   `json:"label"`
		Exts  []string `json:"exts"`
	}
	var list []sys
	for _, s := range games.Systems {
		if games.Available(s.ID) {
			list = append(list, sys{s.ID, s.Label, nonNil(s.Exts)})
		}
	}
	return ok("systems", list, "exts", games.AvailableExts(), "labels", games.AvailableLabels(),
		"arcade", games.Arcade.Supported())
}

func (e *engine) closeGame() error {
	g := e.game
	if g == nil {
		return nil
	}
	e.game = nil
	return g.checkSRAM(true)
}

// open opens a game. The reply describes it, or says what's missing: "need":
// "firmware" (a cartridge chip's program: choose it and open again with
// "firmware": {chip: file}) or "apbase" (an Archipelago patch's original
// ROM: open again with "apBase").
func (e *engine) open(r *request) reply {
	e.closeGame()
	g, rep := e.load(r)
	if rep != nil {
		return rep
	}
	r.firmware = map[string][]byte{}
	for chip, p := range r.Firmware {
		b, err := readLimited(p, 16<<20)
		if err != nil {
			return fail(fmt.Errorf("couldn't read the chip program: %v", err))
		}
		r.firmware[chip] = b
	}
	s, err := newGame(g, e.st, r.openOptions)
	if err != nil {
		if chip, file, msg, isFW := games.NeedsFirmware(err); isFW {
			return reply{"ok": false, "error": msg, "need": "firmware", "chip": chip, "file": file}
		}
		var ny games.NotYetError
		if errors.As(err, &ny) {
			return fail(errors.New(ny.What + " not in RetroPixels for Android."))
		}
		return fail(err)
	}
	for chip, b := range r.firmware {
		if err := e.st.SaveFirmware(chip, b); err != nil {
			s.message = "Couldn't keep the chip program: " + err.Error()
		}
	}
	e.game = s
	cp := *r
	cp.Firmware, cp.firmware, cp.APBase = nil, nil, ""
	e.lastOpen = &cp
	return e.describe(s)
}

func (e *engine) describe(s *game) reply {
	label := s.g.System
	if sys := games.ByID(s.g.System); sys != nil {
		label = sys.Label
	}
	_, rumble := s.core.(interface{ Rumble() float32 })
	_, analog := s.core.(interface {
		SetAnalog(player, axis int, v float32)
	})
	return ok("system", s.g.System, "label", label, "name", s.g.Name, "buttons", nonNil(s.info.Buttons),
		"players", s.info.Players, "stereo", s.info.Stereo, "aspect", s.aspect, "fps", s.fps, "sampleRate", s.rate,
		"width", s.core.Width(), "height", s.core.Height(), "hasSRAM", s.core.SRAM() != nil,
		"analogStick", s.analogStick, "rightStick", s.rightStick, "analog", analog, "rumble", rumble,
		"note", s.note, "dips", s.dips(), "seats", s.seats(), "seatNames", nonNil(s.seatNames()), "seat", s.seat,
		"slots", s.slots(), "map", s.cmap)
}

// load reads the game file (or makes the game from an Archipelago patch).
func (e *engine) load(r *request) (*games.Game, reply) {
	if apatch.IsPatchName(filepath.Base(r.Path)) {
		return e.loadPatch(r)
	}
	opts := games.OpenOptions{}
	if r.Bin != "" && games.Available(r.Bin) {
		bin := r.Bin
		opts.ForBin = func([]byte) string { return bin }
	}
	g, err := games.Open(r.Path, opts)
	if err != nil {
		var ny games.NotYetError
		if errors.As(err, &ny) {
			if strings.HasPrefix(ny.What, "GameCube") {
				return nil, fail(errors.New("GameCube and Wii games aren't played by RetroPixels for Android. " +
					"Dolphin has its own Android app for them."))
			}
			return nil, fail(errors.New(ny.What + " not in RetroPixels for Android."))
		}
		return nil, fail(err)
	}
	return g, nil
}

// apBasePath: where the original ROM for a game's Archipelago patches is kept.
func (e *engine) apBasePath(p *apatch.Patch) string {
	h := sha1.Sum([]byte(p.BaseKey()))
	return filepath.Join(e.st.Dir, "archipelago", "base-"+hex.EncodeToString(h[:8]))
}

func (e *engine) loadPatch(r *request) (*games.Game, reply) {
	name := filepath.Base(r.Path)
	data, err := readLimited(r.Path, apatch.MaxPatch)
	if err != nil {
		return nil, fail(fmt.Errorf("couldn't open %q: %v", name, err))
	}
	p, err := apatch.Read(data, name)
	if err != nil {
		return nil, fail(fmt.Errorf("couldn't open %q: %v", name, err))
	}
	game := p.Game
	if game == "" {
		game = "this game"
	}
	if p.Unsupported != "" || games.ByID(p.System) == nil || !games.Available(p.System) {
		why := p.Unsupported
		if why == "" {
			why = "its console isn't one RetroPixels plays"
		}
		return nil, fail(fmt.Errorf("RetroPixels can't make the %s ROM from %q by itself (%s).", game, name, why))
	}
	kept := e.apBasePath(p)
	basePath, chosen := r.APBase, r.APBase != ""
	if !chosen {
		basePath = kept
	}
	ask := func(why string) reply {
		who := ""
		if p.PlayerName != "" {
			who = " (player " + p.PlayerName + ")"
		}
		text := fmt.Sprintf("%s is an Archipelago patch for %s%s.\n\nChoose your %s ROM — the original one Archipelago's setup guide "+
			"for this game asks for. RetroPixels keeps it for this game's next patch.", name, game, who, game)
		if why != "" {
			text = why + "\n\n" + text
		}
		return reply{"ok": false, "error": text, "need": "apbase", "game": game, "system": p.System}
	}
	base, err := readLimited(basePath, games.MaxROM)
	if err != nil {
		if !chosen {
			return nil, ask("")
		}
		return nil, fail(fmt.Errorf("couldn't read the ROM: %v", err))
	}
	unz, err := apatch.UnzipBase(base)
	if err != nil {
		return nil, ask(err.Error())
	}
	rom, err := p.Apply(unz)
	if err != nil {
		var wb *apatch.WrongBaseError
		if errors.As(err, &wb) {
			if !chosen {
				os.Remove(kept)
			}
			return nil, ask(err.Error())
		}
		return nil, fail(err)
	}
	if chosen {
		os.MkdirAll(filepath.Dir(kept), 0o755)
		games.WriteFileAtomic(kept, base)
	}
	return &games.Game{System: p.System, ROM: rom, Name: games.GameNameOf(r.Path), Path: r.Path}, nil
}

// importSRAM puts a battery save in place of the game's and starts the
// game again with it.
func (e *engine) importSRAM(r *request) reply {
	g := e.game
	b, err := readLimited(r.Path, games.MaxSRAMImport)
	if err != nil {
		return fail(err)
	}
	if g.core.SRAM() == nil {
		return fail(errors.New("this game doesn't keep a battery save"))
	}
	e.game = nil // (closed without writing its own battery save back)
	if err := e.st.ImportSRAM(g.g.System, g.g.Name, b); err != nil {
		e.game = g
		return fail(err)
	}
	if e.lastOpen == nil {
		return fail(errors.New("the game can't be started again"))
	}
	o := *e.lastOpen
	o.Seat = g.seat
	return e.open(&o)
}

func readLimited(path string, limit int64) ([]byte, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	st, err := f.Stat()
	if err != nil {
		return nil, err
	}
	if st.Size() > limit {
		return nil, errors.New("it's far bigger than it should be")
	}
	b := make([]byte, st.Size())
	if _, err := io.ReadFull(f, b); err != nil {
		return nil, err
	}
	return b, nil
}
