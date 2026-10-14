//go:build windows

package main

import (
	"fmt"
	"os/exec"
	"sync"
	"syscall"
	"unsafe"
)

// Windows API helpers shared by the docking and volume code.

var (
	modUser32 = syscall.NewLazyDLL("user32.dll")
	modOle32  = syscall.NewLazyDLL("ole32.dll")
	modKernel = syscall.NewLazyDLL("kernel32.dll")

	pEnumWindows              = modUser32.NewProc("EnumWindows")
	pGetWindowThreadProcessId = modUser32.NewProc("GetWindowThreadProcessId")
	pIsWindowVisible          = modUser32.NewProc("IsWindowVisible")
	pIsIconic                 = modUser32.NewProc("IsIconic")
	pGetWindowTextW           = modUser32.NewProc("GetWindowTextW")
	pGetClassNameW            = modUser32.NewProc("GetClassNameW")
	pGetClientRect            = modUser32.NewProc("GetClientRect")
	pSetDpiCtx                = modUser32.NewProc("SetProcessDpiAwarenessContext")
	pSetDPIAware              = modUser32.NewProc("SetProcessDPIAware")

	pCoInitializeEx   = modOle32.NewProc("CoInitializeEx")
	pCoUninitialize   = modOle32.NewProc("CoUninitialize")
	pCoCreateInstance = modOle32.NewProc("CoCreateInstance")
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

// lowPriority runs a command below normal priority, so saving a recording
// doesn't slow down the next game.
func lowPriority(cmd *exec.Cmd) *exec.Cmd {
	const belowNormal = 0x00004000 // BELOW_NORMAL_PRIORITY_CLASS
	if cmd.SysProcAttr == nil {
		cmd.SysProcAttr = &syscall.SysProcAttr{}
	}
	cmd.SysProcAttr.CreationFlags |= belowNormal
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

// ---------------- windows ----------------

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

// topWindows lists a process's visible top-level windows.
func topWindows(pid int) []uintptr {
	enumMu.Lock()
	enumPID, enumFound = uint32(pid), nil
	pEnumWindows.Call(enumCB, 0)
	found := enumFound
	enumMu.Unlock()
	return found
}
