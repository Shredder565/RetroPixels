# SDL 3 (Zlib licence): game controller input. SDL's own CMake project as a static
# library with only the subsystems input needs (no audio, video, GPU, render,
# camera, dialog or tray), no tests and no install rules.
set(SDL_SHARED OFF)
set(SDL_STATIC ON)
set(SDL_TEST_LIBRARY OFF)
set(SDL_TESTS OFF)
set(SDL_DISABLE_INSTALL ON)
set(SDL_DISABLE_INSTALL_DOCS ON)
set(SDL_INSTALL_TESTS OFF)
set(SDL_UNIX_CONSOLE_BUILD ON)
foreach(subsystem AUDIO VIDEO GPU RENDER CAMERA DIALOG TRAY)
  set(SDL_${subsystem} OFF)
endforeach()

# SDL's HIDAPI may use libusb: point it at the static libusb built here.
set(LibUSB_FOUND TRUE)
set(SDL_HIDAPI_LIBUSB_SHARED OFF)

add_subdirectory("${THIRD}/SDL/SDL" "${CMAKE_CURRENT_BINARY_DIR}/SDL")

foreach(target SDL3 SDL3-static)
  if(TARGET ${target})
    pixelu_third_party_target(${target})
  endif()
endforeach()
