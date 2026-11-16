# AudioCommon: audio output (cubeb, WASAPI, null), the mixer, Dolby Pro
# Logic II decoding (our own matrix decoder), and audio dumping.
# Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

pixelu_library(audiocommon
  IN .
    AudioCommon.cpp AudioCommon.h CubebStream.h Enums.h Mixer.cpp Mixer.h NullSoundStream.cpp
    NullSoundStream.h SurroundDecoder.cpp SurroundDecoder.h
    WASAPIStream.cpp WASAPIStream.h WaveFile.cpp WaveFile.h
)

if(ENABLE_CUBEB)
  message(STATUS "cubeb sound backend: on")
  pixelu_add_sources(audiocommon
    IN .
      CubebStream.cpp CubebUtils.cpp CubebUtils.h
  )
  target_link_libraries(audiocommon PRIVATE cubeb)
endif()

target_link_libraries(audiocommon PUBLIC common)

pixelu_use_pch(audiocommon)
