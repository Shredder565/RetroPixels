package main

import (
	"sync"
	"sync/atomic"
	"unsafe"
)

// Sound through waveOut (winmm): plain 16-bit stereo blocks queued to the
// Windows sound mixer. Each emulated frame's sound is one block; how many
// blocks are still waiting tells the pacer how much sound is queued.

var (
	pWaveOutOpen            = winmm.NewProc("waveOutOpen")
	pWaveOutClose           = winmm.NewProc("waveOutClose")
	pWaveOutPrepareHeader   = winmm.NewProc("waveOutPrepareHeader")
	pWaveOutUnprepareHeader = winmm.NewProc("waveOutUnprepareHeader")
	pWaveOutWrite           = winmm.NewProc("waveOutWrite")
	pWaveOutReset           = winmm.NewProc("waveOutReset")
	pWaveOutGetPosition     = winmm.NewProc("waveOutGetPosition")
)

type mmTime struct {
	Type  uint32
	Value uint32
	pad   uint32
}

const timeSamples = 2

type waveFormatEx struct {
	FormatTag      uint16
	Channels       uint16
	SamplesPerSec  uint32
	AvgBytesPerSec uint32
	BlockAlign     uint16
	BitsPerSample  uint16
	CbSize         uint16
}

type waveHdr struct {
	Data          uintptr
	BufferLength  uint32
	BytesRecorded uint32
	User          uintptr
	Flags         uint32
	Loops         uint32
	Next          uintptr
	Reserved      uintptr
}

const (
	whdrDone     = 1
	waveMapper   = 0xFFFFFFFF
	maxQueuedSec = 0.5 // never queue more than this (drops the newest instead)
)

type waveBlock struct {
	hdr     waveHdr
	data    []int16
	samples int // stereo pairs
}

type waveOut struct {
	mu      sync.Mutex
	h       uintptr
	rate    int
	playing []*waveBlock
	free    []*waveBlock
	written uint32 // sample pairs written since the last reset (the device counts the same way)
}

// openWaveOut opens the default sound device, or returns nil when there is none.
func openWaveOut(rate int) *waveOut {
	f := waveFormatEx{FormatTag: 1, Channels: 2, SamplesPerSec: uint32(rate), AvgBytesPerSec: uint32(rate * 4), BlockAlign: 4, BitsPerSample: 16}
	var h uintptr
	r, _, _ := pWaveOutOpen.Call(uintptr(unsafe.Pointer(&h)), waveMapper, uintptr(unsafe.Pointer(&f)), 0, 0, 0)
	if r != 0 || h == 0 {
		return nil
	}
	return &waveOut{h: h, rate: rate}
}

// reap takes back the blocks that have finished playing. Holds w.mu.
func (w *waveOut) reap() {
	keep := w.playing[:0]
	for _, b := range w.playing {
		if atomic.LoadUint32(&b.hdr.Flags)&whdrDone != 0 {
			pWaveOutUnprepareHeader.Call(w.h, uintptr(unsafe.Pointer(&b.hdr)), unsafe.Sizeof(b.hdr))
			w.free = append(w.free, b)
		} else {
			keep = append(keep, b)
		}
	}
	for i := len(keep); i < len(w.playing); i++ {
		w.playing[i] = nil
	}
	w.playing = keep
}

// queuedLocked: sound not yet played. The device's play position gives it to
// the sample; the blocks still out are the fallback.
func (w *waveOut) queuedLocked() float64 {
	n := 0
	for _, b := range w.playing {
		n += b.samples
	}
	t := mmTime{Type: timeSamples}
	if r, _, _ := pWaveOutGetPosition.Call(w.h, uintptr(unsafe.Pointer(&t)), unsafe.Sizeof(t)); r == 0 && t.Type == timeSamples {
		if q := int(w.written - t.Value); q >= 0 && q <= n+w.rate/10 {
			n = q
		}
	}
	return float64(n) / float64(w.rate)
}

func (w *waveOut) Queued() float64 {
	w.mu.Lock()
	defer w.mu.Unlock()
	w.reap()
	return w.queuedLocked()
}

func (w *waveOut) Write(pcm []int16) {
	if len(pcm) < 2 {
		return
	}
	w.mu.Lock()
	defer w.mu.Unlock()
	w.reap()
	if w.queuedLocked() > maxQueuedSec {
		return
	}
	var b *waveBlock
	for i, f := range w.free {
		if cap(f.data) >= len(pcm) {
			b = f
			w.free[i] = w.free[len(w.free)-1]
			w.free = w.free[:len(w.free)-1]
			break
		}
	}
	if b == nil {
		b = &waveBlock{data: make([]int16, len(pcm), len(pcm)+len(pcm)/4)}
	}
	b.data = append(b.data[:0], pcm...)
	b.samples = len(pcm) / 2
	b.hdr = waveHdr{Data: uintptr(unsafe.Pointer(&b.data[0])), BufferLength: uint32(len(b.data) * 2)}
	if r, _, _ := pWaveOutPrepareHeader.Call(w.h, uintptr(unsafe.Pointer(&b.hdr)), unsafe.Sizeof(b.hdr)); r != 0 {
		w.free = append(w.free, b)
		return
	}
	if r, _, _ := pWaveOutWrite.Call(w.h, uintptr(unsafe.Pointer(&b.hdr)), unsafe.Sizeof(b.hdr)); r != 0 {
		pWaveOutUnprepareHeader.Call(w.h, uintptr(unsafe.Pointer(&b.hdr)), unsafe.Sizeof(b.hdr))
		w.free = append(w.free, b)
		return
	}
	w.playing = append(w.playing, b)
	w.written += uint32(b.samples)
}

// Clear stops the sound and drops what was queued.
func (w *waveOut) Clear() {
	w.mu.Lock()
	defer w.mu.Unlock()
	pWaveOutReset.Call(w.h)
	w.reap()
	w.written = 0 // a reset puts the play position back to 0
}

func (w *waveOut) Close() {
	w.Clear()
	w.mu.Lock()
	defer w.mu.Unlock()
	pWaveOutClose.Call(w.h)
	w.h = 0
}

// noSound stands in when there is no sound device: the clock alone paces the game.
type noSound struct{}

func (noSound) Write([]int16)   {}
func (noSound) Queued() float64 { return -1 }
func (noSound) Clear()          {}
