//go:build windows

package main

import (
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"runtime"
	"sync"
	"syscall"
	"time"
	"unsafe"
)

var (
	modUser32 = syscall.NewLazyDLL("user32.dll")
	modOle32  = syscall.NewLazyDLL("ole32.dll")
	modDXGI   = syscall.NewLazyDLL("dxgi.dll")
	modMMDev  = syscall.NewLazyDLL("Mmdevapi.dll")
	modKernel = syscall.NewLazyDLL("kernel32.dll")

	pEnumWindows              = modUser32.NewProc("EnumWindows")
	pGetWindowThreadProcessId = modUser32.NewProc("GetWindowThreadProcessId")
	pIsWindowVisible          = modUser32.NewProc("IsWindowVisible")
	pIsIconic                 = modUser32.NewProc("IsIconic")
	pGetWindowTextW           = modUser32.NewProc("GetWindowTextW")
	pGetClassNameW            = modUser32.NewProc("GetClassNameW")
	pGetClientRect            = modUser32.NewProc("GetClientRect")
	pClientToScreen           = modUser32.NewProc("ClientToScreen")
	pMonitorFromWindow        = modUser32.NewProc("MonitorFromWindow")
	pSetDpiCtx                = modUser32.NewProc("SetProcessDpiAwarenessContext")
	pSetDPIAware              = modUser32.NewProc("SetProcessDPIAware")

	pCoInitializeEx   = modOle32.NewProc("CoInitializeEx")
	pCoUninitialize   = modOle32.NewProc("CoUninitialize")
	pCoCreateInstance = modOle32.NewProc("CoCreateInstance")
	pCoTaskMemFree    = modOle32.NewProc("CoTaskMemFree")

	pCreateDXGIFactory1 = modDXGI.NewProc("CreateDXGIFactory1")

	pActivateAudioInterfaceAsync = modMMDev.NewProc("ActivateAudioInterfaceAsync")

	pCreateEventW        = modKernel.NewProc("CreateEventW")
	pSetEvent            = modKernel.NewProc("SetEvent")
	pWaitForSingleObject = modKernel.NewProc("WaitForSingleObject")
	pCloseHandle         = modKernel.NewProc("CloseHandle")
)

// Window rectangles must be in real pixels, not scaled ones.
func init() {
	if pSetDpiCtx.Find() == nil {
		const perMonitorV2 = ^uintptr(3) // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 (-4)
		if r, _, _ := pSetDpiCtx.Call(perMonitorV2); r != 0 {
			return
		}
	}
	if pSetDPIAware.Find() == nil {
		pSetDPIAware.Call()
	}
}

func hiddenCommand(name string, args ...string) *exec.Cmd {
	cmd := exec.Command(name, args...)
	cmd.SysProcAttr = &syscall.SysProcAttr{HideWindow: true, CreationFlags: createNoWindow}
	return cmd
}

// ---------------- COM helpers ----------------

type guid struct {
	D1 uint32
	D2 uint16
	D3 uint16
	D4 [8]byte
}

func mkGUID(d1 uint32, d2, d3 uint16, d4 ...byte) guid {
	g := guid{D1: d1, D2: d2, D3: d3}
	copy(g.D4[:], d4)
	return g
}

var (
	clsidMMDeviceEnumerator = mkGUID(0xBCDE0395, 0xE52F, 0x467C, 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E)
	iidIMMDeviceEnumerator  = mkGUID(0xA95664D2, 0x9614, 0x4F35, 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6)
	iidIAudioClient         = mkGUID(0x1CB9AD4C, 0xDBFA, 0x4C32, 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2)
	iidIAudioCaptureClient  = mkGUID(0xC8ADBD64, 0xE71E, 0x48A0, 0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17)
	iidIUnknown             = mkGUID(0x00000000, 0x0000, 0x0000, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46)
	iidIAgileObject         = mkGUID(0x94EA2B94, 0xE9CC, 0x49E0, 0xC0, 0xFF, 0xEE, 0x64, 0xCA, 0x8F, 0x5B, 0x90)
	iidActivateHandler      = mkGUID(0x41D949AB, 0x9862, 0x444A, 0x80, 0xF6, 0xC2, 0x61, 0x33, 0x4D, 0xA5, 0xEB)
	iidIDXGIFactory1        = mkGUID(0x770AAE78, 0xF26F, 0x4DBA, 0xA8, 0x29, 0x25, 0x3C, 0x83, 0xD1, 0xB3, 0x87)
	subtypeFloat            = mkGUID(0x00000003, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71)
)

// comCall calls method n of a COM object's vtable.
func comCall(obj uintptr, n int, args ...uintptr) uintptr {
	vtbl := *(*uintptr)(unsafe.Pointer(obj))
	fn := *(*uintptr)(unsafe.Pointer(vtbl + uintptr(n)*unsafe.Sizeof(uintptr(0))))
	r, _, _ := syscall.SyscallN(fn, append([]uintptr{obj}, args...)...)
	return r
}

func comRelease(obj uintptr) {
	if obj != 0 {
		comCall(obj, 2)
	}
}

func failed(hr uintptr) bool { return int32(hr) < 0 }

func hrErr(what string, hr uintptr) error { return fmt.Errorf("%s failed (0x%08X)", what, uint32(hr)) }

// ---------------- windows & monitors ----------------

type winRECT struct{ L, T, R, B int32 }
type winPOINT struct{ X, Y int32 }

var (
	enumMu    sync.Mutex
	enumPID   uint32
	enumFound []uintptr
	enumCB    = syscall.NewCallback(enumWindowsProc)
)

func enumWindowsProc(hwnd, _ uintptr) uintptr {
	var pid uint32
	pGetWindowThreadProcessId.Call(hwnd, uintptr(unsafe.Pointer(&pid)))
	if pid == enumPID {
		if v, _, _ := pIsWindowVisible.Call(hwnd); v != 0 {
			enumFound = append(enumFound, hwnd)
		}
	}
	return 1
}

func windowText(proc *syscall.LazyProc, hwnd uintptr) string {
	buf := make([]uint16, 512)
	n, _, _ := proc.Call(hwnd, uintptr(unsafe.Pointer(&buf[0])), uintptr(len(buf)))
	return syscall.UTF16ToString(buf[:n])
}

type dxgiOutputDesc struct {
	DeviceName [32]uint16
	Desktop    winRECT
	Attached   int32
	Rotation   uint32
	Monitor    uintptr
}

// findOutput finds the DXGI adapter/output (what the screen grabber uses) for a monitor.
func findOutput(monitor uintptr) (adapter, output int, desk rect, err error) {
	var factory uintptr
	if hr, _, _ := pCreateDXGIFactory1.Call(uintptr(unsafe.Pointer(&iidIDXGIFactory1)), uintptr(unsafe.Pointer(&factory))); failed(hr) {
		return 0, 0, rect{}, hrErr("CreateDXGIFactory1", hr)
	}
	defer comRelease(factory)
	for a := 0; a < 16; a++ {
		var ad uintptr
		if hr := comCall(factory, 12, uintptr(a), uintptr(unsafe.Pointer(&ad))); failed(hr) { // EnumAdapters1
			break
		}
		for o := 0; o < 16; o++ {
			var out uintptr
			if hr := comCall(ad, 7, uintptr(o), uintptr(unsafe.Pointer(&out))); failed(hr) { // EnumOutputs
				break
			}
			var d dxgiOutputDesc
			comCall(out, 7, uintptr(unsafe.Pointer(&d))) // GetDesc
			comRelease(out)
			if d.Monitor == monitor {
				comRelease(ad)
				return a, o, rect{int(d.Desktop.L), int(d.Desktop.T), int(d.Desktop.R), int(d.Desktop.B)}, nil
			}
		}
		comRelease(ad)
	}
	return 0, 0, rect{}, errors.New("couldn't find the monitor the game is on")
}

// topWindows lists a process's visible top-level windows.
func topWindows(pid int) []uintptr {
	enumMu.Lock()
	enumPID, enumFound = uint32(pid), nil
	pEnumWindows.Call(enumCB, 0)
	found := enumFound
	enumMu.Unlock()
	return found
}

// findGameWindow returns the emulator's main (largest) visible window, or the
// game screen docked inside the RetroPixels window.
func findGameWindow(pid int) (gameWindow, bool) {
	if h := embeddedWindow(pid); h != 0 {
		var cr winRECT
		pGetClientRect.Call(h, uintptr(unsafe.Pointer(&cr)))
		var pt winPOINT
		pClientToScreen.Call(h, uintptr(unsafe.Pointer(&pt)))
		win := gameWindow{Handle: h, Title: windowText(pGetWindowTextW, h), Embedded: true,
			Client: rect{int(pt.X), int(pt.Y), int(pt.X + cr.R), int(pt.Y + cr.B)}}
		if win.Client.W() < 200 || win.Client.H() < 200 {
			return gameWindow{}, false
		}
		mon, _, _ := pMonitorFromWindow.Call(h, 2) // MONITOR_DEFAULTTONEAREST
		a, o, desk, err := findOutput(mon)
		if err != nil {
			return gameWindow{}, false
		}
		win.Adapter, win.OutIndex, win.Output = a, o, desk
		return win, true
	}
	found := topWindows(pid)

	var best gameWindow
	bestArea := 0
	for _, h := range found {
		if ic, _, _ := pIsIconic.Call(h); ic != 0 {
			continue
		}
		if windowText(pGetClassNameW, h) == "ConsoleWindowClass" {
			continue
		}
		var cr winRECT
		pGetClientRect.Call(h, uintptr(unsafe.Pointer(&cr)))
		var pt winPOINT
		pClientToScreen.Call(h, uintptr(unsafe.Pointer(&pt)))
		r := rect{int(pt.X), int(pt.Y), int(pt.X + cr.R), int(pt.Y + cr.B)}
		if a := r.W() * r.H(); a > bestArea {
			bestArea = a
			best = gameWindow{Handle: h, Title: windowText(pGetWindowTextW, h), Client: r}
		}
	}
	if bestArea < 200*200 {
		return gameWindow{}, false
	}
	mon, _, _ := pMonitorFromWindow.Call(best.Handle, 2) // MONITOR_DEFAULTTONEAREST
	a, o, desk, err := findOutput(mon)
	if err != nil {
		return gameWindow{}, false
	}
	best.Adapter, best.OutIndex, best.Output = a, o, desk
	return best, true
}

// ---------------- audio (WASAPI) ----------------

type waveFormatEx struct {
	FormatTag      uint16
	Channels       uint16
	SamplesPerSec  uint32
	AvgBytesPerSec uint32
	BlockAlign     uint16
	BitsPerSample  uint16
	CbSize         uint16
}

const (
	loopbackFlag       = 0x00020000
	eventCallbackFlag  = 0x00040000
	autoConvertPCMFlag = 0x80000000
	srcDefaultQuality  = 0x08000000
	bufferSilent       = 0x2
)

type wasapiCapture struct {
	pid    int // 0 = whole PC
	format audioFormat
}

func (w *wasapiCapture) Format() audioFormat { return w.format }
func (w *wasapiCapture) Source() string {
	if w.pid != 0 {
		return "game"
	}
	return "system"
}

func openProcessAudio(pid int) (audioCapture, error) {
	if pActivateAudioInterfaceAsync.Find() != nil {
		return nil, errors.New("this version of Windows can't capture one program's sound")
	}
	c := &wasapiCapture{pid: pid, format: audioFormat{Sample: "s16le", Rate: 48000, Ch: 2}}
	// Check it can be opened now, so we fall back to whole-PC sound early if not.
	errc := make(chan error, 1)
	go func() {
		runtime.LockOSThread()
		defer runtime.UnlockOSThread()
		pCoInitializeEx.Call(0, 0)
		defer pCoUninitialize.Call()
		client, err := activateProcessLoopback(pid)
		if err == nil {
			comRelease(client)
		}
		errc <- err
	}()
	if err := <-errc; err != nil {
		return nil, err
	}
	return c, nil
}

func openSystemAudio() (audioCapture, error) {
	c := &wasapiCapture{}
	errc := make(chan error, 1)
	go func() {
		runtime.LockOSThread()
		defer runtime.UnlockOSThread()
		pCoInitializeEx.Call(0, 0)
		defer pCoUninitialize.Call()
		client, wf, err := activateSystemLoopback()
		if err == nil {
			c.format = wf
			comRelease(client)
		}
		errc <- err
	}()
	if err := <-errc; err != nil {
		return nil, err
	}
	return c, nil
}

// --- completion handler COM object for ActivateAudioInterfaceAsync ---

type activateHandler struct {
	vtbl  *[4]uintptr
	event uintptr
}

var (
	handlerVtbl = [4]uintptr{
		syscall.NewCallback(handlerQI),
		syscall.NewCallback(func(this uintptr) uintptr { return 1 }),
		syscall.NewCallback(func(this uintptr) uintptr { return 1 }),
		syscall.NewCallback(handlerCompleted),
	}
	handlerMu   sync.Mutex
	handlerLive = map[uintptr]*activateHandler{}
)

func handlerQI(this, riid, ppv uintptr) uintptr {
	g := *(*guid)(unsafe.Pointer(riid))
	if g == iidIUnknown || g == iidIAgileObject || g == iidActivateHandler {
		*(*uintptr)(unsafe.Pointer(ppv)) = this
		return 0
	}
	*(*uintptr)(unsafe.Pointer(ppv)) = 0
	return 0x80004002 // E_NOINTERFACE
}

func handlerCompleted(this, op uintptr) uintptr {
	handlerMu.Lock()
	h := handlerLive[this]
	handlerMu.Unlock()
	if h != nil {
		pSetEvent.Call(h.event)
	}
	return 0
}

type propVariantBlob struct {
	VT       uint16
	_        [3]uint16
	CbSize   uint32
	_        uint32
	BlobData uintptr
}

type processLoopbackParams struct {
	ActivationType uint32 // 1 = process loopback
	TargetPID      uint32
	Mode           uint32 // 0 = include the process and its children
}

func activateProcessLoopback(pid int) (uintptr, error) {
	params := &processLoopbackParams{ActivationType: 1, TargetPID: uint32(pid), Mode: 0}
	pv := &propVariantBlob{VT: 65 /* VT_BLOB */, CbSize: uint32(unsafe.Sizeof(*params)), BlobData: uintptr(unsafe.Pointer(params))}
	ev, _, _ := pCreateEventW.Call(0, 0, 0, 0)
	if ev == 0 {
		return 0, errors.New("CreateEvent failed")
	}
	defer pCloseHandle.Call(ev)
	h := &activateHandler{vtbl: &handlerVtbl, event: ev}
	key := uintptr(unsafe.Pointer(h))
	handlerMu.Lock()
	handlerLive[key] = h
	handlerMu.Unlock()
	defer func() { handlerMu.Lock(); delete(handlerLive, key); handlerMu.Unlock() }()

	path, _ := syscall.UTF16PtrFromString(`VAD\Process_Loopback`)
	var op uintptr
	hr, _, _ := pActivateAudioInterfaceAsync.Call(uintptr(unsafe.Pointer(path)), uintptr(unsafe.Pointer(&iidIAudioClient)),
		uintptr(unsafe.Pointer(pv)), key, uintptr(unsafe.Pointer(&op)))
	runtime.KeepAlive(params)
	runtime.KeepAlive(pv)
	if failed(hr) {
		return 0, hrErr("ActivateAudioInterfaceAsync", hr)
	}
	defer comRelease(op)
	if r, _, _ := pWaitForSingleObject.Call(ev, 5000); r != 0 {
		return 0, errors.New("timed out waiting for game audio")
	}
	runtime.KeepAlive(h)
	var actHR, unk uintptr
	if hr := comCall(op, 3, uintptr(unsafe.Pointer(&actHR)), uintptr(unsafe.Pointer(&unk))); failed(hr) || failed(actHR) {
		if failed(hr) {
			return 0, hrErr("GetActivateResult", hr)
		}
		return 0, hrErr("Process audio capture", actHR)
	}
	var client uintptr
	hr = comCall(unk, 0, uintptr(unsafe.Pointer(&iidIAudioClient)), uintptr(unsafe.Pointer(&client)))
	comRelease(unk)
	if failed(hr) {
		return 0, hrErr("IAudioClient", hr)
	}
	return client, nil
}

func activateSystemLoopback() (uintptr, audioFormat, error) {
	var en uintptr
	if hr, _, _ := pCoCreateInstance.Call(uintptr(unsafe.Pointer(&clsidMMDeviceEnumerator)), 0, 0x17, /* CLSCTX_ALL */
		uintptr(unsafe.Pointer(&iidIMMDeviceEnumerator)), uintptr(unsafe.Pointer(&en))); failed(hr) {
		return 0, audioFormat{}, hrErr("MMDeviceEnumerator", hr)
	}
	defer comRelease(en)
	var dev uintptr
	if hr := comCall(en, 4, 0 /* eRender */, 0 /* eConsole */, uintptr(unsafe.Pointer(&dev))); failed(hr) {
		return 0, audioFormat{}, hrErr("GetDefaultAudioEndpoint", hr)
	}
	defer comRelease(dev)
	var client uintptr
	if hr := comCall(dev, 3, uintptr(unsafe.Pointer(&iidIAudioClient)), 0x17, 0, uintptr(unsafe.Pointer(&client))); failed(hr) {
		return 0, audioFormat{}, hrErr("Activate", hr)
	}
	var wf *waveFormatEx
	if hr := comCall(client, 8, uintptr(unsafe.Pointer(&wf))); failed(hr) || wf == nil {
		comRelease(client)
		return 0, audioFormat{}, hrErr("GetMixFormat", hr)
	}
	defer pCoTaskMemFree.Call(uintptr(unsafe.Pointer(wf)))
	af := audioFormat{Rate: int(wf.SamplesPerSec), Ch: int(wf.Channels)}
	isFloat := wf.FormatTag == 3
	if wf.FormatTag == 0xFFFE && wf.CbSize >= 22 { // WAVEFORMATEXTENSIBLE: sub-format GUID at byte 24
		sub := *(*guid)(unsafe.Pointer(uintptr(unsafe.Pointer(wf)) + 24))
		isFloat = sub == subtypeFloat
	}
	switch {
	case isFloat && wf.BitsPerSample == 32:
		af.Sample = "f32le"
	case !isFloat && wf.BitsPerSample == 16:
		af.Sample = "s16le"
	case !isFloat && wf.BitsPerSample == 32:
		af.Sample = "s32le"
	case !isFloat && wf.BitsPerSample == 24:
		af.Sample = "s24le"
	default:
		comRelease(client)
		return 0, audioFormat{}, fmt.Errorf("unsupported sound format (%d-bit)", wf.BitsPerSample)
	}
	return client, af, nil
}

// Run captures until stop closes. Gaps (Windows sends nothing during silence)
// are filled with silence so the sound stays in step with the picture.
func (w *wasapiCapture) Run(write func([]byte), stop <-chan struct{}) error {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	pCoInitializeEx.Call(0, 0)
	defer pCoUninitialize.Call()

	var client uintptr
	var err error
	block := 4
	var flags uintptr
	if w.pid != 0 {
		client, err = activateProcessLoopback(w.pid)
		if err != nil {
			return err
		}
		flags = loopbackFlag | eventCallbackFlag | autoConvertPCMFlag | srcDefaultQuality
		wf := waveFormatEx{FormatTag: 1, Channels: 2, SamplesPerSec: 48000, AvgBytesPerSec: 48000 * 4, BlockAlign: 4, BitsPerSample: 16}
		if hr := comCall(client, 3, 0, flags, 2_000_000, 0, uintptr(unsafe.Pointer(&wf)), 0); failed(hr) {
			comRelease(client)
			return hrErr("Initialize (game audio)", hr)
		}
	} else {
		var af audioFormat
		client, af, err = activateSystemLoopback()
		if err != nil {
			return err
		}
		var wf *waveFormatEx
		comCall(client, 8, uintptr(unsafe.Pointer(&wf)))
		block = int(wf.BlockAlign)
		_ = af
		flags = loopbackFlag
		hr := comCall(client, 3, 0, flags, 2_000_000, 0, uintptr(unsafe.Pointer(wf)), 0)
		pCoTaskMemFree.Call(uintptr(unsafe.Pointer(wf)))
		if failed(hr) {
			comRelease(client)
			return hrErr("Initialize (PC audio)", hr)
		}
	}
	defer comRelease(client)

	var ev uintptr
	if flags&eventCallbackFlag != 0 {
		ev, _, _ = pCreateEventW.Call(0, 0, 0, 0)
		defer pCloseHandle.Call(ev)
		if hr := comCall(client, 13, ev); failed(hr) { // SetEventHandle
			return hrErr("SetEventHandle", hr)
		}
	}
	var capture uintptr
	if hr := comCall(client, 14, uintptr(unsafe.Pointer(&iidIAudioCaptureClient)), uintptr(unsafe.Pointer(&capture))); failed(hr) {
		return hrErr("GetService", hr)
	}
	defer comRelease(capture)
	if hr := comCall(client, 10); failed(hr) { // Start
		return hrErr("Start", hr)
	}
	defer comCall(client, 11) // Stop

	rate := int64(w.format.Rate)
	start := time.Now()
	var written int64
	zeros := make([]byte, int(rate/10)*block)
	for {
		select {
		case <-stop:
			return nil
		default:
		}
		if ev != 0 {
			pWaitForSingleObject.Call(ev, 20)
		} else {
			time.Sleep(10 * time.Millisecond)
		}
		for {
			var packet uint32
			if hr := comCall(capture, 5, uintptr(unsafe.Pointer(&packet))); failed(hr) || packet == 0 {
				break
			}
			var data *byte
			var frames, bflags uint32
			if hr := comCall(capture, 3, uintptr(unsafe.Pointer(&data)), uintptr(unsafe.Pointer(&frames)),
				uintptr(unsafe.Pointer(&bflags)), 0, 0); failed(hr) {
				break
			}
			n := int(frames) * block
			buf := make([]byte, n)
			if bflags&bufferSilent == 0 && data != nil && n > 0 {
				copy(buf, unsafe.Slice(data, n))
			}
			comCall(capture, 4, uintptr(frames)) // ReleaseBuffer
			write(buf)
			written += int64(frames)
		}
		// Fill silence so the sound doesn't fall behind the video.
		expected := int64(time.Since(start).Seconds() * float64(rate))
		if lag := expected - written; lag > rate/10 {
			pad := lag - rate/50
			for pad > 0 {
				chunk := min(pad, int64(len(zeros)/block))
				write(append([]byte(nil), zeros[:chunk*int64(block)]...))
				written += chunk
				pad -= chunk
			}
		}
	}
}

var (
	pCreateNamedPipeW    = modKernel.NewProc("CreateNamedPipeW")
	pConnectNamedPipe    = modKernel.NewProc("ConnectNamedPipe")
	pDisconnectNamedPipe = modKernel.NewProc("DisconnectNamedPipe")
	pipeSeq              uint32
	pipeSeqMu            sync.Mutex
)

func newAudioPipe() (*audioPipe, error) {
	pipeSeqMu.Lock()
	pipeSeq++
	name := fmt.Sprintf(`\\.\pipe\retropixels-audio-%d-%d`, syscall.Getpid(), pipeSeq)
	pipeSeqMu.Unlock()
	n16, _ := syscall.UTF16PtrFromString(name)
	const pipeAccessOutbound, pipeTypeByteWait, rejectRemote = 0x2, 0x0, 0x8
	h, _, e := pCreateNamedPipeW.Call(uintptr(unsafe.Pointer(n16)), pipeAccessOutbound, pipeTypeByteWait|rejectRemote,
		1, 4<<20, 0, 0, 0)
	if syscall.Handle(h) == syscall.InvalidHandle {
		return nil, fmt.Errorf("CreateNamedPipe: %v", e)
	}
	var file *os.File
	var mu sync.Mutex
	return &audioPipe{
		Path: name,
		openFn: func() (io.WriteCloser, error) {
			r, _, e := pConnectNamedPipe.Call(h, 0)
			if r == 0 && e != syscall.Errno(535) { // ERROR_PIPE_CONNECTED is fine
				return nil, fmt.Errorf("ConnectNamedPipe: %v", e)
			}
			mu.Lock()
			defer mu.Unlock()
			file = os.NewFile(h, name)
			return file, nil
		},
		closeFn: func() {
			mu.Lock()
			defer mu.Unlock()
			if file != nil {
				file.Close()
			} else {
				pCloseHandle.Call(h)
			}
		},
	}, nil
}
