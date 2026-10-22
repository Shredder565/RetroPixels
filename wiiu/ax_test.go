package wiiu

import (
	"fmt"
	"math"
	"strings"
	"testing"
)

// TestAX plays axtest.c and checks its sound.
func TestAX(t *testing.T) {
	rpx := buildModule(t, buildOpts{imports: []string{"sndcore2 AXInitWithParams AXAcquireVoice AXVoiceBegin AXVoiceEnd " +
		"AXSetVoiceOffsets AXSetVoiceAdpcm AXSetVoiceSrcRatio AXSetVoiceVe AXSetVoiceDeviceMix AXSetVoiceState " +
		"AXIsVoiceRunning AXGetVoiceLoopCount AXRegisterFrameCallback"}}, "axtest.c")
	m, err := New(rpx, "axtest.rpx", nil)
	if err != nil {
		t.Fatal(err)
	}
	var log strings.Builder
	m.Log = func(s string) { log.WriteString(s + "\n") }
	var sound []float32
	for i := 0; i < 20 && m.Err == nil; i++ {
		m.RunFrame()
		sound = append(sound, m.TakeAudio()...)
	}
	if m.Err != nil {
		t.Fatalf("%v\nlog:\n%s", m.Err, log.String())
	}
	if !strings.Contains(log.String(), "ax test done") {
		t.Fatalf("log:\n%s", log.String())
	}
	var frames, ra, rb, loops int
	fmt.Sscanf(log.String()[strings.Index(log.String(), "frames"):], "frames %d running %d %d loops %d", &frames, &ra, &rb, &loops)
	// 100 ms is 33 AX frames; at 48 kHz the 96-sample loop goes round 50 times
	if frames < 30 || frames > 36 || ra != 1 || rb != 0 || loops < 45 || loops > 55 {
		t.Errorf("frames %d, running %d %d, loops %d\n%s", frames, ra, rb, loops, log.String())
	}
	// 20 frames of 48 kHz stereo
	if want := 20 * 48000 / FPS * 2; math.Abs(float64(len(sound))-want) > 2*144*2 {
		t.Errorf("%d samples, want about %.0f", len(sound), want)
	}
	// left: a square wave of +-0.5, 48 samples each way
	start := -1
	for i := 0; i+1 < len(sound)/2; i++ {
		if sound[2*i] != 0 {
			start = i
			break
		}
	}
	if start < 0 {
		t.Fatal("no sound on the left")
	}
	for i := start; i < start+960; i++ {
		want := float32(0.5)
		if (i-start)/48%2 == 1 {
			want = -0.5
		}
		if sound[2*i] != want {
			t.Fatalf("left sample %d: %v, want %v", i-start, sound[2*i], want)
		}
	}
	// right: 14 ADPCM samples of 7 << 11, at half volume, then silence
	first := -1
	for i := 0; i < len(sound)/2; i++ {
		if sound[2*i+1] != 0 {
			first = i
			break
		}
	}
	if first < 0 {
		t.Fatal("no sound on the right")
	}
	n := 0
	for i := first; i < len(sound)/2 && sound[2*i+1] != 0; i++ {
		if math.Abs(float64(sound[2*i+1])-7*2048/32768.0*0.5) > 1e-6 {
			t.Fatalf("right sample %d: %v", i-first, sound[2*i+1])
		}
		n++
	}
	if n != 14 {
		t.Errorf("%d right samples, want 14", n)
	}
}
