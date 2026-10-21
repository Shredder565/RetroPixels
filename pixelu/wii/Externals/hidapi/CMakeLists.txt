# HIDAPI (BSD-3-Clause and others): real Wii Remotes and the Steam Deck controls,
# through its Windows backend, or on Linux its hidraw backend (which finds
# devices through libudev, part of every systemd or eudev system).
project(hidapi)
set(src "${THIRD}/hidapi/hidapi-src")

if(WIN32)
  add_library(hidapi STATIC "${src}/hidapi/hidapi.h" "${src}/windows/hid.c")
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(UDEV REQUIRED IMPORTED_TARGET libudev)
  add_library(hidapi STATIC "${src}/hidapi/hidapi.h" "${src}/linux/hid.c")
  target_link_libraries(hidapi PRIVATE PkgConfig::UDEV)
endif()
target_include_directories(hidapi PUBLIC "${src}/hidapi")
add_library(Hidapi::Hidapi ALIAS hidapi)
pixelu_third_party_target(hidapi)
