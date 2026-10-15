package main

// A decoder for "BSDIFF40" patches (the format Archipelago's patch files use
// for their .bsdiff4 parts), written for RetroPixels.
//
// Layout: the 8-byte magic, three 8-byte numbers (length of the compressed
// control block, length of the compressed difference block, size of the new
// file), then the three bzip2 streams: control, difference and extra.
// The control block is a list of (add, copy, seek) triples. For each one,
// "add" bytes of the new file are the difference bytes added to the old
// file's bytes, "copy" bytes come straight from the extra block, and the read
// position in the old file then moves by "seek".
//
// Numbers are 8 bytes, little-endian magnitude with the sign in the top bit.

import (
	"bytes"
	"compress/bzip2"
	"errors"
	"fmt"
	"io"
)

const bsdiffMaxOutput = 1 << 30 // nothing patched for a console comes near this

var errBadBsdiff = errors.New("the patch data is damaged (bsdiff4)")

// bsdiffInt reads one sign-and-magnitude number.
func bsdiffInt(b []byte) int64 {
	var v uint64
	for i := 7; i >= 0; i-- {
		v = v<<8 | uint64(b[i])
	}
	n := int64(v &^ (1 << 63))
	if v&(1<<63) != 0 {
		n = -n
	}
	return n
}

// readBz2 decompresses one bzip2 stream, refusing more than limit bytes.
func readBz2(b []byte, limit int64) ([]byte, error) {
	if len(b) == 0 {
		return nil, nil
	}
	out, err := io.ReadAll(io.LimitReader(bzip2.NewReader(bytes.NewReader(b)), limit+1))
	if err != nil {
		return nil, fmt.Errorf("%w: %v", errBadBsdiff, err)
	}
	if int64(len(out)) > limit {
		return nil, errBadBsdiff
	}
	return out, nil
}

// bsdiff4Patch applies a BSDIFF40 patch to old and returns the new file.
func bsdiff4Patch(old, patch []byte) ([]byte, error) {
	if len(patch) < 32 || string(patch[:8]) != "BSDIFF40" {
		return nil, errors.New("that isn't a bsdiff4 patch")
	}
	lenCtrl := bsdiffInt(patch[8:])
	lenDiff := bsdiffInt(patch[16:])
	newSize := bsdiffInt(patch[24:])
	rest := int64(len(patch) - 32)
	if lenCtrl < 0 || lenDiff < 0 || newSize < 0 || newSize > bsdiffMaxOutput || lenCtrl > rest || lenDiff > rest-lenCtrl {
		return nil, errBadBsdiff
	}
	body := patch[32:]
	// Each control triple is 24 bytes and moves at least... nothing, in theory;
	// a sane patch has no more triples than output bytes plus one.
	ctrl, err := readBz2(body[:lenCtrl], (newSize+1)*24)
	if err != nil {
		return nil, err
	}
	diff, err := readBz2(body[lenCtrl:lenCtrl+lenDiff], newSize)
	if err != nil {
		return nil, err
	}
	extra, err := readBz2(body[lenCtrl+lenDiff:], newSize)
	if err != nil {
		return nil, err
	}
	if len(ctrl)%24 != 0 {
		return nil, errBadBsdiff
	}

	out := make([]byte, newSize)
	var newPos, oldPos, diffPos, extraPos int64
	oldLen := int64(len(old))
	for c := 0; c+24 <= len(ctrl); c += 24 {
		add := bsdiffInt(ctrl[c:])
		cp := bsdiffInt(ctrl[c+8:])
		seek := bsdiffInt(ctrl[c+16:])
		if add < 0 || cp < 0 || add > newSize-newPos || add > int64(len(diff))-diffPos {
			return nil, errBadBsdiff
		}
		for i := int64(0); i < add; i++ {
			v := diff[diffPos+i]
			if o := oldPos + i; o >= 0 && o < oldLen {
				v += old[o]
			}
			out[newPos+i] = v
		}
		newPos += add
		oldPos += add
		diffPos += add
		if cp > newSize-newPos || cp > int64(len(extra))-extraPos {
			return nil, errBadBsdiff
		}
		copy(out[newPos:newPos+cp], extra[extraPos:extraPos+cp])
		newPos += cp
		extraPos += cp
		oldPos += seek
	}
	if newPos != newSize {
		return nil, errBadBsdiff
	}
	return out, nil
}
