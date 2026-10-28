// Command snescapture runs a SNES ROM on RetroPixels' Go SNES core for some
// frames and writes a RetroPixels SNES Debugger capture of that moment (the
// same files RetroPixels writes when the debugger asks), or just the picture.
// It's for testing the debugger and for working without the app.
//
//	go run ./tools/snescapture -rom game.sfc -frames 120 -out capdir
//	go run ./tools/snescapture -rom game.sfc -frames 120 -png screen.png
//
// -press "60:start,61:start" holds buttons at frames (player 1).
package main

import (
	"encoding/json"
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"strings"

	"retropixels/core"
	"retropixels/cores/snes"
	"retropixels/internal/snesdbg"
)

func main() {
	romPath := flag.String("rom", "", "SNES ROM (.sfc / .smc)")
	frames := flag.Int("frames", 60, "frames to run before the capture")
	out := flag.String("out", "", "capture folder to write")
	pngOut := flag.String("png", "", "write the picture as a PNG")
	press := flag.String("press", "", "buttons held at frames, frame:button[+button],... (b y select start up down left right a x l r)")
	flag.Parse()
	if *romPath == "" || (*out == "" && *pngOut == "") {
		flag.Usage()
		os.Exit(2)
	}
	rom, err := os.ReadFile(*romPath)
	check(err)
	c, err := snes.New(rom, core.Options{SampleRate: 48000, Name: filepath.Base(*romPath)})
	check(err)
	s := c.(*snes.SNES)
	s.EnableTrace()
	held := map[int]uint32{}
	names := c.Info().Buttons
	if *press != "" {
		for _, part := range strings.Split(*press, ",") {
			f, bs, ok := strings.Cut(part, ":")
			n, err := strconv.Atoi(f)
			if !ok || err != nil {
				check(fmt.Errorf("bad -press %q", part))
			}
			for _, b := range strings.Split(bs, "+") {
				found := false
				for i, nm := range names {
					if strings.EqualFold(nm, b) {
						held[n] |= 1 << i
						found = true
					}
				}
				if !found {
					check(fmt.Errorf("no button %q (buttons: %v)", b, names))
				}
			}
		}
	}
	for f := 0; f < *frames; f++ {
		s.SetInput(0, held[f])
		s.RunFrame()
		s.TakeAudio()
	}
	shot := snesdbg.ScreenPNG(s.Screen(), s.Width(), s.Height())
	if *pngOut != "" {
		check(os.WriteFile(*pngOut, shot, 0o644))
	}
	if *out != "" {
		cp, err := s.DebugCapture()
		check(err)
		check(os.MkdirAll(*out, 0o755))
		meta, err := json.MarshalIndent(cp.Meta, "", " ")
		check(err)
		check(os.WriteFile(filepath.Join(*out, "capture.json"), meta, 0o644))
		for n, b := range cp.Files {
			check(os.WriteFile(filepath.Join(*out, n), b, 0o644))
		}
		check(os.WriteFile(filepath.Join(*out, "screen.png"), shot, 0o644))
		// the ROM next to it, as a session folder has it
		r := rom
		if len(r)&0x3ff == 0x200 {
			r = r[0x200:]
		}
		check(os.WriteFile(filepath.Join(*out, "rom.sfc"), r, 0o644))
	}
}

func check(err error) {
	if err != nil {
		fmt.Fprintln(os.Stderr, "snescapture:", err)
		os.Exit(1)
	}
}
