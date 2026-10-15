package dolphin

// Dolphin's volume, set the way the Windows volume mixer does it: through
// Dolphin's audio session (Core Audio, audiopolicy.h). Only Dolphin's sound
// changes, not the rest of the PC, and Dolphin doesn't need to know about
// it. (The same method as RetroPixels.exe's dolvolume_windows.go.)

import (
	"math"
	"path/filepath"
	"runtime"
	"strings"
	"syscall"
	"time"
	"unsafe"
)

var (
	modOle32                    = syscall.NewLazyDLL("ole32.dll")
	modKernel32                 = syscall.NewLazyDLL("kernel32.dll")
	pCoInitializeEx             = modOle32.NewProc("CoInitializeEx")
	pCoUninitialize             = modOle32.NewProc("CoUninitialize")
	pCoCreateInstance           = modOle32.NewProc("CoCreateInstance")
	pQueryFullProcessImageNameW = modKernel32.NewProc("QueryFullProcessImageNameW")
)

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
	clsidMMDeviceEnumerator  = mkGUID(0xBCDE0395, 0xE52F, 0x467C, 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E)
	iidIMMDeviceEnumerator   = mkGUID(0xA95664D2, 0x9614, 0x4F35, 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6)
	iidIAudioSessionManager2 = mkGUID(0x77AA99A0, 0x1BD6, 0x484F, 0x8B, 0xC7, 0x2C, 0x65, 0x4C, 0x9A, 0x9B, 0x6F)
	iidIAudioSessionControl2 = mkGUID(0xBFB7FF88, 0x7239, 0x4FC9, 0x8F, 0xA2, 0x07, 0xC9, 0x50, 0xBE, 0x9C, 0x6D)
	iidISimpleAudioVolume    = mkGUID(0x87CE5498, 0x68D6, 0x44E5, 0x92, 0x15, 0x6D, 0xA4, 0x7E, 0xF8, 0x83, 0xD8)
)

// Vtable slots. Every interface starts with IUnknown: 0 QueryInterface, 1 AddRef, 2 Release.
const (
	vtQueryInterface       = 0
	vtEnumAudioEndpoints   = 3  // IMMDeviceEnumerator
	vtDevicesGetCount      = 3  // IMMDeviceCollection
	vtDevicesItem          = 4  // IMMDeviceCollection
	vtDeviceActivate       = 3  // IMMDevice
	vtGetSessionEnumerator = 5  // IAudioSessionManager2
	vtSessionsGetCount     = 3  // IAudioSessionEnumerator
	vtSessionsGet          = 4  // IAudioSessionEnumerator
	vtGetProcessId         = 14 // IAudioSessionControl2
	vtSetMasterVolume      = 3  // ISimpleAudioVolume
	vtSetMute              = 5  // ISimpleAudioVolume

	clsctxAll         = 0x17
	eRender           = 0
	deviceStateActive = 1
	processQueryLimit = 0x1000
	coinitMulti       = 0x0
	audclntNoProcess  = 0x0889000D // AUDCLNT_S_NO_SINGLE_PROCESS: a session shared by several programs
)

// floatArgsOK: SetMasterVolume takes a float, and syscall.SyscallN only takes
// integers. On windows/amd64 the Go runtime copies each of the first four
// arguments into both its integer register and the matching XMM register,
// so the float's bits passed as the 2nd argument land in XMM1, where
// SetMasterVolume reads its level. On windows/386 arguments go on the stack.
// windows/arm64 doesn't fill float registers, so there only mute is used.
const floatArgsOK = runtime.GOARCH == "amd64" || runtime.GOARCH == "386"

// com is a COM object: a pointer to its table of methods.
type com = unsafe.Pointer

// comCall calls method n of a COM object.
func comCall(obj com, n int, args ...uintptr) uintptr {
	vtbl := *(*unsafe.Pointer)(obj)
	fn := *(*uintptr)(unsafe.Add(vtbl, n*int(unsafe.Sizeof(uintptr(0)))))
	r, _, _ := syscall.SyscallN(fn, append([]uintptr{uintptr(obj)}, args...)...)
	return r
}

func comRelease(obj com) {
	if obj != nil {
		comCall(obj, 2)
	}
}

func failed(hr uintptr) bool { return int32(hr) < 0 }

func ptr[T any](p *T) uintptr { return uintptr(unsafe.Pointer(p)) }

// FollowVolume applies the volume to one Dolphin run. Dolphin only opens its
// sound a moment after it starts, so it keeps trying for 20 seconds, and for
// 3 more whenever kick fires (the setting changed). get returns the volume
// (0–100) and whether it's muted.
func FollowVolume(pid int, exe string, get func() (int, bool), kick <-chan struct{}, exited <-chan struct{}) {
	name := filepath.Base(exe)
	tick := time.NewTicker(500 * time.Millisecond)
	defer tick.Stop()
	until := time.Now().Add(20 * time.Second)
	for {
		if time.Now().Before(until) {
			v, m := get()
			SetProgramVolume(pid, name, float32(v)/100, m)
		}
		select {
		case <-exited:
			return
		case <-kick:
			if t := time.Now().Add(3 * time.Second); t.After(until) {
				until = t
			}
		case <-tick.C:
		}
	}
}

// SetProgramVolume sets the volume of every audio session (on every speaker
// or headphone output) that belongs to process pid, or to a program called
// exe as a fallback. It returns how many sessions it changed.
func SetProgramVolume(pid int, exe string, level float32, mute bool) int {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	if hr, _, _ := pCoInitializeEx.Call(0, coinitMulti); !failed(hr) {
		defer pCoUninitialize.Call()
	}
	// Out-parameters live on the heap so they can't move while Windows writes to them.
	en := new(com)
	if hr, _, _ := pCoCreateInstance.Call(ptr(&clsidMMDeviceEnumerator), 0, clsctxAll,
		ptr(&iidIMMDeviceEnumerator), ptr(en)); failed(hr) || *en == nil {
		return 0
	}
	defer comRelease(*en)
	devs := new(com)
	if hr := comCall(*en, vtEnumAudioEndpoints, eRender, deviceStateActive, ptr(devs)); failed(hr) || *devs == nil {
		return 0
	}
	defer comRelease(*devs)
	n := new(uint32)
	if hr := comCall(*devs, vtDevicesGetCount, ptr(n)); failed(hr) {
		return 0
	}
	names := map[uint32]bool{}
	done := 0
	for i := uint32(0); i < *n; i++ {
		dev := new(com)
		if hr := comCall(*devs, vtDevicesItem, uintptr(i), ptr(dev)); failed(hr) || *dev == nil {
			continue
		}
		done += setDeviceSessions(*dev, pid, exe, names, level, mute)
		comRelease(*dev)
	}
	return done
}

func setDeviceSessions(dev com, pid int, exe string, names map[uint32]bool, level float32, mute bool) int {
	mgr := new(com)
	if hr := comCall(dev, vtDeviceActivate, ptr(&iidIAudioSessionManager2), clsctxAll, 0, ptr(mgr)); failed(hr) || *mgr == nil {
		return 0
	}
	defer comRelease(*mgr)
	sessions := new(com)
	if hr := comCall(*mgr, vtGetSessionEnumerator, ptr(sessions)); failed(hr) || *sessions == nil {
		return 0
	}
	defer comRelease(*sessions)
	count := new(int32)
	if hr := comCall(*sessions, vtSessionsGetCount, ptr(count)); failed(hr) {
		return 0
	}
	done := 0
	for i := int32(0); i < *count; i++ {
		ctl := new(com)
		if hr := comCall(*sessions, vtSessionsGet, uintptr(i), ptr(ctl)); failed(hr) || *ctl == nil {
			continue
		}
		ctl2 := new(com)
		hr := comCall(*ctl, vtQueryInterface, ptr(&iidIAudioSessionControl2), ptr(ctl2))
		comRelease(*ctl)
		if failed(hr) || *ctl2 == nil {
			continue
		}
		spid := new(uint32)
		hr = comCall(*ctl2, vtGetProcessId, ptr(spid))
		if !failed(hr) && hr != audclntNoProcess && *spid != 0 && (int(*spid) == pid || isProgram(*spid, exe, names)) {
			vol := new(com)
			if hr := comCall(*ctl2, vtQueryInterface, ptr(&iidISimpleAudioVolume), ptr(vol)); !failed(hr) && *vol != nil {
				if setSessionVolume(*vol, level, mute) {
					done++
				}
				comRelease(*vol)
			}
		}
		comRelease(*ctl2)
	}
	return done
}

func setSessionVolume(vol com, level float32, mute bool) bool {
	ok := true
	if floatArgsOK {
		// ISimpleAudioVolume::SetMasterVolume(float fLevel, LPCGUID EventContext)
		ok = !failed(comCall(vol, vtSetMasterVolume, uintptr(math.Float32bits(level)), 0))
	} else if level == 0 {
		mute = true
	}
	m := uintptr(0)
	if mute {
		m = 1
	}
	// ISimpleAudioVolume::SetMute(BOOL bMute, LPCGUID EventContext)
	return !failed(comCall(vol, vtSetMute, m, 0)) && ok
}

// isProgram tells whether process pid runs a program called exe (e.g. Dolphin.exe).
func isProgram(pid uint32, exe string, cache map[uint32]bool) bool {
	if r, ok := cache[pid]; ok {
		return r
	}
	r := false
	if h, err := syscall.OpenProcess(processQueryLimit, false, pid); err == nil {
		buf := make([]uint16, 1024)
		size := new(uint32)
		*size = uint32(len(buf))
		if ok, _, _ := pQueryFullProcessImageNameW.Call(uintptr(h), 0, uintptr(unsafe.Pointer(&buf[0])), ptr(size)); ok != 0 {
			name := filepath.Base(syscall.UTF16ToString(buf[:*size]))
			r = strings.EqualFold(name, exe) || strings.EqualFold(name, "Dolphin.exe")
		}
		syscall.CloseHandle(h)
	}
	cache[pid] = r
	return r
}
