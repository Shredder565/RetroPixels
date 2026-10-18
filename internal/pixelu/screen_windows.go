//go:build windows

package pixelu

import (
	"errors"
	"syscall"
	"unsafe"
)

var (
	user32              = syscall.NewLazyDLL("user32.dll")
	pRegisterWindowMsg  = user32.NewProc("RegisterWindowMessageW")
	pSendMessageTimeout = user32.NewProc("SendMessageTimeoutW")
)

// SetScreen picks what PixelU's game window (hwnd) shows: the TV picture,
// the GamePad's screen, or the other one of the two. It returns whether the
// GamePad screen is shown now. With it shown, a click or a touch on the
// picture is a touch on the GamePad's touch screen. (PixelU answers the
// "PixelU.Screen" window message, see MainWindow::MSWWindowProc.)
func SetScreen(hwnd uintptr, mode int) (bool, error) {
	name, _ := syscall.UTF16PtrFromString("PixelU.Screen")
	msg, _, _ := pRegisterWindowMsg.Call(uintptr(unsafe.Pointer(name)))
	if msg == 0 || hwnd == 0 {
		return false, errors.New("couldn't reach PixelU")
	}
	var res uintptr
	const smtoAbortIfHung = 0x0002
	if ok, _, _ := pSendMessageTimeout.Call(hwnd, msg, uintptr(mode), 0, smtoAbortIfHung, 2000, uintptr(unsafe.Pointer(&res))); ok == 0 {
		return false, errors.New("PixelU didn't answer (an older PixelU build can't show the GamePad screen here)")
	}
	return res == 1, nil
}
