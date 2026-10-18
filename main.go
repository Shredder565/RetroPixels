// RetroPixels — desktop app for playing retro games.
//
// It serves the embedded page (and RetroPixels' own emulators) on a fixed local
// port, so save states stored by the page stay put between runs, opens it in a
// chromeless Edge/Chrome app window, and launches Dolphin for GameCube and Wii,
// PixelU for Wii U and MAME (any version of it) for arcade games.
package main

import (
	"bytes"
	"crypto/rand"
	"embed"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io/fs"
	"log"
	"mime"
	"net"
	"net/http"
	"os"
	"os/exec"
	"path"
	"path/filepath"
	"strings"
	"sync"
	"time"
)

//go:embed web
var webFS embed.FS

const (
	port    = 47821
	appName = "RetroPixels"
	// appVersion is shown in the app and goes in the download's name and VERSION.txt.
	appVersion = "2.21.0"
)

var (
	token  string
	hostOK = map[string]bool{
		fmt.Sprintf("127.0.0.1:%d", port): true,
		fmt.Sprintf("localhost:%d", port): true,
	}
)

// ---------------- config ----------------

type recentGame struct {
	Path       string    `json:"path"`
	Name       string    `json:"name"`
	LastPlayed time.Time `json:"lastPlayed"`
}

type config struct {
	AutoRecord    *bool  `json:"switchAutoRecord,omitempty"` // key kept so the old setting carries over
	RecordFPS     int    `json:"recordFps,omitempty"`
	RecordQuality string `json:"recordQuality,omitempty"`
	RecordDir     string `json:"recordDir,omitempty"`

	// The Dolphin.exe used before PixelU's Wii / GameCube engine: no longer
	// started, only read to find its old settings and games folders.
	DolphinExe        string       `json:"dolphinExe,omitempty"`
	DolphinFolders    []string     `json:"dolphinFolders,omitempty"`
	DolphinRecent     []recentGame `json:"dolphinRecent,omitempty"`
	DolphinFullscreen *bool        `json:"dolphinFullscreen,omitempty"`
	DolphinEmbed      *bool        `json:"dolphinEmbed,omitempty"` // play inside the RetroPixels window
	DolphinDetected   bool         `json:"dolphinDetected,omitempty"`
	DolphinVolume     *int         `json:"dolphinVolume,omitempty"` // 0–100, unset = 100
	DolphinMuted      bool         `json:"dolphinMuted,omitempty"`

	// Wii U games through PixelU (pixelu.go)
	PixelUExe        string       `json:"pixeluExe,omitempty"`
	PixelUFolders    []string     `json:"pixeluFolders,omitempty"`
	PixelURecent     []recentGame `json:"pixeluRecent,omitempty"`
	PixelUFullscreen *bool        `json:"pixeluFullscreen,omitempty"` // unset = on
	PixelUEmbed      *bool        `json:"pixeluEmbed,omitempty"`      // unset = on
	PixelURecord     *bool        `json:"pixeluRecord,omitempty"`     // unset = off
}

var (
	cfgMu sync.Mutex
	cfg   config
)

func configDir() string {
	if d, err := os.UserConfigDir(); err == nil {
		return filepath.Join(d, appName)
	}
	return filepath.Join(".", appName)
}

func loadConfig() {
	b, err := os.ReadFile(filepath.Join(configDir(), "config.json"))
	if err == nil {
		_ = json.Unmarshal(b, &cfg)
	}
}

func saveConfigLocked() error {
	if err := os.MkdirAll(configDir(), 0o755); err != nil {
		return err
	}
	b, _ := json.MarshalIndent(cfg, "", "  ")
	tmp := filepath.Join(configDir(), "config.json.tmp")
	if err := os.WriteFile(tmp, b, 0o644); err != nil {
		return err
	}
	return os.Rename(tmp, filepath.Join(configDir(), "config.json"))
}

// ---------------- launching emulators ----------------

// startEmulator runs an emulator for one game and tracks it. rec (optional)
// is the game's recording; onStart (optional) runs alongside it, e.g. to dock
// its window into ours.
func startEmulator(emu string, args []string, game, name, kind, label string, rec *recSession, onStart func(pid int, exited <-chan struct{})) error {
	cmd := exec.Command(emu, args...)
	cmd.Dir = filepath.Dir(emu) // portable emulators keep their data next to the .exe
	return startProcess(cmd, game, name, kind, label, rec, onStart, nil)
}

// startProcess is startEmulator for a command that's already made. onExit
// (optional) gets how the emulator ended, once it has.
func startProcess(cmd *exec.Cmd, game, name, kind, label string, rec *recSession, onStart func(pid int, exited <-chan struct{}), onExit func(err error, ran time.Duration)) error {
	runMu.Lock()
	if running != nil {
		cur := running.Name
		runMu.Unlock()
		return fmt.Errorf("%s is still running. Close it first, then start another game", cur)
	}
	if err := cmd.Start(); err != nil {
		runMu.Unlock()
		return fmt.Errorf("couldn't start the emulator: %w", err)
	}
	started := time.Now()
	running = &runningGame{Name: name, Path: game, Started: started, Emu: label, Kind: kind}
	runMu.Unlock()
	if rec != nil {
		startRecSession(rec, cmd.Args[1:])
	}
	exited := make(chan struct{})
	go func() {
		err := cmd.Wait()
		close(exited)
		runMu.Lock()
		running = nil
		runMu.Unlock()
		if onExit != nil {
			onExit(err, time.Since(started))
		}
	}()
	if onStart != nil {
		go onStart(cmd.Process.Pid, exited)
	}
	if rec != nil {
		go rec.follow(exited)
	}
	return nil
}

// ---------------- HTTP ----------------

func writeJSON(w http.ResponseWriter, code int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "no-store")
	w.WriteHeader(code)
	_ = json.NewEncoder(w).Encode(v)
}

// guard rejects requests that don't come from our own page: wrong Host (DNS
// rebinding) or missing token (other websites can't read it from our page).
func guard(next http.HandlerFunc) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if !hostOK[r.Host] {
			http.Error(w, "forbidden", http.StatusForbidden)
			return
		}
		if r.Header.Get("X-Retro-Token") != token {
			http.Error(w, "forbidden", http.StatusForbidden)
			return
		}
		if r.Method != http.MethodPost && r.Method != http.MethodGet {
			http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
			return
		}
		next(w, r)
	}
}

// statusPayload: what's running and the recorder's state.
func statusPayload() map[string]any {
	rec := recStatus()
	runMu.Lock()
	var now any
	if running != nil {
		now = map[string]any{"name": running.Name, "started": running.Started, "emu": running.Emu, "kind": running.Kind, "embedded": running.Embedded}
	}
	runMu.Unlock()
	return map[string]any{"rec": rec, "running": now}
}

func newMux() *http.ServeMux {
	mux := http.NewServeMux()
	sub, _ := fs.Sub(webFS, "web")
	indexHTML, _ := fs.ReadFile(sub, "index.html")
	page := bytes.Replace(indexHTML,
		[]byte("/*__RETRO_DESKTOP__*/"),
		[]byte(fmt.Sprintf(`window.RETRO_DESKTOP = { token: %q, version: %q };`, token, appVersion)), 1)

	mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		if !hostOK[r.Host] {
			http.Error(w, "forbidden", http.StatusForbidden)
			return
		}
		p := strings.TrimPrefix(path.Clean(r.URL.Path), "/")
		if p == "" || p == "index.html" {
			w.Header().Set("Content-Type", "text/html; charset=utf-8")
			w.Header().Set("Cache-Control", "no-store")
			_, _ = w.Write(page)
			return
		}
		// RetroPixels' own emulators and other page files.
		b, err := fs.ReadFile(sub, p)
		if err != nil || strings.HasPrefix(p, ".") {
			http.NotFound(w, r)
			return
		}
		if t := mime.TypeByExtension(path.Ext(p)); t != "" {
			w.Header().Set("Content-Type", t)
		}
		w.Header().Set("Cache-Control", "no-store")
		_, _ = w.Write(b)
	})

	// Unauthenticated, harmless: lets a second copy of the app detect the first.
	mux.HandleFunc("/ping", func(w http.ResponseWriter, r *http.Request) {
		_, _ = w.Write([]byte(appName))
	})

	mux.HandleFunc("/api/status", guard(func(w http.ResponseWriter, r *http.Request) {
		writeJSON(w, 200, statusPayload())
	}))

	registerRecorderRoutes(mux)
	registerFFmpegRoutes(mux)
	registerDolphinRoutes(mux)
	registerPixelURoutes(mux)
	registerMameRoutes(mux)
	registerLifetimeRoutes(mux)
	registerN64Routes(mux)
	registerWiiURoutes(mux)
	registerArcadeRoutes(mux)
	registerAPRoutes(mux)
	return mux
}

func alreadyRunning() bool {
	c := http.Client{Timeout: 700 * time.Millisecond}
	resp, err := c.Get(fmt.Sprintf("http://127.0.0.1:%d/ping", port))
	if err != nil {
		return false
	}
	defer resp.Body.Close()
	b := make([]byte, 32)
	n, _ := resp.Body.Read(b)
	return string(b[:n]) == appName
}

func main() {
	url := fmt.Sprintf("http://127.0.0.1:%d/", port)

	if alreadyRunning() {
		// Bring up another window on the copy that's already running.
		_ = openAppWindow(url, false)
		return
	}

	tb := make([]byte, 24)
	_, _ = rand.Read(tb)
	token = hex.EncodeToString(tb)
	migrateOldName()
	loadConfig()
	go autoDetectDolphin()

	ln, err := net.Listen("tcp", fmt.Sprintf("127.0.0.1:%d", port))
	if err != nil {
		showError(fmt.Sprintf("%s couldn't start because port %d is in use by another program.\n\n%v", appName, port, err))
		os.Exit(1)
	}
	srv := &http.Server{Handler: newMux(), ReadHeaderTimeout: 10 * time.Second}
	go func() {
		if err := srv.Serve(ln); err != nil && !errors.Is(err, http.ErrServerClosed) {
			log.Println(err)
		}
	}()
	// Archipelago's clients find RetroPixels here (loopback only); see archipelago.go.
	ap.start()

	// Opens the window and blocks until it closes. Then the app exits.
	if err := openAppWindow(url, true); err != nil {
		showError(err.Error())
		os.Exit(1)
	}
}
