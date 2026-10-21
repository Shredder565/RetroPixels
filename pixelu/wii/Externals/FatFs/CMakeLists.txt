# FatFs by ChaN (BSD-1-Clause style, vendored in Dolphin's Externals with its
# ffconf.h): builds and reads the FAT images of the emulated SD card.
set(src "${THIRD}/FatFs")

add_library(FatFs STATIC
  "${src}/ff.c"
  "${src}/ffunicode.c"
  "${src}/diskio.h"
  "${src}/ff.h"
  "${src}/ffconf.h"
)
target_include_directories(FatFs PUBLIC "${src}")
