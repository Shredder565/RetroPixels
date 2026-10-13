package main

// Pieces shared by the Dolphin launcher and the recorder.

import (
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"path/filepath"
	"regexp"
	"strings"
	"sync"
	"time"
)

// ---------------- running game ----------------

type runningGame struct {
	Name     string
	Path     string
	Started  time.Time
	Emu      string // label shown in the app, e.g. "Dolphin"
	Kind     string // e.g. "dolphin"
	Embedded bool   // its game screen is docked inside the RetroPixels window
}

var (
	runMu   sync.Mutex
	running *runningGame
)

// ---------------- names ----------------

var (
	reBrackets = regexp.MustCompile(`\s*[\[\(][^\]\)]*[\]\)]`)
	reSpaces   = regexp.MustCompile(`\s+`)
)

// cleanGameName turns "Super Mario Sunshine (USA) [GMSE01].iso" into "Super Mario Sunshine".
func cleanGameName(file string) string {
	n := strings.TrimSuffix(file, filepath.Ext(file))
	n = strings.ReplaceAll(n, "_", " ")
	n = strings.ReplaceAll(n, "  ", " – ") // dumps often turn "A: B" / "A – B" into a double space
	n = reBrackets.ReplaceAllString(n, "")
	n = reSpaces.ReplaceAllString(n, " ")
	n = strings.TrimSpace(strings.Trim(n, " -–+"))
	if n == "" {
		return strings.TrimSuffix(file, filepath.Ext(file))
	}
	return n
}

// ---------------- settings description (for the settings panels) ----------------

type opt struct {
	V any    `json:"v"`
	L string `json:"l"`
}

type setting struct {
	Key     string  `json:"key"`
	Label   string  `json:"label"`
	Group   string  `json:"group"`
	Type    string  `json:"type"` // select | bool | range
	Options []opt   `json:"options,omitempty"`
	Min     float64 `json:"min,omitempty"`
	Max     float64 `json:"max,omitempty"`
	Step    float64 `json:"step,omitempty"`
	Help    string  `json:"help,omitempty"`
	Value   any     `json:"value"`
	Present bool    `json:"present"`
}

func sel(pairs ...any) []opt {
	var o []opt
	for i := 0; i+1 < len(pairs); i += 2 {
		o = append(o, opt{V: pairs[i], L: pairs[i+1].(string)})
	}
	return o
}

func coerce(s setting, raw any, current any) (any, error) {
	switch s.Type {
	case "bool":
		b, ok := raw.(bool)
		if !ok {
			return nil, fmt.Errorf("%s must be on or off", s.Label)
		}
		return b, nil
	case "range":
		f, ok := raw.(float64)
		if !ok || f < s.Min || f > s.Max {
			return nil, fmt.Errorf("%s is out of range", s.Label)
		}
		if s.Step >= 1 {
			return int64(f), nil
		}
		return f, nil
	case "select":
		for _, o := range s.Options {
			if fmt.Sprint(o.V) == fmt.Sprint(raw) {
				return o.V, nil
			}
		}
		if fmt.Sprint(current) == fmt.Sprint(raw) {
			return current, nil
		}
		return nil, fmt.Errorf("%s: %v isn't a valid choice", s.Label, raw)
	}
	return nil, errors.New("unknown setting type")
}

// ---------------- request helpers ----------------

func readBody(w http.ResponseWriter, r *http.Request, v any) bool {
	if err := json.NewDecoder(http.MaxBytesReader(w, r.Body, 64<<10)).Decode(v); err != nil {
		writeJSON(w, 400, map[string]any{"error": "bad request"})
		return false
	}
	return true
}

func fail(w http.ResponseWriter, err error) { writeJSON(w, 400, map[string]any{"error": err.Error()}) }
