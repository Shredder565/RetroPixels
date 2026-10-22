# FatFs R0.14b by ChaN (BSD-1-Clause style), from the release archive, with
# PixelU-Wii's two patches (Externals/patches/fatfs-*.patch: ChaN's UTF-8 fix
# and the ffconf.h options): builds and reads the FAT images of the emulated
# SD card and the WiiConnect24 VFF files.
set(src "${THIRD}/FatFs/ff14b/source")

add_library(FatFs STATIC
  "${src}/ff.c"
  "${src}/ffunicode.c"
  "${src}/diskio.h"
  "${src}/ff.h"
  "${src}/ffconf.h"
  include/ff.h
)
# include/ff.h comes first: PixelU-Wii's code sees FatFs' TCHAR as FATFS_TCHAR,
# so it cannot clash with Windows' TCHAR. diskio.h is FatFs' own.
target_include_directories(FatFs PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include" "${src}")
target_compile_definitions(FatFs INTERFACE "PIXELU_FATFS_FF_H=\"${src}/ff.h\"")
pixelu_third_party_target(FatFs)
