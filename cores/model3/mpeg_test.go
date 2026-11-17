package model3

import (
	"encoding/binary"
	"math"
	"os"
	"testing"
)

// TestMPEGDecode compares the decoder with a reference decoding (made with
// another decoder, such as ffmpeg) of the same data, when M3_MPEG_DATA and
// M3_MPEG_REF name them: every sample must match to within two 16-bit steps.
func TestMPEGDecode(t *testing.T) {
	data, err := os.ReadFile(os.Getenv("M3_MPEG_DATA"))
	if err != nil {
		t.Skip("no MPEG data")
	}
	ref, err := os.ReadFile(os.Getenv("M3_MPEG_REF"))
	if err != nil {
		t.Skip("no reference")
	}
	var d mpegDecoder
	var pcm [mpegMaxSamplesPerFrm]int16
	pos, out := 0, 0
	maxDiff, frames := 0, 0
	var sig, noise float64
	for pos < len(data)-4 && out*2+4 <= len(ref) {
		n, used, ch := d.decodeFrame(data[pos:], pcm[:])
		if used == 0 || (frames > 0 && n == 0) {
			break
		}
		if frames > 0 && used > 1441 {
			break // skipped data that isn't music: the reference treats it differently
		}
		pos += used
		for i := 0; i < n*ch && out*2+2 <= len(ref); i++ {
			r := int(int16(binary.LittleEndian.Uint16(ref[out*2:])))
			diff := r - int(pcm[i])
			maxDiff = max(maxDiff, diff, -diff)
			sig += float64(r * r)
			noise += float64(diff * diff)
			out++
		}
		frames++
	}
	snr := 10 * math.Log10(sig/max(noise, 1))
	t.Logf("%d frames, %d samples, largest difference %d, SNR %.1f dB", frames, out, maxDiff, snr)
	if maxDiff > 2 {
		t.Errorf("decoded samples differ from the reference: SNR %.1f dB, largest difference %d", snr, maxDiff)
	}
}
