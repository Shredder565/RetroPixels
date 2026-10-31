// Package stream sends a game live to YouTube: ffmpeg grabs the part of the
// screen the game is shown in (whatever is in front there: RetroPixels' own
// picture or a docked engine's window) with the PC's sound, and pushes it to
// YouTube's ingest server with the player's stream key. ChatURL finds the
// page of the stream's live chat.
package stream

import (
	"errors"
	"fmt"
	"net/url"
	"regexp"
	"strings"
)

// IngestURL is YouTube's primary RTMP ingest server.
const IngestURL = "rtmp://a.rtmp.youtube.com/live2"

// Quality is one choice of picture size and bitrate.
type Quality struct {
	ID      string
	Label   string
	Height  int // the picture is scaled to this height (keeping its shape)
	FPS     int
	VideoKb int // video bitrate, kbit/s
}

// Qualities: YouTube's recommended bitrates for these sizes.
var Qualities = []Quality{
	{"720p30", "720p, 30 fps (4.5 Mbit/s)", 720, 30, 4500},
	{"720p60", "720p, 60 fps (6 Mbit/s)", 720, 60, 6000},
	{"1080p30", "1080p, 30 fps (8 Mbit/s)", 1080, 30, 8000},
	{"1080p60", "1080p, 60 fps (12 Mbit/s)", 1080, 60, 12000},
}

// QualityOf finds a quality by ID; the first (720p30) when it's unknown.
func QualityOf(id string) Quality {
	for _, q := range Qualities {
		if q.ID == id {
			return q
		}
	}
	return Qualities[0]
}

// Region is the part of the screen that's streamed, in physical pixels.
type Region struct{ X, Y, W, H int }

// Audio is the raw sound ffmpeg reads on its standard input: interleaved
// samples, 32-bit float (Float) or 16-bit integer, little-endian. Rate 0:
// no sound (silence is sent, YouTube wants an audio track).
type Audio struct {
	Rate, Channels int
	Float          bool
}

var reKey = regexp.MustCompile(`^[A-Za-z0-9_-]{8,64}$`)

// CheckKey tells whether s looks like a YouTube stream key (xxxx-xxxx-xxxx-xxxx-xxxx).
func CheckKey(s string) error {
	s = strings.TrimSpace(s)
	if s == "" {
		return errors.New("there's no stream key yet: copy it from YouTube Studio (Go live > Stream > Stream key)")
	}
	if !reKey.MatchString(s) {
		return errors.New("that doesn't look like a YouTube stream key: copy it from YouTube Studio (Go live > Stream > Stream key)")
	}
	return nil
}

// Args is ffmpeg's command line: the screen region at the quality's frame
// rate, the sound from standard input, H.264 + AAC in FLV to YouTube.
func Args(r Region, a Audio, q Quality, key string) []string {
	w, h := r.W&^1, r.H&^1 // H.264 needs even sizes
	args := []string{"-hide_banner", "-loglevel", "warning", "-nostats",
		"-thread_queue_size", "512",
		"-f", "gdigrab", "-framerate", fmt.Sprint(q.FPS), "-draw_mouse", "0",
		"-offset_x", fmt.Sprint(r.X), "-offset_y", fmt.Sprint(r.Y), "-video_size", fmt.Sprintf("%dx%d", w, h), "-i", "desktop"}
	if a.Rate > 0 && a.Channels > 0 {
		f := "s16le"
		if a.Float {
			f = "f32le"
		}
		args = append(args, "-thread_queue_size", "1024", "-f", f, "-ar", fmt.Sprint(a.Rate), "-ac", fmt.Sprint(a.Channels), "-i", "pipe:0",
			"-shortest") // the sound input closing ends the stream properly
	} else {
		args = append(args, "-f", "lavfi", "-i", "anullsrc=channel_layout=stereo:sample_rate=48000")
	}
	height := q.Height
	if h < height {
		height = h &^ 1 // never scaled up
	}
	args = append(args,
		"-map", "0:v", "-map", "1:a",
		"-vf", fmt.Sprintf("scale=-2:%d:flags=bicubic,format=yuv420p", height),
		"-c:v", "libx264", "-preset", "veryfast", "-tune", "zerolatency",
		"-b:v", fmt.Sprintf("%dk", q.VideoKb), "-maxrate", fmt.Sprintf("%dk", q.VideoKb), "-bufsize", fmt.Sprintf("%dk", 2*q.VideoKb),
		"-g", fmt.Sprint(2*q.FPS), "-keyint_min", fmt.Sprint(2*q.FPS), "-sc_threshold", "0",
		"-c:a", "aac", "-b:a", "160k", "-ar", "48000", "-ac", "2",
		"-f", "flv", IngestURL+"/"+strings.TrimSpace(key))
	return args
}

// Redact hides the stream key in text (ffmpeg's messages, a log).
func Redact(s, key string) string {
	if k := strings.TrimSpace(key); k != "" {
		s = strings.ReplaceAll(s, k, "<stream key>")
	}
	return s
}

var (
	reVideoID = regexp.MustCompile(`^[A-Za-z0-9_-]{11}$`)
	reHandle  = regexp.MustCompile(`^@[A-Za-z0-9._-]{3,30}$`)
	reChannel = regexp.MustCompile(`^UC[A-Za-z0-9_-]{22}$`)
)

// Target is what the player gave for the chat: a stream (video ID) or a
// channel (whose current live stream is looked up).
type Target struct {
	VideoID string
	Channel string // "@name" or a "UC…" channel ID
}

// ParseTarget reads a stream link (youtube.com/watch?v=…, youtu.be/…,
// youtube.com/live/…, studio.youtube.com/video/…/livestreaming), a video ID,
// a channel link or handle (@name, youtube.com/@name, youtube.com/channel/UC…).
func ParseTarget(s string) (Target, error) {
	s = strings.TrimSpace(s)
	if s == "" {
		return Target{}, errors.New("give your channel (@name) or your stream's link")
	}
	if reHandle.MatchString(s) {
		return Target{Channel: s}, nil
	}
	if reChannel.MatchString(s) {
		return Target{Channel: s}, nil
	}
	if reVideoID.MatchString(s) {
		return Target{VideoID: s}, nil
	}
	if !strings.Contains(s, "://") {
		s = "https://" + s
	}
	u, err := url.Parse(s)
	if err != nil {
		return Target{}, errors.New("that isn't a YouTube link")
	}
	host := strings.TrimPrefix(strings.ToLower(u.Hostname()), "www.")
	host = strings.TrimPrefix(host, "m.")
	parts := strings.FieldsFunc(u.Path, func(r rune) bool { return r == '/' })
	switch host {
	case "youtu.be":
		if len(parts) > 0 && reVideoID.MatchString(parts[0]) {
			return Target{VideoID: parts[0]}, nil
		}
	case "youtube.com", "studio.youtube.com":
		if v := u.Query().Get("v"); reVideoID.MatchString(v) {
			return Target{VideoID: v}, nil
		}
		if len(parts) >= 2 && (parts[0] == "live" || parts[0] == "video" || parts[0] == "shorts" || parts[0] == "embed") && reVideoID.MatchString(parts[1]) {
			return Target{VideoID: parts[1]}, nil
		}
		if len(parts) >= 1 && reHandle.MatchString(parts[0]) {
			return Target{Channel: parts[0]}, nil
		}
		if len(parts) >= 2 && parts[0] == "channel" && reChannel.MatchString(parts[1]) {
			return Target{Channel: parts[1]}, nil
		}
	}
	return Target{}, errors.New("that isn't a YouTube stream or channel link")
}

// ChatPage is the pop-out live chat of a stream.
func ChatPage(videoID string) string {
	return "https://www.youtube.com/live_chat?is_popout=1&v=" + videoID
}

// LivePage is a channel's page for its current live stream.
func LivePage(channel string) string {
	if strings.HasPrefix(channel, "@") {
		return "https://www.youtube.com/" + channel + "/live"
	}
	return "https://www.youtube.com/channel/" + channel + "/live"
}

var reCanonical = regexp.MustCompile(`<link rel="canonical" href="https://www\.youtube\.com/watch\?v=([A-Za-z0-9_-]{11})"`)

// LiveVideoID finds the video ID in a channel's /live page (its canonical
// link), "" when the channel isn't live (the page is then the channel's own).
func LiveVideoID(page string) string {
	if m := reCanonical.FindStringSubmatch(page); m != nil {
		return m[1]
	}
	return ""
}
