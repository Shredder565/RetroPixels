# miniLZO 2.10 (GPL-2.0-or-later) from the LZO 2.10 release archive: save
# states and netplay. minilzo/minilzo.c is the whole library; the lzoconf.h and
# lzodefs.h it includes are in the archive's include/lzo. include/lzo/lzo1x.h
# here maps the full library's header, which PixelU-Wii includes, onto minilzo.h.
set(src "${THIRD}/LZO/lzo")

add_library(lzo2 STATIC "${src}/minilzo/minilzo.c" "${src}/minilzo/minilzo.h")
target_include_directories(lzo2 PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${src}/minilzo"
  "${src}/include/lzo"
)
add_library(LZO::LZO ALIAS lzo2)
pixelu_third_party_target(lzo2)
