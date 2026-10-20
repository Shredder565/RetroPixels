add_library(audiocommon
  AudioCommon.cpp
  AudioCommon.h
  CubebStream.h
  Enums.h
  Mixer.cpp
  Mixer.h
  SurroundDecoder.cpp
  SurroundDecoder.h
  NullSoundStream.cpp
  NullSoundStream.h
  WaveFile.cpp
  WaveFile.h
)

if(ENABLE_CUBEB)
  message(STATUS "Cubeb found, enabling Cubeb sound backend")
  target_sources(audiocommon PRIVATE
    CubebStream.cpp
    CubebUtils.cpp
    CubebUtils.h
  )
  target_link_libraries(audiocommon PRIVATE cubeb)
endif()

if(WIN32)
  target_sources(audiocommon PRIVATE
    # Dolphin loads openal32.dll at runtime
    OpenALStream.cpp
    OpenALStream.h

    WASAPIStream.cpp
    WASAPIStream.h
  )
endif()

target_link_libraries(audiocommon
PUBLIC
  common

PRIVATE
  FreeSurround)

if(ENABLE_CUBEB)
  target_link_libraries(audiocommon PRIVATE cubeb::cubeb)
endif()

if(MSVC)
  # Add precompiled header
  target_link_libraries(audiocommon PRIVATE use_pch)
endif()
