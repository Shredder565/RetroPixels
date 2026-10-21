# Bochs' x86 disassembler (LGPL-2.1, vendored in Dolphin's Externals), used by the
# debugger to show the JIT's host code.
set(src "${THIRD}/Bochs_disasm")

add_library(bdisasm STATIC
  "${src}/dis_decode.cc"
  "${src}/dis_groups.cc"
  "${src}/resolve.cc"
  "${src}/syntax.cc"
  "${src}/stdafx.cc"
)
target_include_directories(bdisasm PUBLIC "${src}")
pixelu_third_party_target(bdisasm)
