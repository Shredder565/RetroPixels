//go:build windows

package main

// Sets the volume of Dolphin's sound the same way the Windows volume mixer
// does: through Dolphin's audio session (Core Audio, audiopolicy.h). Only
// Dolphin's sound changes, not the rest of the PC, and Dolphin doesn't need
// to know about it.

import (
	"math"
	"path/filepath"
	"runtime"
	"sync"
	"syscall"
	"time"
	"unsafe"

	"retropixels/internal/dolphin"
)

var (
	pQueryFullProcessImageNameW = modKernel.NewProc("QueryFullProcessImageNameW")

	iidIAudioSessionManager2 = mkGUID(0x77AA99A0, 0x1BD6, 0x484F, 0x8B, 0xC7, 0x2C, 0x65, 0x4C, 0x9A, 0x9B, 0x6F) // IAudioSessionManager2
	iidIAudioSessionControl2 = mkGUID(0xBFB7FF88, 0x7239, 0x4FC9, 0x8F, 0xA2, 0x07, 0xC9, 0x50, 0xBE, 0x9C, 0x6D) // IAudioSessionControl2
	iidISimpleAudioVolume    = mkGUID(0x87CE5498, 0x68D6, 0x44E5, 0x92, 0x15, 0x6D, 0xA4, 0x7E, 0xF8, 0x83, 0xD8) // ISimpleAudioVolume
)

// Vtable slots. Every interface starts with IUnknown: 0 QueryInterface, 1 AddRef, 2 Release.
const (
	vtQueryInterface = 0

	// IMMDeviceEnumerator: 3 EnumAudioEndpoints, 4 GetDefaultAudioEndpoint, 5 GetDevice, ...
	vtEnumAudioEndpoints = 3
	// IMMDeviceCollection: 3 GetCount, 4 Item
	vtDevicesGetCount = 3
	vtDevicesItem     = 4
	// IMMDevice: 3 Activate, 4 OpenPropertyStore, 5 GetId, 6 GetState
	vtDeviceActivate = 3
	// IAudioSessionManager: 3 GetAudioSessionControl, 4 GetSimpleAudioVolume;
	// IAudioSessionManager2 adds 5 GetSessionEnumerator, 6–9 notifications.
	vtGetSessionEnumerator = 5
	// IAudioSessionEnumerator: 3 GetCount, 4 GetSession
	vtSessionsGetCount = 3
	vtSessionsGet      = 4
	// IAudioSessionControl: 3 GetState ... 11 UnregisterAudioSessionNotification;
	// IAudioSessionControl2 adds 12 GetSessionIdentifier, 13 GetSessionInstanceIdentifier,
	// 14 GetProcessId, 15 IsSystemSoundsSession, 16 SetDuckingPreference.
	vtGetProcessId = 14
	// ISimpleAudioVolume: 3 SetMasterVolume, 4 GetMasterVolume, 5 SetMute, 6 GetMute
	vtSetMasterVolume = 3
	vtSetMute         = 5

	clsctxAll         = 0x17 // CLSCTX_ALL
	eRender           = 0
	deviceStateActive = 1          // DEVICE_STATE_ACTIVE
	processQueryLimit = 0x1000     // PROCESS_QUERY_LIMITED_INFORMATION
	coinitMulti       = 0x0        // COINIT_MULTITHREADED
	audclntNoProcess  = 0x0889000D // AUDCLNT_S_NO_SINGLE_PROCESS: a session shared by several programs
)

// floatArgsOK: SetMasterVolume takes a float, and syscall.SyscallN only takes
// integers. On windows/amd64 the Go runtime's asmstdcall copies each of the
// first four arguments into both its integer register and the matching XMM
// register (runtime/sys_windows_amd64.s), so the float's bits passed as the
// 2nd argument land in XMM1, where SetMasterVolume reads its level. On
// windows/386 arguments go on the stack, where a float32 is just its 4 bytes.
// windows/arm64 doesn't fill float registers, so there only mute is used.
const floatArgsOK = runtime.GOARCH == "amd64" || runtime.GOARCH == "386"

var (
	volMu   sync.Mutex
	volPID  int
	volKick = make(chan struct{}, 1)
)

// followDolphinVolume applies the volume setting to one Dolphin run. Dolphin
// only opens its sound a moment after it starts, so it keeps trying for a
// while, and again whenever the setting changes.
func followDolphinVolume(pid int, exited <-chan struct{}) {
	volMu.Lock()
	volPID = pid
	volMu.Unlock()
	defer func() {
		volMu.Lock()
		if volPID == pid {
			volPID = 0
		}
		volMu.Unlock()
	}()
	select {
	case <-volKick: // left over from before this game
	default:
	}
	exe := filepath.Base(dolphinExe())
	tick := time.NewTicker(500 * time.Millisecond)
	defer tick.Stop()
	until := time.Now().Add(20 * time.Second)
	for {
		if time.Now().Before(until) {
			v, m := dolphinVolume()
			setProgramVolume(pid, exe, float32(v)/100, m)
		}
		select {
		case <-exited:
			return
		case <-volKick:
			if t := time.Now().Add(3 * time.Second); t.After(until) {
				until = t
			}
		case <-tick.C:
		}
	}
}

// applyDolphinVolumeNow re-applies the setting to the game that's playing.
func applyDolphinVolumeNow() {
	volMu.Lock()
	on := volPID != 0
	volMu.Unlock()
	if !on {
		return
	}
	select {
	case volKick <- struct{}{}:
	default:
	}
}

func ptr[T any](p *T) uintptr { return uintptr(unsafe.Pointer(p)) }

// setProgramVolume sets the volume of every audio session (on every speaker
// or headphone output) that belongs to process pid, or to a program called
// exe as a fallback. It returns how many sessions it changed.
func setProgramVolume(pid int, exe string, level float32, mute bool) int {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	if hr, _, _ := pCoInitializeEx.Call(0, coinitMulti); !failed(hr) { // S_OK or S_FALSE both need CoUninitialize
		defer pCoUninitialize.Call()
	}
	// Out-parameters live on the heap so they can't move while Windows writes to them.
	en := new(uintptr)
	if hr, _, _ := pCoCreateInstance.Call(ptr(&clsidMMDeviceEnumerator), 0, clsctxAll,
		ptr(&iidIMMDeviceEnumerator), ptr(en)); failed(hr) || *en == 0 {
		return 0
	}
	defer comRelease(*en)
	devs := new(uintptr)
	if hr := comCall(*en, vtEnumAudioEndpoints, eRender, deviceStateActive, ptr(devs)); failed(hr) || *devs == 0 {
		return 0
	}
	defer comRelease(*devs)
	n := new(uint32)
	if hr := comCall(*devs, vtDevicesGetCount, ptr(n)); failed(hr) {
		return 0
	}
	names := map[uint32]bool{} // process ID -> is it Dolphin, looked up once per call
	done := 0
	for i := uint32(0); i < *n; i++ {
		dev := new(uintptr)
		if hr := comCall(*devs, vtDevicesItem, uintptr(i), ptr(dev)); failed(hr) || *dev == 0 {
			continue
		}
		done += setDeviceSessions(*dev, pid, exe, names, level, mute)
		comRelease(*dev)
	}
	return done
}

func setDeviceSessions(dev uintptr, pid int, exe string, names map[uint32]bool, level float32, mute bool) int {
	mgr := new(uintptr)
	if hr := comCall(dev, vtDeviceActivate, ptr(&iidIAudioSessionManager2), clsctxAll, 0, ptr(mgr)); failed(hr) || *mgr == 0 {
		return 0
	}
	defer comRelease(*mgr)
	sessions := new(uintptr)
	if hr := comCall(*mgr, vtGetSessionEnumerator, ptr(sessions)); failed(hr) || *sessions == 0 {
		return 0
	}
	defer comRelease(*sessions)
	count := new(int32)
	if hr := comCall(*sessions, vtSessionsGetCount, ptr(count)); failed(hr) {
		return 0
	}
	done := 0
	for i := int32(0); i < *count; i++ {
		ctl := new(uintptr)
		if hr := comCall(*sessions, vtSessionsGet, uintptr(i), ptr(ctl)); failed(hr) || *ctl == 0 {
			continue
		}
		ctl2 := new(uintptr)
		hr := comCall(*ctl, vtQueryInterface, ptr(&iidIAudioSessionControl2), ptr(ctl2))
		comRelease(*ctl)
		if failed(hr) || *ctl2 == 0 {
			continue
		}
		spid := new(uint32)
		hr = comCall(*ctl2, vtGetProcessId, ptr(spid))
		if !failed(hr) && hr != audclntNoProcess && *spid != 0 && (int(*spid) == pid || isProgram(*spid, exe, names)) {
			vol := new(uintptr)
			if hr := comCall(*ctl2, vtQueryInterface, ptr(&iidISimpleAudioVolume), ptr(vol)); !failed(hr) && *vol != 0 {
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

func setSessionVolume(vol uintptr, level float32, mute bool) bool {
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

// isProgram tells whether process pid runs a program called exe (e.g.
// PixelU-Wii.exe): the engine's own file name, whatever it is.
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
			r = dolphin.IsProgram(syscall.UTF16ToString(buf[:*size]), exe)
		}
		syscall.CloseHandle(h)
	}
	cache[pid] = r
	return r
}
