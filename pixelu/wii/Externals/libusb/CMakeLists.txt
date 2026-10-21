# libusb (LGPL-2.1-or-later): USB passthrough, the GameCube adapter and real
# Bluetooth adapters. Built from its Windows sources (WinUSB and UsbDK backends)
# with the config.h libusb ships for MSVC in libusb/msvc.
set(src "${THIRD}/libusb/libusb")

add_library(usb STATIC EXCLUDE_FROM_ALL)
pixelu_add_sources(usb
  IN "${src}/libusb" core.c descriptor.c hotplug.c io.c strerror.c sync.c
  IN "${src}/libusb/os" events_windows.c threads_windows.c windows_common.c windows_usbdk.c windows_winusb.c
)
set_target_properties(usb PROPERTIES VERSION 1.0.26)
# BEFORE: other libraries have a config.h too; libusb's must win.
target_include_directories(usb BEFORE
  PUBLIC "${src}/libusb"
  PRIVATE "${src}/msvc"
)
pixelu_third_party_target(usb)

# An imported interface target rather than an alias, so that SDL's try_compile
# checks can use it too.
add_library(LibUSB::LibUSB INTERFACE IMPORTED GLOBAL)
target_link_libraries(LibUSB::LibUSB INTERFACE usb)
target_include_directories(LibUSB::LibUSB INTERFACE "${src}/libusb")
