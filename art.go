package main

// Box art for the library tiles (GameCube, Wii, Wii U, 3DS / DS, arcade):
// the page asks for each game's cover as its tile comes into view, and shows
// the game's initials only when there's none. internal/boxart finds it.

import (
	"mime"
	"net/http"
	"os"
	"path/filepath"

	"retropixels/internal/boxart"
)

func registerArtRoutes(mux *http.ServeMux) {
	boxart.UserAgent = appName + "/" + appVersion
	// GET /api/art?system=gamecube&path=...&name=...&id=...&short=...
	mux.HandleFunc("/api/art", guard(func(w http.ResponseWriter, r *http.Request) {
		q := r.URL.Query()
		g := boxart.Game{System: q.Get("system"), Path: q.Get("path"), Name: q.Get("name"), ID: q.Get("id"), Short: q.Get("short")}
		switch g.System {
		case boxart.GameCube, boxart.Wii, boxart.WiiU, boxart.DS, boxart.N3DS, boxart.Arcade:
		default:
			http.Error(w, "unknown system", http.StatusBadRequest)
			return
		}
		var p string
		if q.Get("cached") == "1" {
			p = boxart.Cached(g)
		} else {
			p, _ = boxart.Find(g)
		}
		if p == "" || !boxart.IsImage(p) {
			http.NotFound(w, r)
			return
		}
		b, err := os.ReadFile(p)
		if err != nil {
			http.NotFound(w, r)
			return
		}
		if t := mime.TypeByExtension(filepath.Ext(p)); t != "" {
			w.Header().Set("Content-Type", t)
		}
		w.Header().Set("Cache-Control", "private, max-age=86400")
		_, _ = w.Write(b)
	}))
}
