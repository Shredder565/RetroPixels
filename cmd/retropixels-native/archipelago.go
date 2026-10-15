package main

// Archipelago (archipelago.gg) multiworld randomizers, as in RetroPixels.exe:
// an Archipelago patch file (.apsm, .apred, .apemerald...) is made into the
// game with the original ROM it needs (asked for once, checked, remembered per
// game), and while a game runs Archipelago's own clients reach its memory:
// the BizHawk Client on 127.0.0.1:43055-43059 and, for Super Nintendo games,
// the SNI Client on port 23074 (internal/apbridge). Their reads and writes
// happen on the game's thread, between two frames (session.apRun).
//
// The window's side (dialogs, menu, title) is in archipelago_windows.go.

import (
	"crypto/sha1"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"time"

	"retropixels/core"
	"retropixels/internal/apatch"
	"retropixels/internal/apbridge"
)

// apSettings: what settings.json keeps for Archipelago.
type apSettings struct {
	// the original ROM each game's patches are made from, by apatch.Patch.BaseKey
	APBases map[string]string `json:"archipelagoBaseROMs,omitempty"`
}

func (s *apSettings) apBase(p *apatch.Patch) string { return s.APBases[p.BaseKey()] }

func (s *apSettings) setAPBase(p *apatch.Patch, path string) {
	if s.APBases == nil {
		s.APBases = map[string]string{}
	}
	if path == "" {
		delete(s.APBases, p.BaseKey())
	} else {
		s.APBases[p.BaseKey()] = path
	}
}

var errAPPatchFile = errors.New("that's an Archipelago patch file")

// readAPPatchFile reads a patch file and checks RetroPixels Native can make
// its game (the messages are the web page's).
func readAPPatchFile(path string) (*apatch.Patch, error) {
	name := filepath.Base(path)
	data, err := readLimited(path, apatch.MaxPatch)
	if err != nil {
		return nil, fmt.Errorf("couldn't open %q: %v", name, err)
	}
	p, err := apatch.Read(data, name)
	if err != nil {
		return nil, fmt.Errorf("couldn't open %q: %v", name, err)
	}
	game := apGameName(p)
	if p.Unsupported != "" || systemByID(p.System) == nil {
		why := p.Unsupported
		if why == "" {
			why = "its console isn't one RetroPixels plays"
		}
		return nil, fmt.Errorf("RetroPixels can't make the %s ROM from %q by itself (%s). "+
			"Open the patch with the Archipelago Launcher (\"Open Patch\"), which makes the ROM, then open that ROM here — "+
			"the Archipelago connection works the same way.", game, name, why)
	}
	if !available(p.System) {
		return nil, errNotYet{fmt.Sprintf("%q is an Archipelago patch for %s, a %s game. %s games are", name, game,
			systemByID(p.System).Label, systemByID(p.System).Label)}
	}
	return p, nil
}

func readLimited(path string, limit int64) ([]byte, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	b, err := io.ReadAll(io.LimitReader(f, limit+1))
	if err != nil {
		return nil, err
	}
	if int64(len(b)) > limit {
		return nil, errors.New("it's far bigger than it should be")
	}
	return b, nil
}

func apGameName(p *apatch.Patch) string {
	if p.Game == "" {
		return "this game"
	}
	return p.Game
}

// apAskText: the question before choosing the original ROM.
func apAskText(p *apatch.Patch, patchPath string) string {
	game := apGameName(p)
	who := ""
	if p.PlayerName != "" {
		who = " (player " + p.PlayerName + ")"
	}
	return fmt.Sprintf("%s is an Archipelago patch for %s%s.\n\nChoose your %s ROM — the original one Archipelago's setup guide "+
		"for this game asks for. RetroPixels keeps it for this game's next patch.", filepath.Base(patchPath), game, who, game)
}

// apMakeGame makes the game from a patch and the original ROM (a zipped ROM
// works too). The game is named after the patch file, so its saves are its
// own, apart from the game without the patch.
func apMakeGame(p *apatch.Patch, patchPath string, base []byte) (*loadedGame, error) {
	base, err := apatch.UnzipBase(base)
	if err != nil {
		return nil, err
	}
	rom, err := p.Apply(base)
	if err != nil {
		return nil, err
	}
	return &loadedGame{System: p.System, ROM: rom, Name: gameNameOf(patchPath), Path: patchPath, AP: p}, nil
}

// apWrongBase: the ROM chosen isn't the one the patch needs.
func apWrongBase(err error) bool {
	var wb *apatch.WrongBaseError
	return errors.As(err, &wb)
}

// apBaseExts: the files offered when choosing the original ROM.
func apBaseExts(sys string) []string {
	var out []string
	if s := systemByID(sys); s != nil {
		for _, e := range s.Exts {
			out = append(out, "."+e)
		}
	}
	if sys == "segaMD" || sys == "sms" {
		out = append(out, ".bin")
	}
	return append(out, ".zip")
}

// ---------------- the running game ----------------

var apNamed = regexp.MustCompile(`(?i)^AP[_-]`)

// apGameFor: what the clients are told about a game, or nil when its core's
// memory can't be reached (or Archipelago has no client for the system).
func apGameFor(g *loadedGame, c core.Core) *apbridge.Game {
	sys := apbridge.SystemFor[g.System]
	if _, ok := c.(core.Memory); !ok || sys == "" {
		return nil
	}
	if gb, ok := c.(interface{ IsCGB() bool }); ok && gb.IsCGB() {
		sys = "GBC"
	}
	title := g.Name
	if g.AP != nil && g.AP.Game != "" {
		title = g.AP.Game
	}
	return &apbridge.Game{Core: g.System, System: sys, Title: title, Hash: romHash(g.System, g.ROM)}
}

// romHash: the ROM's SHA-1 in upper-case hex, as BizHawk gives it (an iNES
// file's leaves out its 16-byte header and trainer).
func romHash(sys string, rom []byte) string {
	if sys == "nes" && len(rom) > 16 && string(rom[:4]) == "NES\x1a" {
		skip := 16
		if rom[6]&4 != 0 {
			skip += 512
		}
		if skip < len(rom) {
			rom = rom[skip:]
		}
	}
	h := sha1.Sum(rom)
	return strings.ToUpper(hex.EncodeToString(h[:]))
}

// apTarget carries out the clients' batches on a session's game thread.
type apTarget struct{ s *session }

// apHold: LOCK holds the game still (no frames) until UNLOCK, for at most
// 2 seconds, like BizHawk's connector script.
const apHold = 2 * time.Second

func (t apTarget) Run(ops []apbridge.Op) ([]apbridge.Res, error) { return t.s.apRun(ops) }

// apRun runs a batch between two frames, on the game's thread.
func (s *session) apRun(ops []apbridge.Op) ([]apbridge.Res, error) {
	mem, ok := s.core.(core.Memory)
	if !ok {
		return nil, apbridge.ErrNoGame
	}
	var res []apbridge.Res
	done := make(chan struct{})
	go func() {
		defer close(done)
		_ = s.call(func() error {
			res = apbridge.Exec(mem, ops, func(o apbridge.Op) {
				switch o.T {
				case "msg":
					if o.Text != "" {
						s.host.event("ap-msg", o.Text)
					}
				case "lock":
					s.apHoldUntil = time.Now().Add(apHold)
				case "unlock":
					s.apHoldUntil = time.Time{}
				}
			})
			return nil
		})
	}()
	select {
	case <-done:
	case <-time.After(apbridge.CallTimeout):
		return nil, errors.New("the game didn't answer in time")
	}
	if res == nil {
		return nil, errors.New("the game has stopped")
	}
	return res, nil
}

// apHeld: a client has the game held still (game thread only).
func (s *session) apHeld(now time.Time) bool {
	return !s.apHoldUntil.IsZero() && now.Before(s.apHoldUntil)
}

// apShown: whether the window shows the Archipelago status for a game: one
// made from a patch, a ROM named the way Archipelago names them, or any game
// a client has connected to (as the web page's badge).
func apShown(g *loadedGame, st apbridge.Status) bool {
	return g.AP != nil || apNamed.MatchString(g.Name) || len(st.Clients) > 0
}

// apStatusText: "Archipelago: BizHawk Client connected" and the like.
func apStatusText(sys string, st apbridge.Status) string {
	client := apbridge.ClientFor(sys)
	switch {
	case len(st.Clients) > 0:
		return "Archipelago: " + strings.Join(st.Clients, " + ") + " connected"
	case apPortProblem(sys, st) != "":
		return "Archipelago: can't connect"
	}
	return "Archipelago: waiting for the " + client
}

func apPortProblem(sys string, st apbridge.Status) string {
	if apbridge.ClientFor(sys) == apbridge.SNIClient {
		return st.SNIErr
	}
	return st.BizHawkErr
}

// apHelpText: Help > Archipelago, for the running game's system ("" = none).
func apHelpText(g *loadedGame, st apbridge.Status, served bool) string {
	var b strings.Builder
	sys := ""
	if g != nil {
		sys = g.System
	}
	if g != nil && g.AP != nil {
		fmt.Fprintf(&b, "%s", apGameName(g.AP))
		if g.AP.PlayerName != "" {
			fmt.Fprintf(&b, " · player %s", g.AP.PlayerName)
		}
		if g.AP.Server != "" {
			fmt.Fprintf(&b, " · server %s", g.AP.Server)
		}
		b.WriteString("\n\n")
	}
	if g != nil && served {
		b.WriteString(apStatusText(sys, st) + "\n\n")
	}
	if g == nil || !served {
		b.WriteString("RetroPixels Native works with Archipelago multiworld randomizers. Open your patch file " +
			"(for example AP_..._P1_Name.apred, .apemerald) with File > Open game or drop it on the window. " +
			"The first time, RetroPixels asks for the original ROM the game's Archipelago setup guide names " +
			"and keeps it for the next patch.\n\n" +
			"While the game runs, start the BizHawk Client (Game Boy / Color, Game Boy Advance, NES, Nintendo 64) " +
			"or the SNI Client (Super Nintendo) in the Archipelago Launcher. ")
		if g != nil && !served {
			b.WriteString("\n\nThe game running now isn't one Archipelago's clients can connect to.")
		}
		b.WriteString("\n\nRetroPixels answers the clients only on this computer (127.0.0.1), and never connects to the Archipelago server itself.")
		return b.String()
	}
	client := apbridge.ClientFor(sys)
	notNeeded := "BizHawk or its Lua script"
	if client == apbridge.SNIClient {
		notNeeded = "SNI"
	}
	fmt.Fprintf(&b, "1. Open the Archipelago Launcher and start the %s. It finds RetroPixels by itself — you don't need %s.\n", client, notNeeded)
	b.WriteString("2. In the client, connect to the multiworld server (for example /connect archipelago.gg:38281) and enter your player name if it asks.\n")
	b.WriteString("3. Keep playing here. Items you find are sent, and items from others arrive in the game.\n")
	if p := apPortProblem(sys, st); p != "" {
		fmt.Fprintf(&b, "\nRetroPixels can't take the %s's address: %s. Close that program and RetroPixels takes the address within a few seconds.\n", client, p)
	}
	b.WriteString("\nRetroPixels answers the client only on this computer (127.0.0.1). Its save files and save states for this game " +
		"are kept apart from the same game without the patch.")
	return b.String()
}
