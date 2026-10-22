# Original code for RetroPixels (PixelU). Not derived from Cemu.
# CemuInput: emulated controllers and the host input APIs behind them

add_library(CemuInput)
pixelu_add_folder(CemuInput .
	ControllerFactory.cpp
	ControllerFactory.h
	DefaultControllerProfile.cpp
	DefaultControllerProfile.h
	InputManager.cpp
	InputManager.h
)
pixelu_add_folder(CemuInput api
	Controller.cpp
	Controller.h
	ControllerProvider.h
	ControllerState.cpp
	ControllerState.h
	InputAPI.h
)
pixelu_add_folder(CemuInput api/DSU
	DSUController.cpp
	DSUController.h
	DSUControllerProvider.cpp
	DSUControllerProvider.h
	DSUMessages.cpp
	DSUMessages.h
)
pixelu_add_folder(CemuInput api/Keyboard
	KeyboardController.cpp
	KeyboardController.h
	KeyboardControllerProvider.cpp
	KeyboardControllerProvider.h
)
pixelu_add_folder(CemuInput emulated
	ClassicController.cpp
	ClassicController.h
	EmulatedController.cpp
	EmulatedController.h
	ProController.cpp
	ProController.h
	VPADController.cpp
	VPADController.h
	WiimoteController.cpp
	WiimoteController.h
	WPADController.cpp
	WPADController.h
)
pixelu_add_folder(CemuInput motion Mahony.h MotionHandler.h MotionSample.h)

pixelu_library(CemuInput)

# GameCube controller adapter (libusb)
if(ENABLE_LIBUSB)
	pixelu_add_folder(CemuInput api/GameCube
		GameCubeController.cpp
		GameCubeController.h
		GameCubeControllerProvider.cpp
		GameCubeControllerProvider.h
	)
endif()

if(WIN32)
	pixelu_add_folder(CemuInput api/XInput
		XInputController.cpp
		XInputController.h
		XInputControllerProvider.cpp
		XInputControllerProvider.h
	)
	pixelu_add_folder(CemuInput api/DirectInput
		DirectInputController.cpp
		DirectInputController.h
		DirectInputControllerProvider.cpp
		DirectInputControllerProvider.h
	)
endif()

# real Wii remotes, through HIDAPI and/or BlueZ
if(SUPPORTS_WIIMOTE)
	target_compile_definitions(CemuInput PUBLIC SUPPORTS_WIIMOTE)
	pixelu_add_folder(CemuInput api/Wiimote
		NativeWiimoteController.cpp
		NativeWiimoteController.h
		WiimoteControllerProvider.cpp
		WiimoteControllerProvider.h
		WiimoteDevice.h
		WiimoteMessages.h
	)
	if(ENABLE_HIDAPI)
		pixelu_add_folder(CemuInput api/Wiimote/hidapi HidapiWiimote.cpp HidapiWiimote.h)
	endif()
	if(ENABLE_BLUEZ)
		pixelu_add_folder(CemuInput api/Wiimote/l2cap L2CapWiimote.cpp L2CapWiimote.h)
	endif()
endif()

if(ENABLE_SDL)
	pixelu_add_folder(CemuInput api/SDL
		SDLController.cpp
		SDLController.h
		SDLControllerProvider.cpp
		SDLControllerProvider.h
	)
	target_link_libraries(CemuInput PRIVATE SDL3::SDL3)
endif()

target_include_directories(CemuInput PUBLIC "../")
target_link_libraries(CemuInput PRIVATE CemuCommon CemuGui)
if(ENABLE_BLUEZ)
	target_link_libraries(CemuInput PRIVATE bluez::bluez)
endif()
