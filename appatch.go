package main

// Archipelago patch files, for the page: internal/apatch reads them and makes
// the game; the page asks for the base ROM and keeps it.

import (
	"encoding/binary"
	"errors"
	"io"
	"net/http"

	"retropixels/internal/apatch"
)

// ---------------- HTTP ----------------

func registerAPPatchRoutes(mux *http.ServeMux) {
	// Body: the patch file. Answers what it is, so the page knows which ROM to ask for.
	mux.HandleFunc("/api/ap/inspect", guard(func(w http.ResponseWriter, r *http.Request) {
		data, err := io.ReadAll(http.MaxBytesReader(w, r.Body, apatch.MaxPatch))
		if err != nil {
			fail(w, errors.New("that patch file is too big"))
			return
		}
		p, err := apatch.Read(data, r.URL.Query().Get("name"))
		if err != nil {
			fail(w, err)
			return
		}
		writeJSON(w, 200, p)
	}))

	// Body: u32 patch length (little-endian), the patch file, then the base ROM.
	// Answers the patched ROM.
	mux.HandleFunc("/api/ap/patch", guard(func(w http.ResponseWriter, r *http.Request) {
		data, err := io.ReadAll(http.MaxBytesReader(w, r.Body, apatch.MaxPatch+apatch.MaxROM+4))
		if err != nil || len(data) < 4 {
			fail(w, errors.New("the patch and ROM didn't arrive"))
			return
		}
		n := int(binary.LittleEndian.Uint32(data))
		if n > len(data)-4 {
			fail(w, errors.New("the patch and ROM didn't arrive"))
			return
		}
		p, err := apatch.Read(data[4:4+n], r.URL.Query().Get("name"))
		if err != nil {
			fail(w, err)
			return
		}
		rom, err := p.Apply(data[4+n:])
		if err != nil {
			var wb *apatch.WrongBaseError
			code := "patch"
			if errors.As(err, &wb) {
				code = "wrongBase"
			}
			writeJSON(w, 400, map[string]any{"error": err.Error(), "code": code})
			return
		}
		w.Header().Set("Content-Type", "application/octet-stream")
		w.Header().Set("Cache-Control", "no-store")
		_, _ = w.Write(rom)
	}))
}
