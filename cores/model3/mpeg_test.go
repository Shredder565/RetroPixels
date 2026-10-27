package model3

import (
	"encoding/binary"
	"os"
	"testing"
)

// TestMPEGDecode compares the decoder with a reference decoding (made with
// ffmpeg) of the same data, when M3_MPEG_DATA and M3_MPEG_REF name them.
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
	for pos < len(data)-4 && out*2+4 <= len(ref) {
		n, used, ch := d.decodeFrame(data[pos:], pcm[:])
		if used == 0 {
			break
		}
		if frames > 0 && used > 1441 {
			break // skipped data that isn't music: the reference treats it differently
		}
		pos += used
		for i := 0; i < n*ch && out*2+2 <= len(ref); i++ {
			r := int(int16(binary.LittleEndian.Uint16(ref[out*2:])))
			if diff := r - int(pcm[i]); diff > maxDiff || -diff > maxDiff {
				if diff < 0 {
					diff = -diff
				}
				maxDiff = diff
			}
			out++
		}
		frames++
	}
	t.Logf("%d frames, %d samples, largest difference %d", frames, out, maxDiff)
	if maxDiff > 4 {
		t.Errorf("decoded samples differ from the reference by up to %d", maxDiff)
	}
}
