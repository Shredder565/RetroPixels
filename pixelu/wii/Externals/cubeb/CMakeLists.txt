# cubeb (ISC, Mozilla): audio output and microphone input. Built here from its
# sources with the speex resampler it bundles, and the backends of the system:
# WASAPI and WinMM on Windows; PulseAudio (which PipeWire also serves) and ALSA
# on Linux. (cubeb's own CMake project also builds tests, tools and Rust parts.)
cmake_minimum_required(VERSION 3.14)
project(cubeb VERSION 0.0.0 LANGUAGES C CXX)
set(src "${THIRD}/cubeb/cubeb")

set(BUILD_SHARED_LIBS OFF CACHE BOOL "Third-party libraries are linked statically")

add_library(cubeb
  "${src}/src/cubeb.c"
  "${src}/src/cubeb_log.cpp"
  "${src}/src/cubeb_mixer.cpp"
  "${src}/src/cubeb_resampler.cpp"
  "${src}/src/cubeb_strings.c"
  "${src}/src/cubeb_utils.cpp"
)
if(WIN32)
  target_sources(cubeb PRIVATE
    "${src}/src/cubeb_wasapi.cpp"
    "${src}/src/cubeb_winmm.c"
  )
endif()
add_library(cubeb::cubeb ALIAS cubeb)
target_include_directories(cubeb PUBLIC "$<BUILD_INTERFACE:${src}/include>")
if(WIN32)
  target_compile_definitions(cubeb PRIVATE USE_WASAPI USE_WINMM)
endif()
pixelu_third_party_target(cubeb)

# cubeb_export.h, which cubeb.h includes.
include(GenerateExportHeader)
generate_export_header(cubeb EXPORT_FILE_NAME "${CMAKE_BINARY_DIR}/exports/cubeb_export.h")
target_include_directories(cubeb PUBLIC "$<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/exports>")

# The speex resampler cubeb ships, compiled into cubeb.
add_library(speex OBJECT "${src}/subprojects/speex/resample.c")
set_target_properties(speex PROPERTIES POSITION_INDEPENDENT_CODE TRUE)
target_include_directories(speex INTERFACE "${src}/subprojects")
target_compile_definitions(speex PUBLIC OUTSIDE_SPEEX FLOATING_POINT EXPORT= RANDOM_PREFIX=speex)
pixelu_third_party_target(speex)
target_link_libraries(cubeb PRIVATE $<BUILD_INTERFACE:speex>)

find_package(Threads)
if(WIN32)
  target_link_libraries(cubeb PRIVATE Threads::Threads avrt ole32 ksuser winmm)
else()
  # Linux: the PulseAudio and ALSA backends. Only their headers are needed to
  # build (libpulse-dev, libasound2-dev); cubeb opens libpulse.so.0 and
  # libasound.so.2 with dlopen when a stream is made (its lazy loading), so the
  # program starts on systems without one of them and picks the other, or
  # PixelU-Wii's null sound output when neither is there.
  option(CUBEB_PULSE "cubeb's PulseAudio backend (also PipeWire's PulseAudio server)" ON)
  option(CUBEB_ALSA "cubeb's ALSA backend" ON)
  include(CheckIncludeFile)
  set(backends "")
  if(CUBEB_PULSE)
    check_include_file(pulse/pulseaudio.h have_pulse_h)
    if(NOT have_pulse_h)
      message(FATAL_ERROR "cubeb: pulse/pulseaudio.h not found. Install libpulse-dev "
        "(Fedora: pulseaudio-libs-devel) or configure with -DCUBEB_PULSE=OFF.")
    endif()
    target_sources(cubeb PRIVATE "${src}/src/cubeb_pulse.c")
    target_compile_definitions(cubeb PRIVATE USE_PULSE)
    list(APPEND backends PulseAudio)
  endif()
  if(CUBEB_ALSA)
    check_include_file(alsa/asoundlib.h have_alsa_h)
    if(NOT have_alsa_h)
      message(FATAL_ERROR "cubeb: alsa/asoundlib.h not found. Install libasound2-dev "
        "(Fedora: alsa-lib-devel) or configure with -DCUBEB_ALSA=OFF.")
    endif()
    target_sources(cubeb PRIVATE "${src}/src/cubeb_alsa.c")
    target_compile_definitions(cubeb PRIVATE USE_ALSA)
    list(APPEND backends ALSA)
  endif()
  if(NOT backends)
    message(FATAL_ERROR "cubeb: no backend on Linux; keep CUBEB_PULSE or CUBEB_ALSA on, "
      "or configure PixelU-Wii with -DENABLE_CUBEB=OFF.")
  endif()
  string(REPLACE ";" ", " backends "${backends}")
  message(STATUS "cubeb backends: ${backends} (loaded at run time)")
  target_link_libraries(cubeb PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
endif()
