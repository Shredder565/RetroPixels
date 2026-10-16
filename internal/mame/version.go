package mame

import (
	"bytes"
	"context"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"time"
)

// ---------------- versions ----------------

var (
	reVersionOut = regexp.MustCompile(`^\s*(\d+\.\d+[a-z]?\d*)\b`)          // "0.270 (mame0270)"
	reVersionAny = regexp.MustCompile(`(?i)\bv?(0\.\d{2,3}(?:[bu]\d+)?)\b`) // "M.A.M.E. v0.37b16"
	reVersionDir = regexp.MustCompile(`(?i)mame[ _-]?0(\d{2,3})([bu]\d+)?`) // "mame0270", "mame0106u12"
	reVersionNum = regexp.MustCompile(`^(\d+)\.(\d+)([a-z]?)(\d*)`)
)

// ParseVersionOutput reads the version from MAME's -version (or -help) output.
func ParseVersionOutput(out string) string {
	out = strings.TrimSpace(out)
	if m := reVersionOut.FindStringSubmatch(out); m != nil {
		return m[1]
	}
	if m := reVersionAny.FindStringSubmatch(out); m != nil {
		return m[1]
	}
	return ""
}

// VersionFromPath guesses the version from a folder or file name such as
// mame0270 or "MAME 0.270" (for a MAME that won't say).
func VersionFromPath(p string) string {
	for d := p; d != "" && d != filepath.Dir(d); d = filepath.Dir(d) {
		b := filepath.Base(d)
		if m := reVersionAny.FindStringSubmatch(b); m != nil {
			return m[1]
		}
		if m := reVersionDir.FindStringSubmatch(b); m != nil {
			n, _ := strconv.Atoi(m[1])
			return "0." + strconv.Itoa(n) + strings.ToLower(m[2])
		}
	}
	return ""
}

// CompareVersions orders MAME versions: 0.37b16 < 0.37 < 0.106 < 0.106u2 <
// 0.270. Unknown ("") comes first. It returns -1, 0 or 1.
func CompareVersions(a, b string) int {
	pa, oka := splitVersion(a)
	pb, okb := splitVersion(b)
	switch {
	case !oka && !okb:
		return strings.Compare(a, b)
	case !oka:
		return -1
	case !okb:
		return 1
	}
	for i := range pa {
		if pa[i] != pb[i] {
			if pa[i] < pb[i] {
				return -1
			}
			return 1
		}
	}
	return 0
}

// splitVersion: major, minor, suffix rank (beta < release < update), suffix number.
func splitVersion(v string) ([4]int, bool) {
	m := reVersionNum.FindStringSubmatch(v)
	if m == nil {
		return [4]int{}, false
	}
	maj, _ := strconv.Atoi(m[1])
	min, _ := strconv.Atoi(m[2])
	rank := 1
	switch m[3] {
	case "b":
		rank = 0
	case "u":
		rank = 2
	}
	n, _ := strconv.Atoi(m[4])
	return [4]int{maj, min, rank, n}, true
}

// run runs MAME with a few arguments (no window) and returns what it printed.
func run(exe string, timeout time.Duration, args ...string) (string, error) {
	ctx, cancel := context.WithTimeout(context.Background(), timeout)
	defer cancel()
	cmd := exec.CommandContext(ctx, exe, args...)
	cmd.Dir = filepath.Dir(exe)
	hideProbe(cmd)
	var out bytes.Buffer
	cmd.Stdout = &out
	cmd.Stderr = &out
	err := cmd.Run()
	return out.String(), err
}

// ProbeVersion asks MAME its version ("0.270"). Old versions that don't
// know -version are asked for -help; failing that the folder name is used.
func ProbeVersion(exe string) string {
	if out, _ := run(exe, 15*time.Second, "-version"); out != "" {
		if v := ParseVersionOutput(out); v != "" {
			return v
		}
	}
	if out, _ := run(exe, 15*time.Second, "-help"); out != "" {
		if m := reVersionAny.FindStringSubmatch(out); m != nil {
			return m[1]
		}
	}
	return VersionFromPath(exe)
}

// ---------------- command-line options ----------------

// Options is the set of command-line options one MAME knows (from its
// -showconfig), so RetroPixels only passes options that version has.
type Options map[string]bool

// ParseShowConfig reads the option names from -showconfig output.
func ParseShowConfig(out string) Options {
	o := Options{}
	for _, l := range strings.Split(out, "\n") {
		l = strings.TrimSpace(l)
		if l == "" || strings.HasPrefix(l, "#") || strings.HasPrefix(l, "<") {
			continue
		}
		f := strings.Fields(l)[0]
		if strings.Trim(f, "abcdefghijklmnopqrstuvwxyz0123456789_") == "" {
			o[f] = true
		}
	}
	if len(o) < 10 { // not a real -showconfig (an error message, a GUI front end)
		return nil
	}
	return o
}

var (
	optMu    sync.Mutex
	optCache = map[string]Options{} // exe|size|mtime
)

// OptionsOf returns the options this MAME knows (nil: it didn't say).
func OptionsOf(exe string) Options {
	st, err := os.Stat(exe)
	if err != nil {
		return nil
	}
	key := exe + "|" + strconv.FormatInt(st.Size(), 10) + "|" + strconv.FormatInt(st.ModTime().Unix(), 10)
	optMu.Lock()
	o, ok := optCache[key]
	optMu.Unlock()
	if ok {
		return o
	}
	out, _ := run(exe, 20*time.Second, "-showconfig")
	o = ParseShowConfig(out)
	optMu.Lock()
	optCache[key] = o
	optMu.Unlock()
	return o
}
