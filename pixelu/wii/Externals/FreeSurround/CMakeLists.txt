# FreeSurround (GPL-2.0-or-later, with KISS FFT, BSD-3-Clause): stereo to 5.1
# upmixing for the audio backends. Kept in this folder, see README.txt.
set(src "${CMAKE_CURRENT_SOURCE_DIR}")

add_library(FreeSurround STATIC
  "${src}/source/ChannelMaps.cpp"
  "${src}/source/FreeSurroundDecoder.cpp"
  "${src}/source/KissFFT.cpp"
  "${src}/source/KissFFTR.cpp"
)
target_include_directories(FreeSurround PUBLIC "${src}/include")
if(NOT MSVC)
  set_target_properties(FreeSurround PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
endif()
pixelu_third_party_target(FreeSurround)
target_compile_options(FreeSurround PRIVATE -w)
