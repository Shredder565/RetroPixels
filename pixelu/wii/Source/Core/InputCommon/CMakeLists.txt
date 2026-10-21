# InputCommon: controller configuration and the input backends (DirectInput,
# XInput, Windows.Gaming.Input and keyboard and mouse on Windows; XInput2 keyboard and
# mouse on Linux; SDL, DSU, the GameCube adapter).
# Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

pixelu_library(inputcommon
  IN .
    DynamicInputTextureManager.cpp DynamicInputTextureManager.h GCAdapter.cpp GCAdapter.h
    ImageOperations.cpp ImageOperations.h InputConfig.cpp InputConfig.h InputProfile.cpp
    InputProfile.h
  IN ControlReference
    ControlReference.cpp ControlReference.h ExpressionParser.cpp ExpressionParser.h
    FunctionExpression.cpp FunctionExpression.h
  IN ControllerEmu
    ControllerEmu.cpp ControllerEmu.h StickGate.cpp StickGate.h
  IN ControllerEmu/Control
    Control.cpp Control.h Input.cpp Input.h Output.cpp Output.h
  IN ControllerEmu/ControlGroup
    AnalogStick.cpp AnalogStick.h Attachments.cpp Attachments.h Buttons.cpp Buttons.h
    ControlGroup.cpp ControlGroup.h Cursor.cpp Cursor.h Force.cpp Force.h IMUAccelerometer.cpp
    IMUAccelerometer.h IMUCursor.cpp IMUCursor.h IMUGyroscope.cpp IMUGyroscope.h
    IRPassthrough.cpp IRPassthrough.h MixedTriggers.cpp MixedTriggers.h
    ModifySettingsButton.cpp ModifySettingsButton.h Slider.cpp Slider.h Tilt.cpp Tilt.h
    Triggers.cpp Triggers.h
  IN ControllerEmu/Setting
    NumericSetting.cpp NumericSetting.h
  IN ControllerInterface
    ControllerInterface.cpp ControllerInterface.h CoreDevice.cpp CoreDevice.h InputBackend.cpp
    InputBackend.h MappingCommon.cpp MappingCommon.h
  IN ControllerInterface/DualShockUDPClient
    DualShockUDPClient.cpp DualShockUDPClient.h DualShockUDPProto.h
  IN ControllerInterface/Wiimote
    WiimoteController.cpp WiimoteController.h
  IN DynamicInputTextures
    DITConfiguration.cpp DITConfiguration.h DITData.h DITSpecification.cpp DITSpecification.h
)

target_link_libraries(inputcommon
  PUBLIC
    common
    LibUSB::LibUSB
  PRIVATE
    fmt::fmt
    sfml-network
    spng::spng
)

if(WIN32)
  pixelu_add_sources(inputcommon
    IN ControllerInterface/DInput
      DInput.cpp DInput.h DInput8.h DInputJoystick.cpp DInputJoystick.h DInputKeyboardMouse.cpp
      DInputKeyboardMouse.h NamedKeys.h XInputFilter.cpp XInputFilter.h
    IN ControllerInterface/ForceFeedback
      ForceFeedbackDevice.cpp ForceFeedbackDevice.h
    IN ControllerInterface/WGInput
      WGInput.cpp WGInput.h
    IN ControllerInterface/Win32
      Win32.cpp Win32.h
    IN ControllerInterface/XInput
      XInput.cpp XInput.h
  )
endif()

if(NOT WIN32)
  # Keyboard and mouse on X11 (and XWayland): the XInput2 backend. libX11 and libXi are
  # on every X11 desktop; building needs libx11-dev and libxi-dev.
  option(ENABLE_X11 "X11 support (keyboard and mouse input through XInput2)" ON)
  if(ENABLE_X11)
    find_package(X11)
    if(NOT X11_FOUND OR NOT X11_Xi_FOUND)
      message(FATAL_ERROR "X11 keyboard and mouse input needs Xlib and XInput2 (libx11-dev, "
        "libxi-dev; Fedora: libX11-devel, libXi-devel), or configure with -DENABLE_X11=OFF.")
    endif()
    pixelu_add_sources(inputcommon
      IN ControllerInterface/Xlib
        XInput2.cpp XInput2.h
    )
    target_link_libraries(inputcommon PRIVATE X11::X11 X11::Xi)
    # Public: the emulated controllers' default mappings use the X11 key and button names.
    target_compile_definitions(inputcommon PUBLIC HAVE_X11=1)
  endif()
endif()

if(ENABLE_SDL)
  pixelu_add_sources(inputcommon
    IN ControllerInterface/SDL
      SDL.cpp SDL.h SDLGamepad.cpp SDLGamepad.h
  )
  target_link_libraries(inputcommon PRIVATE SDL3::SDL3)
endif()

pixelu_use_pch(inputcommon)

if(TARGET Hidapi::Hidapi)
  # The Steam Deck's built-in controls.
  pixelu_add_sources(inputcommon
    IN ControllerInterface/SteamDeck
      SteamDeck.cpp SteamDeck.h
  )
  target_link_libraries(inputcommon PUBLIC Hidapi::Hidapi)
  target_compile_definitions(inputcommon PRIVATE HAVE_HIDAPI=1)
endif()
