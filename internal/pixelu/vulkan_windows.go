//go:build windows

package pixelu

import (
	"errors"
	"sync"
	"syscall"
	"unsafe"
)

var (
	vulkanOnce sync.Once
	vulkanErr  error
)

// CheckVulkan says whether Vulkan works on this PC: its loader
// (vulkan-1.dll, installed with the graphics driver) is there and finds at
// least one graphics card that can use it. PixelU draws Wii U games with
// Vulkan; without it they can play with OpenGL instead. Checked once.
func CheckVulkan() error {
	vulkanOnce.Do(func() { vulkanErr = checkVulkan() })
	return vulkanErr
}

func checkVulkan() (err error) {
	defer func() {
		if r := recover(); r != nil { // a broken driver can crash in here
			err = errors.New("the graphics driver's Vulkan part crashed")
		}
	}()
	dll, e := syscall.LoadDLL("vulkan-1.dll")
	if e != nil {
		return errors.New("Vulkan isn't installed (it comes with the graphics driver)")
	}
	create, e1 := dll.FindProc("vkCreateInstance")
	enum, e2 := dll.FindProc("vkEnumeratePhysicalDevices")
	destroy, e3 := dll.FindProc("vkDestroyInstance")
	if e1 != nil || e2 != nil || e3 != nil {
		return errors.New("the Vulkan on this PC is incomplete")
	}
	// VkApplicationInfo asking for Vulkan 1.1, and VkInstanceCreateInfo.
	type appInfo struct {
		sType         uint32
		pNext         uintptr
		appName       uintptr
		appVersion    uint32
		engineName    uintptr
		engineVersion uint32
		apiVersion    uint32
	}
	type createInfo struct {
		sType                   uint32
		pNext                   uintptr
		flags                   uint32
		pApplicationInfo        *appInfo
		enabledLayerCount       uint32
		ppEnabledLayerNames     uintptr
		enabledExtensionCount   uint32
		ppEnabledExtensionNames uintptr
	}
	ai := &appInfo{sType: 0 /* VK_STRUCTURE_TYPE_APPLICATION_INFO */, apiVersion: 1<<22 | 1<<12}
	ci := &createInfo{sType: 1 /* VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO */, pApplicationInfo: ai}
	var inst uintptr
	if r, _, _ := create.Call(uintptr(unsafe.Pointer(ci)), 0, uintptr(unsafe.Pointer(&inst))); int32(r) != 0 || inst == 0 {
		return errors.New("the graphics driver doesn't support Vulkan 1.1")
	}
	defer destroy.Call(inst, 0)
	var n uint32
	if r, _, _ := enum.Call(inst, uintptr(unsafe.Pointer(&n)), 0); int32(r) != 0 || n == 0 {
		return errors.New("no graphics card on this PC can use Vulkan")
	}
	return nil
}
