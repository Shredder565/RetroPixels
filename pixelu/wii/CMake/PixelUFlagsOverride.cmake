# Original code for RetroPixels (PixelU-Wii).
#
# Loaded through CMAKE_USER_MAKE_RULES_OVERRIDE, before the default flags are cached.
# With MSVC, optimised builds use full optimisation (/O2, which CMake's own
# RelWithDebInfo default leaves without inlining) and always carry debug info
# inside the objects (/Z7), so a Release build can still be debugged.

if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
  foreach(lang C CXX)
    foreach(config RELEASE RELWITHDEBINFO)
      set(CMAKE_${lang}_FLAGS_${config} "/O2 /DNDEBUG /Z7")
    endforeach()
  endforeach()
endif()
