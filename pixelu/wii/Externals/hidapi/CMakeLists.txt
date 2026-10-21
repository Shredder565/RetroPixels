# HIDAPI (BSD-3-Clause and others): real Wii Remotes and the Steam Deck controls,
# through its Windows backend.
project(hidapi)
set(src "${THIRD}/hidapi/hidapi-src")

add_library(hidapi STATIC "${src}/hidapi/hidapi.h" "${src}/windows/hid.c")
target_include_directories(hidapi PUBLIC "${src}/hidapi")
add_library(Hidapi::Hidapi ALIAS hidapi)
pixelu_third_party_target(hidapi)
