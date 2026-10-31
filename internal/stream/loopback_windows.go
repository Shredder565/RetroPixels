package stream

// The PC's sound, as it goes to the speakers (WASAPI loopback on the default
// playback device): RetroPixels' own games and the engines' alike. Written
// as raw samples for ffmpeg; while nothing plays, silence is written so the
// stream's sound keeps time with its picture.

import (
	"fmt"
	"io"
	"runtime"
	"syscall"
	"time"
	"unsafe"
)

var (
	modOle32          = syscall.NewLazyDLL("ole32.dll")
	pCoInitializeEx   = modOle32.NewProc("CoInitializeEx")
	pCoUninitialize   = modOle32.NewProc("CoUninitialize")
	pCoCreateInstance = modOle32.NewProc("CoCreateInstance")
	pCoTaskMemFree    = modOle32.NewProc("CoTaskMemFree")
)

type guid struct {
	D1     uint32
	D2, D3 uint16
	D4     [8]byte
}

var (
	clsidMMDeviceEnumerator = guid{0xBCDE0395, 0xE52F, 0x467C, [8]byte{0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}}
	iidIMMDeviceEnumerator  = guid{0xA95664D2, 0x9614, 0x4F35, [8]byte{0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}}
	iidIAudioClient         = guid{0x1CB9AD4C, 0xDBFA, 0x4C32, [8]byte{0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}}
	iidIAudioCaptureClient  = guid{0xC8ADBD64, 0xE71E, 0x48A0, [8]byte{0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17}}
	subtypeFloat            = guid{0x00000003, 0x0000, 0x0010, [8]byte{0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}}
)

const (
	clsctxAll            = 0x17
	eRender              = 0
	eConsole             = 0
	sharedMode           = 0
	streamFlagsLoopback  = 0x00020000
	bufferFlagsSilent    = 0x2
	waveFormatIEEEFloat  = 3
	waveFormatExtensible = 0xFFFE

	vtRelease                 = 2
	vtGetDefaultAudioEndpoint = 4  // IMMDeviceEnumerator
	vtActivate                = 3  // IMMDevice
	vtInitialize              = 3  // IAudioClient
	vtGetMixFormat            = 8  // IAudioClient
	vtStart                   = 10 // IAudioClient
	vtStop                    = 11 // IAudioClient
	vtGetService              = 14 // IAudioClient
	vtGetBuffer               = 3  // IAudioCaptureClient
	vtReleaseBuffer           = 4  // IAudioCaptureClient
	vtGetNextPacketSize       = 5  // IAudioCaptureClient
)

// waveFormatEx is WAVEFORMATEX followed by WAVEFORMATEXTENSIBLE's extra part.
type waveFormatEx struct {
	FormatTag      uint16
	Channels       uint16
	SamplesPerSec  uint32
	AvgBytesPerSec uint32
	BlockAlign     uint16
	BitsPerSample  uint16
	Size           uint16
	ValidBits      uint16
	ChannelMask    uint32
	SubFormat      guid
}

func vcall(obj unsafe.Pointer, slot int, args ...uintptr) uintptr {
	vtbl := *(*unsafe.Pointer)(obj)
	fn := *(*uintptr)(unsafe.Add(vtbl, slot*int(unsafe.Sizeof(uintptr(0)))))
	r, _, _ := syscall.SyscallN(fn, append([]uintptr{uintptr(obj)}, args...)...)
	return r
}

func hr(r uintptr, what string) error {
	if int32(r) < 0 {
		return fmt.Errorf("%s failed (0x%08X)", what, uint32(r))
	}
	return nil
}

// Loopback is a running capture of the PC's sound.
type Loopback struct {
	Format Audio
	stop   chan struct{}
	done   chan struct{}
	Err    error // why it stopped early (set before done closes)
}

// StartLoopback starts capturing the PC's sound into w, in the format it
// returns (the playback device's own). Close stops it.
func StartLoopback(w io.Writer) (*Loopback, error) {
	l := &Loopback{stop: make(chan struct{}), done: make(chan struct{})}
	ready := make(chan error, 1)
	go l.run(w, ready)
	if err := <-ready; err != nil {
		return nil, err
	}
	return l, nil
}

// Close stops the capture and waits for it to end.
func (l *Loopback) Close() {
	select {
	case <-l.stop:
	default:
		close(l.stop)
	}
	<-l.done
}

func (l *Loopback) run(w io.Writer, ready chan<- error) {
	defer close(l.done)
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	pCoInitializeEx.Call(0, 0) // COINIT_MULTITHREADED
	defer pCoUninitialize.Call()

	fail := func(err error) {
		ready <- err
	}
	var enum, dev, client, capture unsafe.Pointer
	defer func() {
		for _, o := range []unsafe.Pointer{capture, client, dev, enum} {
			if o != nil {
				vcall(o, vtRelease)
			}
		}
	}()
	r, _, _ := pCoCreateInstance.Call(uintptr(unsafe.Pointer(&clsidMMDeviceEnumerator)), 0, clsctxAll,
		uintptr(unsafe.Pointer(&iidIMMDeviceEnumerator)), uintptr(unsafe.Pointer(&enum)))
	if err := hr(r, "opening the sound devices"); err != nil {
		fail(err)
		return
	}
	if err := hr(vcall(enum, vtGetDefaultAudioEndpoint, eRender, eConsole, uintptr(unsafe.Pointer(&dev))), "finding the speakers"); err != nil {
		fail(err)
		return
	}
	if err := hr(vcall(dev, vtActivate, uintptr(unsafe.Pointer(&iidIAudioClient)), clsctxAll, 0, uintptr(unsafe.Pointer(&client))), "opening the speakers"); err != nil {
		fail(err)
		return
	}
	var pf *waveFormatEx
	if err := hr(vcall(client, vtGetMixFormat, uintptr(unsafe.Pointer(&pf))), "reading the sound format"); err != nil {
		fail(err)
		return
	}
	defer pCoTaskMemFree.Call(uintptr(unsafe.Pointer(pf)))
	f := *pf
	isFloat := f.FormatTag == waveFormatIEEEFloat || (f.FormatTag == waveFormatExtensible && f.SubFormat == subtypeFloat)
	switch {
	case isFloat && f.BitsPerSample == 32, !isFloat && f.BitsPerSample == 16:
	default:
		fail(fmt.Errorf("the speakers' sound format (%d-bit) isn't supported", f.BitsPerSample))
		return
	}
	l.Format = Audio{Rate: int(f.SamplesPerSec), Channels: int(f.Channels), Float: isFloat}
	const bufferDuration = 10_000_000 // 1 s, in 100 ns units
	if err := hr(vcall(client, vtInitialize, sharedMode, streamFlagsLoopback, bufferDuration, 0, uintptr(unsafe.Pointer(pf)), 0), "starting the sound capture"); err != nil {
		fail(err)
		return
	}
	if err := hr(vcall(client, vtGetService, uintptr(unsafe.Pointer(&iidIAudioCaptureClient)), uintptr(unsafe.Pointer(&capture))), "starting the sound capture"); err != nil {
		fail(err)
		return
	}
	if err := hr(vcall(client, vtStart), "starting the sound capture"); err != nil {
		fail(err)
		return
	}
	defer vcall(client, vtStop)
	ready <- nil

	frameBytes := int(f.BlockAlign)
	start := time.Now()
	written := int64(0) // frames
	silence := make([]byte, frameBytes*int(f.SamplesPerSec)/10)
	tick := time.NewTicker(10 * time.Millisecond)
	defer tick.Stop()
	for {
		select {
		case <-l.stop:
			return
		case <-tick.C:
		}
		for {
			var n uint32
			if hr(vcall(capture, vtGetNextPacketSize, uintptr(unsafe.Pointer(&n))), "") != nil || n == 0 {
				break
			}
			var data unsafe.Pointer
			var frames, flags uint32
			if err := hr(vcall(capture, vtGetBuffer, uintptr(unsafe.Pointer(&data)), uintptr(unsafe.Pointer(&frames)),
				uintptr(unsafe.Pointer(&flags)), 0, 0), "reading the sound"); err != nil {
				l.Err = err
				return
			}
			size := int(frames) * frameBytes
			var err error
			if flags&bufferFlagsSilent != 0 || data == nil {
				err = writeSilence(w, silence, size)
			} else {
				_, err = w.Write(unsafe.Slice((*byte)(data), size))
			}
			vcall(capture, vtReleaseBuffer, uintptr(frames))
			if err != nil {
				l.Err = err
				return
			}
			written += int64(frames)
		}
		// Nothing playing: Windows sends no packets, so silence fills the gap
		// (kept 50 ms behind real time so real sound isn't pushed late).
		want := int64(time.Since(start).Seconds()*float64(f.SamplesPerSec)) - int64(f.SamplesPerSec)/20
		if gap := want - written; gap > 0 {
			if err := writeSilence(w, silence, int(gap)*frameBytes); err != nil {
				l.Err = err
				return
			}
			written = want
		}
	}
}

func writeSilence(w io.Writer, zero []byte, n int) error {
	for n > 0 {
		k := min(n, len(zero))
		if _, err := w.Write(zero[:k]); err != nil {
			return err
		}
		n -= k
	}
	return nil
}
