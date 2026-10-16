package main

import (
	"crypto/sha1"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"os"

	"retropixels/core"
	"retropixels/internal/games"
)

// referenceResult: what a game's core makes in its first frames, run
// straight on the core (no protocol), for the tests to compare the engine's
// frames with.
type referenceResult struct {
	System  string `json:"system"`
	Width   int    `json:"width"`
	Height  int    `json:"height"`
	Frames  int    `json:"frames"`
	Pixels  string `json:"pixels"`  // SHA-1 of the last picture as R, G, B, 255 bytes
	Audio   string `json:"audio"`   // SHA-1 of all the sound as s16 little-endian stereo (volume 1)
	Samples int    `json:"samples"` // s16 values in it
}

func runReference(path string, frames, rate int) (*referenceResult, error) {
	g, err := games.Open(path, games.OpenOptions{})
	if err != nil {
		return nil, err
	}
	c, err := games.Makers[g.System](g.ROM, core.Options{SampleRate: rate, Name: g.Name})
	if err != nil {
		return nil, err
	}
	info := c.Info()
	players := min(max(info.Players, 1), 4)
	ah := sha1.New()
	var pcm []int16
	var le []byte
	samples := 0
	for i := 0; i < frames; i++ {
		// the engine sets every player's input before each run, as here
		if pc, ok := c.(interface{ SetConnected(n int) }); ok {
			pc.SetConnected(1)
		}
		for p := 0; p < players; p++ {
			c.SetInput(p, 0)
			if an, ok := c.(core.Analog); ok {
				for a := 0; a < 4; a++ {
					an.SetAnalog(p, a, 0)
				}
			}
		}
		c.RunFrame()
		pcm = games.ToPCM(pcm, c.TakeAudio(), info.Stereo, 1)
		le = le[:0]
		for _, s := range pcm {
			le = binary.LittleEndian.AppendUint16(le, uint16(s))
		}
		ah.Write(le)
		samples += len(pcm)
	}
	w, h := c.Width(), c.Height()
	ph := sha1.Sum(appendRGBA(nil, c.Screen()[:w*h]))
	return &referenceResult{System: g.System, Width: w, Height: h, Frames: frames, Pixels: hex.EncodeToString(ph[:]),
		Audio: hex.EncodeToString(ah.Sum(nil)), Samples: samples}, nil
}

func reference(path string, frames, rate int) error {
	r, err := runReference(path, frames, rate)
	if err != nil {
		return err
	}
	b, _ := json.Marshal(r)
	_, err = fmt.Fprintln(os.Stdout, string(b))
	return err
}
