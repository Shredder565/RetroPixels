package stream

import (
	"strings"
	"testing"
)

func TestArgs(t *testing.T) {
	a := strings.Join(Args(Region{100, 50, 1281, 721}, Audio{48000, 2, true}, QualityOf("720p30"), " abcd-efgh-ijkl-mnop-qrst "), " ")
	for _, want := range []string{
		"-f gdigrab -framerate 30 -draw_mouse 0 -offset_x 100 -offset_y 50 -video_size 1280x720 -i desktop",
		"-f f32le -ar 48000 -ac 2 -i pipe:0",
		"scale=-2:720", "-b:v 4500k", "-g 60",
		"-f flv rtmp://a.rtmp.youtube.com/live2/abcd-efgh-ijkl-mnop-qrst",
	} {
		if !strings.Contains(a, want) {
			t.Errorf("missing %q in %s", want, a)
		}
	}
	a = strings.Join(Args(Region{0, 0, 640, 480}, Audio{}, QualityOf("1080p60"), "k"), " ")
	if !strings.Contains(a, "anullsrc") || !strings.Contains(a, "scale=-2:480") || !strings.Contains(a, "-framerate 60") {
		t.Error(a)
	}
}

func TestCheckKeyAndRedact(t *testing.T) {
	if CheckKey("abcd-efgh-ijkl-mnop-qrst") != nil || CheckKey("") == nil || CheckKey("rtmp://x y") == nil {
		t.Error("CheckKey")
	}
	if got := Redact("failed: rtmp://a/live2/abcd-efgh", "abcd-efgh"); strings.Contains(got, "abcd") {
		t.Error(got)
	}
}

func TestParseTarget(t *testing.T) {
	for in, want := range map[string]Target{
		"https://www.youtube.com/watch?v=dQw4w9WgXcQ&t=3":            {VideoID: "dQw4w9WgXcQ"},
		"youtu.be/dQw4w9WgXcQ":                                       {VideoID: "dQw4w9WgXcQ"},
		"https://youtube.com/live/dQw4w9WgXcQ?si=x":                  {VideoID: "dQw4w9WgXcQ"},
		"https://studio.youtube.com/video/dQw4w9WgXcQ/livestreaming": {VideoID: "dQw4w9WgXcQ"},
		"dQw4w9WgXcQ":     {VideoID: "dQw4w9WgXcQ"},
		"@RetroPixelsFan": {Channel: "@RetroPixelsFan"},
		"https://www.youtube.com/@Some.One/streams":                {Channel: "@Some.One"},
		"https://www.youtube.com/channel/UCabcdefghijklmnopqrstuv": {Channel: "UCabcdefghijklmnopqrstuv"},
	} {
		got, err := ParseTarget(in)
		if err != nil || got != want {
			t.Errorf("%s: %+v %v", in, got, err)
		}
	}
	for _, bad := range []string{"", "https://example.com/watch?v=dQw4w9WgXcQ", "hello there"} {
		if _, err := ParseTarget(bad); err == nil {
			t.Errorf("%q accepted", bad)
		}
	}
}

func TestLiveVideoID(t *testing.T) {
	page := `<html><link rel="canonical" href="https://www.youtube.com/watch?v=dQw4w9WgXcQ"><body>`
	if LiveVideoID(page) != "dQw4w9WgXcQ" || LiveVideoID(`<link rel="canonical" href="https://www.youtube.com/@x">`) != "" {
		t.Error("LiveVideoID")
	}
	if LivePage("@x") != "https://www.youtube.com/@x/live" || ChatPage("dQw4w9WgXcQ") != "https://www.youtube.com/live_chat?is_popout=1&v=dQw4w9WgXcQ" {
		t.Error("pages")
	}
}
