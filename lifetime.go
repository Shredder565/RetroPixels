package main

import (
	"net/http"
	"sync"
	"time"
)

// The page checks in every few seconds while it's open, and says goodbye when
// it closes. Once the browser window has closed, the app quits as soon as the
// page has said goodbye (or has been silent for a few minutes, which covers
// crashes). Minimized windows check in less often, hence the long timeout.

var (
	lifeMu    sync.Mutex
	lastBeat  = time.Now()
	lastBye   time.Time
	staleTime = 3 * time.Minute
	byeGrace  = 8 * time.Second
)

func heartbeat() {
	lifeMu.Lock()
	lastBeat = time.Now()
	lifeMu.Unlock()
}

func registerLifetimeRoutes(mux *http.ServeMux) {
	mux.HandleFunc("/api/heartbeat", guard(func(w http.ResponseWriter, r *http.Request) {
		heartbeat()
		w.WriteHeader(http.StatusNoContent)
	}))
	// sendBeacon can't add headers, so the token comes in the body.
	mux.HandleFunc("/api/bye", func(w http.ResponseWriter, r *http.Request) {
		if !hostOK[r.Host] || r.Method != http.MethodPost {
			http.Error(w, "forbidden", http.StatusForbidden)
			return
		}
		buf := make([]byte, 128)
		n, _ := r.Body.Read(buf)
		if string(buf[:n]) != token {
			http.Error(w, "forbidden", http.StatusForbidden)
			return
		}
		lifeMu.Lock()
		lastBye = time.Now()
		lifeMu.Unlock()
		w.WriteHeader(http.StatusNoContent)
	})
}

func shouldQuit(now time.Time) bool {
	lifeMu.Lock()
	defer lifeMu.Unlock()
	if lastBye.After(lastBeat) && now.Sub(lastBye) > byeGrace {
		return true
	}
	return now.Sub(lastBeat) > staleTime
}

func waitForPageToClose() {
	for !shouldQuit(time.Now()) {
		time.Sleep(2 * time.Second)
	}
}
