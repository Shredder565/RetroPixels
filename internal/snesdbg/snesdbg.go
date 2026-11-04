// Package snesdbg connects a running SNES game to the RetroPixels SNES
// Debugger (tools/snes-debugger), a separate program that opens alongside
// RetroPixels when a SNES game starts.
//
// They talk through files in one folder (Base), so RetroPixels.exe (whose
// SNES emulator runs in the browser window) and RetroPixels Native work the
// same way:
//
//	active.json           the game playing now: {"session": "<folder>", ...}
//	                      ("session" is "" when no SNES game is playing)
//	debugger.json         the debugger's heartbeat: {"pid": n, "alive": unix ms}
//	sessions/<name>/      one per game started:
//	  session.json        the game's name and title, the ROM's SHA-1
//	  rom.sfc             the ROM as the emulator runs it (no copier header)
//	  requests/<id>.req   the debugger asks for a capture
//	  captures/<id>/      RetroPixels answers: capture.json, the memory and
//	                      who-wrote-it files (see cores/snes/trace.go), screen.png
//
// docs/snes-debugger.md describes the files.
package snesdbg

import (
	"bytes"
	"crypto/sha1"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"image"
	"image/png"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"runtime"
	"sort"
	"strings"
	"sync"
	"time"
)

// keepSessions: older session folders are removed when a new game starts.
const keepSessions = 12

// Base is the folder RetroPixels and the debugger share:
// %LOCALAPPDATA%\RetroPixels SNES Debugger on Windows (RETRO_SNESDBG_DIR
// overrides it).
func Base() string {
	if d := os.Getenv("RETRO_SNESDBG_DIR"); d != "" {
		return d
	}
	base := os.Getenv("LOCALAPPDATA")
	if base == "" {
		base, _ = os.UserCacheDir()
	}
	if base == "" {
		base = os.TempDir()
	}
	return filepath.Join(base, "RetroPixels SNES Debugger")
}

// ExeName is the debugger program's file name.
func ExeName() string {
	if runtime.GOOS == "windows" {
		return "RetroPixels-SNES-Debugger.exe"
	}
	return "retropixels-snes-debugger"
}

// FindExe looks for the debugger next to the running program (and in a
// "SNES Debugger" folder there); "" when it isn't there.
func FindExe() string {
	if p := os.Getenv("RETRO_SNESDBG_EXE"); p != "" {
		return p
	}
	exe, err := os.Executable()
	if err != nil {
		return ""
	}
	dir := filepath.Dir(exe)
	for _, p := range []string{filepath.Join(dir, ExeName()), filepath.Join(dir, "SNES Debugger", ExeName())} {
		if st, err := os.Stat(p); err == nil && !st.IsDir() {
			return p
		}
	}
	return ""
}

// DebuggerRunning: the debugger wrote its heartbeat in the last few seconds.
func DebuggerRunning(base string) bool {
	b, err := os.ReadFile(filepath.Join(base, "debugger.json"))
	if err != nil {
		return false
	}
	var hb struct {
		Alive int64 `json:"alive"`
	}
	if json.Unmarshal(b, &hb) != nil {
		return false
	}
	return time.Since(time.UnixMilli(hb.Alive)) < 4*time.Second
}

// Session is one SNES game playing with the debugger.
type Session struct {
	Dir  string
	base string

	mu     sync.Mutex
	asked  map[string]bool // requests already being answered
	closed bool
}

var unsafeName = regexp.MustCompile(`[^A-Za-z0-9 _\-]+`)

func writeAtomic(path string, data []byte) error {
	tmp := path + ".tmp"
	if err := os.WriteFile(tmp, data, 0o644); err != nil {
		return err
	}
	return os.Rename(tmp, path)
}

// Start begins a session for a game: it writes the ROM and session.json,
// marks it as the game playing now, and (with launch) starts the debugger
// unless it's already running. app names the RetroPixels program.
func Start(rom []byte, name, title, app string, launch bool) (*Session, error) {
	if len(rom)&0x3ff == 0x200 {
		rom = rom[0x200:] // copier header, as the emulator drops it
	}
	base := Base()
	sessions := filepath.Join(base, "sessions")
	if err := os.MkdirAll(sessions, 0o755); err != nil {
		return nil, err
	}
	prune(sessions)
	safe := strings.TrimSpace(unsafeName.ReplaceAllString(name, ""))
	if safe == "" {
		safe = "game"
	}
	if len(safe) > 48 {
		safe = safe[:48]
	}
	dir := filepath.Join(sessions, time.Now().Format("20060102-150405")+"-"+safe)
	for i := 2; ; i++ {
		if _, err := os.Stat(dir); errors.Is(err, os.ErrNotExist) {
			break
		}
		dir = filepath.Join(sessions, fmt.Sprintf("%s-%s-%d", time.Now().Format("20060102-150405"), safe, i))
	}
	for _, d := range []string{dir, filepath.Join(dir, "requests"), filepath.Join(dir, "captures")} {
		if err := os.MkdirAll(d, 0o755); err != nil {
			return nil, err
		}
	}
	if err := os.WriteFile(filepath.Join(dir, "rom.sfc"), rom, 0o644); err != nil {
		return nil, err
	}
	sum := sha1.Sum(rom)
	meta, _ := json.MarshalIndent(map[string]any{
		"format": "retropixels-snes-session", "version": 1, "name": name, "title": title, "app": app,
		"romFile": "rom.sfc", "romSha1": hex.EncodeToString(sum[:]), "romSize": len(rom), "started": time.Now().Unix(),
	}, "", "  ")
	if err := writeAtomic(filepath.Join(dir, "session.json"), meta); err != nil {
		return nil, err
	}
	s := &Session{Dir: dir, base: base, asked: map[string]bool{}}
	s.markActive(dir, name, app)
	if launch && !DebuggerRunning(base) {
		if exe := FindExe(); exe != "" {
			cmd := exec.Command(exe, "--base", base)
			cmd.Dir = filepath.Dir(exe)
			hideWindow(cmd)
			if err := cmd.Start(); err == nil {
				go cmd.Wait() //nolint:errcheck // it runs on its own
			}
		}
	}
	return s, nil
}

func (s *Session) markActive(dir, name, app string) {
	b, _ := json.MarshalIndent(map[string]any{"session": dir, "name": name, "app": app, "pid": os.Getpid(), "since": time.Now().Unix()}, "", "  ")
	_ = writeAtomic(filepath.Join(s.base, "active.json"), b)
}

// End: the game stopped.
func (s *Session) End() {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.closed {
		return
	}
	s.closed = true
	// only clear active.json if it's still this game
	if b, err := os.ReadFile(filepath.Join(s.base, "active.json")); err == nil {
		var a struct {
			Session string `json:"session"`
		}
		if json.Unmarshal(b, &a) == nil && a.Session == s.Dir {
			s.markActive("", "", "")
		}
	}
}

var reqName = regexp.MustCompile(`^[A-Za-z0-9_\-]{1,64}$`)

// Pending lists capture requests not answered yet (and marks them taken).
func (s *Session) Pending() []string {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.closed {
		return nil
	}
	ents, err := os.ReadDir(filepath.Join(s.Dir, "requests"))
	if err != nil {
		return nil
	}
	var out []string
	for _, e := range ents {
		id, ok := strings.CutSuffix(e.Name(), ".req")
		if !ok || !reqName.MatchString(id) || s.asked[id] {
			continue
		}
		if _, err := os.Stat(filepath.Join(s.Dir, "captures", id)); err == nil {
			s.asked[id] = true
			continue
		}
		s.asked[id] = true
		out = append(out, id)
	}
	sort.Strings(out)
	return out
}

// captureFiles are the binary files a capture holds, with their sizes.
var captureFiles = map[string]int{
	"vram.bin": 0x10000, "cgram.bin": 512, "oam.bin": 544, "wram.bin": 0x20000,
	"vram_src.bin": 0x40000, "vram_from.bin": 0x40000, "cgram_src.bin": 2048, "cgram_from.bin": 2048,
	"wram_src.bin": 0x80000, "wram_from.bin": 0x80000,
}

// Save writes the answer to request id: capture.json (meta), the files and
// the screen (a PNG, or nil). The folder appears whole (it's written next to
// it and renamed).
func (s *Session) Save(id string, meta []byte, files map[string][]byte, screenPNG []byte) error {
	if !reqName.MatchString(id) {
		return fmt.Errorf("snesdbg: bad request id")
	}
	for n, b := range files {
		want, ok := captureFiles[n]
		if !ok {
			return fmt.Errorf("snesdbg: unknown capture file %q", n)
		}
		if len(b) != want {
			return fmt.Errorf("snesdbg: %s is %d bytes, not %d", n, len(b), want)
		}
	}
	if !json.Valid(meta) {
		return fmt.Errorf("snesdbg: capture.json isn't JSON")
	}
	caps := filepath.Join(s.Dir, "captures")
	tmp := filepath.Join(caps, ".tmp-"+id)
	_ = os.RemoveAll(tmp)
	if err := os.MkdirAll(tmp, 0o755); err != nil {
		return err
	}
	if err := os.WriteFile(filepath.Join(tmp, "capture.json"), meta, 0o644); err != nil {
		return err
	}
	for n, b := range files {
		if err := os.WriteFile(filepath.Join(tmp, n), b, 0o644); err != nil {
			return err
		}
	}
	if len(screenPNG) > 0 {
		if err := os.WriteFile(filepath.Join(tmp, "screen.png"), screenPNG, 0o644); err != nil {
			return err
		}
	}
	if err := os.Rename(tmp, filepath.Join(caps, id)); err != nil {
		return err
	}
	_ = os.Remove(filepath.Join(s.Dir, "requests", id+".req"))
	return nil
}

// Request asks for a capture (what the debugger does; for tests and tools).
func Request(sessionDir, id string) error {
	return os.WriteFile(filepath.Join(sessionDir, "requests", id+".req"), []byte(`{"kind":"capture"}`), 0o644)
}

// ScreenPNG encodes a core's picture (0x00RRGGBB pixels) as a PNG.
func ScreenPNG(px []uint32, w, h int) []byte {
	if w <= 0 || h <= 0 || len(px) < w*h {
		return nil
	}
	img := image.NewNRGBA(image.Rect(0, 0, w, h))
	for i, c := range px[:w*h] {
		o := i * 4
		img.Pix[o], img.Pix[o+1], img.Pix[o+2], img.Pix[o+3] = uint8(c>>16), uint8(c>>8), uint8(c), 255
	}
	var buf bytes.Buffer
	_ = png.Encode(&buf, img)
	return buf.Bytes()
}

// DecodeUpload reads a capture sent by the browser's SNES emulator
// (web/emu/snes_debug.js): "RPSC", u32 count, then per file u16 name length,
// name, u32 size, bytes. capture.json and screen.png are among the files.
func DecodeUpload(b []byte) (meta []byte, files map[string][]byte, screen []byte, err error) {
	if len(b) < 8 || string(b[:4]) != "RPSC" {
		return nil, nil, nil, fmt.Errorf("snesdbg: not a capture")
	}
	n := int(binary.LittleEndian.Uint32(b[4:]))
	p := 8
	files = map[string][]byte{}
	for i := 0; i < n; i++ {
		if p+2 > len(b) {
			return nil, nil, nil, fmt.Errorf("snesdbg: capture cut short")
		}
		nl := int(binary.LittleEndian.Uint16(b[p:]))
		p += 2
		if p+nl+4 > len(b) {
			return nil, nil, nil, fmt.Errorf("snesdbg: capture cut short")
		}
		name := string(b[p : p+nl])
		p += nl
		sz := int(binary.LittleEndian.Uint32(b[p:]))
		p += 4
		if sz < 0 || p+sz > len(b) {
			return nil, nil, nil, fmt.Errorf("snesdbg: capture cut short")
		}
		data := b[p : p+sz]
		p += sz
		switch name {
		case "capture.json":
			meta = data
		case "screen.png":
			screen = data
		default:
			files[name] = data
		}
	}
	if meta == nil {
		return nil, nil, nil, fmt.Errorf("snesdbg: the capture has no capture.json")
	}
	return meta, files, screen, nil
}

// prune removes all but the newest session folders.
func prune(sessions string) {
	ents, err := os.ReadDir(sessions)
	if err != nil {
		return
	}
	var names []string
	for _, e := range ents {
		if e.IsDir() {
			names = append(names, e.Name())
		}
	}
	sort.Strings(names)
	for len(names) >= keepSessions {
		_ = os.RemoveAll(filepath.Join(sessions, names[0]))
		names = names[1:]
	}
}
