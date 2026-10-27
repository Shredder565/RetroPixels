package ps3

import (
	"encoding/binary"
	"fmt"
	"math"
	"math/rand"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// haveLAME: ffmpeg with the LAME MP3 encoder (the tests make MP3s with it)
func haveLAME(t *testing.T) {
	out, err := exec.Command("ffmpeg", "-hide_banner", "-encoders").Output()
	if err != nil || !strings.Contains(string(out), "libmp3lame") {
		t.Skip("no ffmpeg with libmp3lame")
	}
}

// TestMP3AgainstFFmpeg encodes test sounds with ffmpeg (LAME) in many
// formats, decodes them with ffmpeg and with our decoder, and compares.
func TestMP3AgainstFFmpeg(t *testing.T) {
	haveLAME(t)
	dir := t.TempDir()
	// a test sound: tones, sweeps, noise bursts and clicks (for short
	// blocks), different in the two channels
	const rate = 48000
	n := rate * 4
	rng := rand.New(rand.NewSource(1))
	pcm := make([]byte, 0, n*8)
	for i := 0; i < n; i++ {
		tt := float64(i) / rate
		l := 0.3*math.Sin(2*math.Pi*440*tt) + 0.2*math.Sin(2*math.Pi*(200+3000*tt)*tt)
		r := 0.25*math.Sin(2*math.Pi*660*tt) + 0.1*math.Sin(2*math.Pi*5000*tt)
		if (i/9000)%3 == 1 {
			l += 0.3 * (rng.Float64()*2 - 1)
			r += 0.2 * (rng.Float64()*2 - 1)
		}
		if i%12000 < 40 {
			l += 0.6
			r -= 0.5
		}
		pcm = binary.LittleEndian.AppendUint32(pcm, math.Float32bits(float32(l)))
		pcm = binary.LittleEndian.AppendUint32(pcm, math.Float32bits(float32(r)))
	}
	raw := filepath.Join(dir, "in.raw")
	os.WriteFile(raw, pcm, 0o644)

	cases := []struct {
		rate, ch int
		args     []string
	}{
		{44100, 1, []string{"-b:a", "96k"}},
		{44100, 2, []string{"-b:a", "128k"}},
		{44100, 2, []string{"-b:a", "128k", "-joint_stereo", "0"}},
		{48000, 2, []string{"-b:a", "320k"}},
		{32000, 2, []string{"-b:a", "64k"}},
		{44100, 2, []string{"-q:a", "2"}},
		{22050, 2, []string{"-b:a", "64k"}},
		{24000, 1, []string{"-b:a", "32k"}},
		{16000, 2, []string{"-b:a", "24k"}},
		{11025, 1, []string{"-b:a", "16k"}},
		{8000, 1, []string{"-b:a", "8k"}},
	}
	for _, c := range cases {
		name := fmt.Sprintf("%d_%d_%v", c.rate, c.ch, c.args)
		t.Run(name, func(t *testing.T) {
			mp3 := filepath.Join(dir, "t.mp3")
			args := []string{"-y", "-loglevel", "error", "-f", "f32le", "-ar", "48000", "-ac", "2", "-i", raw,
				"-ar", fmt.Sprint(c.rate), "-ac", fmt.Sprint(c.ch), "-c:a", "libmp3lame", "-write_xing", "0", "-id3v2_version", "0"}
			args = append(args, c.args...)
			if out, err := exec.Command("ffmpeg", append(args, mp3)...).CombinedOutput(); err != nil {
				t.Fatalf("encode: %v %s", err, out)
			}
			ref := filepath.Join(dir, "ref.raw")
			if out, err := exec.Command("ffmpeg", "-y", "-loglevel", "error", "-i", mp3, "-f", "f32le", "-c:a", "pcm_f32le", ref).CombinedOutput(); err != nil {
				t.Fatalf("decode: %v %s", err, out)
			}
			data, _ := os.ReadFile(mp3)
			rb, _ := os.ReadFile(ref)
			want := make([]float32, len(rb)/4)
			for i := range want {
				want[i] = math.Float32frombits(binary.LittleEndian.Uint32(rb[4*i:]))
			}
			got := mp3DecodeAll(data)
			snr, off := mp3Compare(got, want, c.ch)
			t.Logf("%d samples (ffmpeg %d), offset %d: SNR %.1f dB", len(got), len(want), off, snr)
			if snr < 100 {
				t.Errorf("SNR %.1f dB", snr)
			}
		})
	}
}

// mp3DecodeAll decodes a whole file to interleaved samples.
func mp3DecodeAll(data []byte) []float32 {
	var d mp3Dec
	var out [2][1152]float32
	var all []float32
	for len(data) >= 4 {
		h, ok := mp3ParseHeader(data)
		if !ok || len(data) < h.size {
			data = data[1:]
			continue
		}
		_, n, err := d.decode(data, &out)
		data = data[h.size:]
		if err != nil {
			continue
		}
		for i := 0; i < n; i++ {
			for ch := 0; ch < h.channels; ch++ {
				all = append(all, out[ch][i])
			}
		}
	}
	return all
}

// mp3Compare: the SNR of got against want, at the best alignment.
func mp3Compare(got, want []float32, ch int) (float64, int) {
	best, bestOff := -1e9, 0
	for off := -2000; off <= 2000; off++ {
		var sig, noise float64
		cnt := 0
		for i := 4000; i+4000 < len(want); i += 7 {
			j := i + off*ch
			if j < 0 || j >= len(got) {
				continue
			}
			sig += float64(want[i]) * float64(want[i])
			e := float64(got[j] - want[i])
			noise += e * e
			cnt++
		}
		if cnt < 1000 {
			continue
		}
		snr := 10 * math.Log10(sig/max(noise, 1e-30))
		if snr > best {
			best, bestOff = snr, off
		}
	}
	return best, bestOff
}

// TestMP3StateSaveLoad: decoding with the state saved and restored between
// frames (as the game's engine swaps streams) gives the same sound.
func TestMP3StateSaveLoad(t *testing.T) {
	haveLAME(t)
	mp3 := filepath.Join(t.TempDir(), "t.mp3")
	if out, err := exec.Command("ffmpeg", "-y", "-loglevel", "error", "-f", "lavfi", "-i", "sine=frequency=500:duration=3",
		"-af", "volume=0.5", "-ar", "44100", "-ac", "1", "-c:a", "libmp3lame", "-b:a", "96k", "-write_xing", "0", mp3).CombinedOutput(); err != nil {
		t.Fatalf("encode: %v %s", err, out)
	}
	data, _ := os.ReadFile(mp3)
	want := mp3DecodeAll(data)
	var got []float32
	ctx := make([]byte, mp3StateSize)
	var out [2][1152]float32
	var d mp3Dec
	for len(data) >= 4 {
		h, ok := mp3ParseHeader(data)
		if !ok || len(data) < h.size {
			data = data[1:]
			continue
		}
		d.load(ctx)
		_, n, err := d.decode(data, &out)
		d.save(ctx)
		data = data[h.size:]
		if err == nil {
			got = append(got, out[0][:n]...)
		}
	}
	snr, off := mp3Compare(got, want, 1)
	if off != 0 || snr < 100 {
		t.Fatalf("SNR %.1f dB at offset %d", snr, off)
	}
}
