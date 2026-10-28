package main

// Record > Record games automatically: every game, whichever system plays
// it (RetroPixels' own emulators, GameCube / Wii, Wii U, MAME, PS3 in RPCS3), is recorded
// from its start, as RetroPixels.exe does. Turned off, F9 still records
// RetroPixels' own games, and GameCube / Wii's "Record the next game" stays.

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"time"

	"retropixels/internal/ffmpeg"
	"retropixels/internal/mame"
)

func (a *app) toggleAutoRecord() {
	v := !a.settings.autoRecord()
	a.settings.AutoRecord = &v
	saveSettings(a.settings)
	if v {
		a.showMessage("Games will be recorded automatically (from the next game)")
	} else {
		a.showMessage("Automatic recording off")
	}
}

// recFFmpeg: recordings need ffmpeg. When it's missing, the player is asked
// once whether to download it (an automatic recording doesn't download
// 200 MB without asking); No means playing without recording. needNow: the
// engine needs ffmpeg while the game runs (PixelU), so a download that is
// only starting is too late for this game. manual: the player asked for this
// recording, so they're asked again even after a No.
func (a *app) recFFmpeg(needNow, manual bool) bool {
	if ffmpeg.Find() != "" {
		return true
	}
	if a.ffDownload != "" {
		if needNow {
			a.showMessage("Not recording this game: ffmpeg is still downloading")
		}
		return !needNow
	}
	if a.ffDeclined && !manual {
		return false
	}
	if messageBox(a.hwnd, "Recording needs ffmpeg, a free video program (about 200 MB, from github.com/BtbN/FFmpeg-Builds; "+
		"RetroPixels.exe uses the same one).\n\nDownload it now? (No: play without recording this time. "+
		"Record > Record games automatically turns automatic recording off.)",
		"RetroPixels Native", mbYesNo|mbIconInfo) != idYes {
		if !manual {
			a.ffDeclined = true // not asked again until RetroPixels Native is started again
		}
		return false
	}
	a.downloadFFmpeg()
	if needNow {
		a.showMessage("Downloading ffmpeg: games are recorded from the next one")
	}
	return !needNow
}

// autoRecordBuiltIn starts recording a game RetroPixels' own emulators play,
// as F9 does, when automatic recording is on. A recording under a few
// seconds (the game closed straight away) isn't kept.
func (a *app) autoRecordBuiltIn() {
	s := a.sess
	if s == nil || a.recOn || !a.settings.autoRecord() || !a.recFFmpeg(false, false) {
		return
	}
	ff := ffmpeg.Find()
	if ff == "" { // downloading: recording starts once it is ready (ff-done)
		return
	}
	if err := s.startRecordingMin(ff, a.encoder(ff), a.settings.recQuality(), a.settings.recDir(), minRecording); err != nil {
		a.showMessage("Couldn't start recording: " + err.Error())
		return
	}
	a.recOn, a.recStart = true, time.Now()
	a.updateTitle()
}

// ---------------- Wii U and MAME ----------------

// extRec is a Wii U or MAME game being saved by its engine: its raw files
// go to dir (a folder for this game alone) and become one .mp4 when the
// game ends.
type extRec struct {
	game    string
	dir     string
	out     string // the .mp4 to make (in the recordings folder)
	started time.Time
}

// newExtRec: a recording about to start (UI thread). It counts as a
// recording still to be saved from now, so closing the window waits for it.
func (a *app) newExtRec(game, dir string) *extRec {
	now := time.Now()
	a.dol.mu.Lock()
	a.dol.pending++
	a.dol.mu.Unlock()
	return &extRec{game: game, dir: dir, out: uniqueOut(a.settings.recDir(), game, now), started: now}
}

// extRecDropped: the game didn't start after all.
func (a *app) extRecDropped(r *extRec) {
	a.dol.mu.Lock()
	a.dol.pending--
	a.dol.mu.Unlock()
	go removeDir(r.dir)
}

// extSave makes the video once the game has ended (in the background, one
// recording at a time, shown in the title bar as GameCube / Wii ones are).
// save gets ffmpeg and makes r.out.
func (a *app) extSave(r *extRec, save func(ff string, progress func(int)) error) {
	dolSaveMu.Lock()
	defer dolSaveMu.Unlock()
	defer removeDir(r.dir) // the raw files are huge
	set := func(step string, pc int) {
		a.dol.mu.Lock()
		a.dol.saving, a.dol.saveStep, a.dol.savePct = r.game, step, pc
		a.dol.mu.Unlock()
		a.event("dol-progress", "")
	}
	set("ffmpeg", 0)
	ff := a.dolWaitFFmpeg()
	err := errors.New("the recording couldn't be saved because ffmpeg isn't there to make the video")
	if ff != "" {
		set("saving", 0)
		err = save(ff, func(pc int) { set("saving", pc) })
	}
	a.dol.mu.Lock()
	a.dol.saving = ""
	a.dol.pending--
	a.dol.mu.Unlock()
	switch {
	case err == nil:
		a.event("dol-saved", filepath.Base(r.out))
	case errors.Is(err, errRecTooShort), errors.Is(err, mame.ErrTooShort):
		a.event("dol-progress", "") // nothing worth keeping
	default:
		a.event("ext-save-error", r.game+": "+err.Error())
	}
}

// recTitle: " - ● REC m:ss" while an engine records the game.
func recTitle(since time.Time) string {
	d := int(time.Since(since).Seconds())
	return fmt.Sprintf(" - ● REC %d:%02d", d/60, d%60)
}

// removeDir deletes a recording's raw files, trying again for a few seconds
// (Windows can keep a file busy for a moment after the engine closes).
func removeDir(dir string) {
	for i := 0; i < 10 && os.RemoveAll(dir) != nil; i++ {
		time.Sleep(time.Second)
	}
}
