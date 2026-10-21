# miniLZO (GPL-2.0-or-later, vendored in Dolphin's Externals): save states.
set(src "${THIRD}/LZO")

add_library(lzo2 STATIC "${src}/minilzo.c")
target_include_directories(lzo2 PUBLIC "${src}")
add_library(LZO::LZO ALIAS lzo2)
pixelu_third_party_target(lzo2)
