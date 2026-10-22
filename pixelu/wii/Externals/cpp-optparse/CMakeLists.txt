# cpp-optparse (MIT): command-line parsing for PixelU-Wii.exe and dolphin-tool.
set(src "${THIRD}/cpp-optparse/cpp-optparse")

add_library(cpp-optparse STATIC "${src}/OptionParser.cpp" "${src}/OptionParser.h")
target_include_directories(cpp-optparse PUBLIC "${src}")
target_compile_features(cpp-optparse PUBLIC cxx_std_11)
if(NOT MSVC)
  # It throws on bad options; MSVC builds have exceptions on (/EHsc) already.
  target_compile_options(cpp-optparse PRIVATE -fexceptions)
endif()
pixelu_third_party_target(cpp-optparse)
