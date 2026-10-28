package main

// Making the video of a game RPCS3 recorded (internal/rpcs3/record.go):
// RPCS3's picture file and its sound files, lined up by when each began.

import (
	"errors"
	"time"

	"retropixels/internal/dolphin"
	"retropixels/internal/mame"
	"retropixels/internal/rpcs3"
)

// errRPCS3NoPicture: RPCS3 ran a while but saved no picture.
var errRPCS3NoPicture = errors.New("RPCS3 didn't save the game's picture, so nothing was recorded")

// rpcs3Save makes out from what RPCS3 in folder dir saved for the game
// started at started, and deletes RPCS3's files once the video is made.
func rpcs3Save(ff, enc, dir string, started time.Time, ranFor float64, out string, progress func(int)) error {
	rec := rpcs3.FindRecording(dir, started)
	var clips []mame.Clip
	videoLen := 0.0
	for _, f := range rec.Videos {
		c, err := probeClip(ff, f)
		if err != nil || c.Dur < 0.05 {
			continue
		}
		clips = append(clips, c)
		videoLen += c.Dur
	}
	if len(clips) == 0 {
		rec.Remove()
		if ranFor > 30 {
			return errRPCS3NoPicture
		}
		return errRecTooShort
	}
	if videoLen < minRecording {
		rec.Remove()
		return errRecTooShort
	}
	var sound []string
	for _, s := range rec.Sounds {
		if rpcs3.RepairWav(s) == nil && dolphin.WavSeconds(s) > 0 {
			sound = append(sound, s)
		}
	}
	// The sound began before the picture by lead seconds: pixeluSaveArgs
	// cuts that much off its start when the sound is that much longer.
	soundLen := 0.0
	if len(sound) > 0 {
		lead := 0.0
		if !rec.SoundStart.IsZero() && !rec.VideoStart.IsZero() {
			lead = rec.VideoStart.Sub(rec.SoundStart).Seconds()
		}
		soundLen = videoLen + lead
		if soundLen <= 0 {
			soundLen = 0.001
		}
	}
	if err := encodeSave(ff, enc, clips, sound, videoLen, soundLen, out, progress); err != nil {
		return err
	}
	rec.Remove()
	return nil
}
