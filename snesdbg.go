package main

// The RetroPixels SNES Debugger (tools/snes-debugger) opens alongside SNES
// games. In RetroPixels.exe the SNES emulator runs in the page, so the page
// (web/emu/snes_debug.js) sends the game's ROM and its captures here, and
// internal/snesdbg keeps them in the folder the debugger reads.

import (
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"io"
	"net/http"
	"sync"
	"time"

	"retropixels/internal/snesdbg"
)

type snesDbgPage struct {
	s        *snesdbg.Session
	lastPoll time.Time
}

var (
	snesDbgMu    sync.Mutex
	snesDbgPages = map[string]*snesDbgPage{}
	// for tests: whether Start may start the debugger program
	snesDbgLaunch = true
	snesDbgReaper sync.Once
)

func snesDbgJSON(w http.ResponseWriter, code int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(code)
	_ = json.NewEncoder(w).Encode(v)
}

// snesDbgReap ends sessions whose page stopped asking (the game was closed).
func snesDbgReap() {
	snesDbgMu.Lock()
	defer snesDbgMu.Unlock()
	for id, p := range snesDbgPages {
		if time.Since(p.lastPoll) > 8*time.Second {
			p.s.End()
			delete(snesDbgPages, id)
		}
	}
}

func registerSNESDebugRoutes(mux *http.ServeMux) {
	snesDbgReaper.Do(func() {
		go func() {
			for range time.Tick(3 * time.Second) {
				snesDbgReap()
			}
		}()
	})
	mux.HandleFunc("/api/snesdbg/start", guard(func(w http.ResponseWriter, r *http.Request) {
		rom, err := io.ReadAll(io.LimitReader(r.Body, 16<<20))
		if err != nil || len(rom) < 0x8000 {
			snesDbgJSON(w, http.StatusBadRequest, map[string]any{"error": "no ROM"})
			return
		}
		q := r.URL.Query()
		s, err := snesdbg.Start(rom, q.Get("name"), q.Get("title"), "RetroPixels.exe", snesDbgLaunch)
		if err != nil {
			snesDbgJSON(w, http.StatusInternalServerError, map[string]any{"error": err.Error()})
			return
		}
		var b [8]byte
		_, _ = rand.Read(b[:])
		id := hex.EncodeToString(b[:])
		snesDbgMu.Lock()
		// one SNES game plays at a time: the one before has ended
		for k, p := range snesDbgPages {
			p.s.End()
			delete(snesDbgPages, k)
		}
		snesDbgPages[id] = &snesDbgPage{s: s, lastPoll: time.Now()}
		snesDbgMu.Unlock()
		snesDbgJSON(w, http.StatusOK, map[string]any{"id": id, "session": s.Dir, "debugger": snesdbg.FindExe() != ""})
	}))
	page := func(r *http.Request) *snesDbgPage {
		snesDbgMu.Lock()
		defer snesDbgMu.Unlock()
		p := snesDbgPages[r.URL.Query().Get("id")]
		if p != nil {
			p.lastPoll = time.Now()
		}
		return p
	}
	mux.HandleFunc("/api/snesdbg/poll", guard(func(w http.ResponseWriter, r *http.Request) {
		p := page(r)
		if p == nil {
			snesDbgJSON(w, http.StatusNotFound, map[string]any{"error": "no such game"})
			return
		}
		pending := p.s.Pending()
		if pending == nil {
			pending = []string{}
		}
		snesDbgJSON(w, http.StatusOK, map[string]any{"pending": pending})
	}))
	mux.HandleFunc("/api/snesdbg/capture", guard(func(w http.ResponseWriter, r *http.Request) {
		p := page(r)
		if p == nil {
			snesDbgJSON(w, http.StatusNotFound, map[string]any{"error": "no such game"})
			return
		}
		body, err := io.ReadAll(io.LimitReader(r.Body, 8<<20))
		if err != nil {
			snesDbgJSON(w, http.StatusBadRequest, map[string]any{"error": err.Error()})
			return
		}
		meta, files, screen, err := snesdbg.DecodeUpload(body)
		if err == nil {
			err = p.s.Save(r.URL.Query().Get("req"), meta, files, screen)
		}
		if err != nil {
			snesDbgJSON(w, http.StatusBadRequest, map[string]any{"error": err.Error()})
			return
		}
		snesDbgJSON(w, http.StatusOK, map[string]any{"ok": true})
	}))
	mux.HandleFunc("/api/snesdbg/end", guard(func(w http.ResponseWriter, r *http.Request) {
		snesDbgMu.Lock()
		id := r.URL.Query().Get("id")
		if p := snesDbgPages[id]; p != nil {
			p.s.End()
			delete(snesDbgPages, id)
		}
		snesDbgMu.Unlock()
		snesDbgJSON(w, http.StatusOK, map[string]any{"ok": true})
	}))
}
