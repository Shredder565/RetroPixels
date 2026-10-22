# Bochs' x86 disassembler (LGPL-2.1-or-later), bochs/disasm from the Bochs
# repository at svn r12420 (July 2014), with PixelU-Wii's patch
# Externals/patches/bochs-disasm-adc-imm32.patch. Used by the debugger to show
# the JIT's host code. include/config.h here stands in for the config.h Bochs'
# configure script writes.
set(src "${THIRD}/Bochs_disasm/bochs/bochs/disasm")

add_library(bdisasm STATIC
  "${src}/dis_decode.cc"
  "${src}/dis_groups.cc"
  "${src}/resolve.cc"
  "${src}/syntax.cc"
  "${src}/disasm.h"
  include/config.h
)
target_include_directories(bdisasm PUBLIC "${src}" "${CMAKE_CURRENT_SOURCE_DIR}/include")
if(MSVC)
  target_compile_definitions(bdisasm PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
pixelu_third_party_target(bdisasm)
