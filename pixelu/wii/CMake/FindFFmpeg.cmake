# Original code for RetroPixels (PixelU-Wii).
#
# find_package(FFmpeg COMPONENTS <lib>...) for the prebuilt FFmpeg the Windows
# build uses: FFMPEG_DIR (or the FFMPEG_DIR environment variable) is a folder with
# include/lib<name>/<name>.h and lib/<name>.lib for each library. Without
# FFMPEG_DIR the usual system locations and pkg-config are searched.
#
# For each component found:  FFmpeg::<name> (imported target), FFmpeg_<name>_FOUND,
# FFmpeg_INCLUDE_<name>, FFmpeg_LIBRARY_<name>.  Overall: FFmpeg_FOUND,
# FFmpeg_INCLUDES, FFmpeg_LIBRARIES.

# What each library needs from the others.
set(_pixelu_ffmpeg_needs_avcodec avutil)
set(_pixelu_ffmpeg_needs_avdevice avcodec avformat avutil)
set(_pixelu_ffmpeg_needs_avfilter avutil)
set(_pixelu_ffmpeg_needs_avformat avcodec avutil)
set(_pixelu_ffmpeg_needs_postproc avutil)
set(_pixelu_ffmpeg_needs_swresample avutil)
set(_pixelu_ffmpeg_needs_swscale avutil)

if(NOT FFMPEG_DIR AND DEFINED ENV{FFMPEG_DIR})
  set(FFMPEG_DIR "$ENV{FFMPEG_DIR}")
endif()

# Components asked for, plus what they depend on.
set(_pixelu_ffmpeg_libs ${FFmpeg_FIND_COMPONENTS})
foreach(_lib IN LISTS FFmpeg_FIND_COMPONENTS)
  list(APPEND _pixelu_ffmpeg_libs ${_pixelu_ffmpeg_needs_${_lib}})
endforeach()
list(REMOVE_DUPLICATES _pixelu_ffmpeg_libs)

if(NOT FFMPEG_DIR)
  find_package(PkgConfig QUIET)
endif()

set(FFmpeg_INCLUDES)
set(FFmpeg_LIBRARIES)
foreach(_lib IN LISTS _pixelu_ffmpeg_libs)
  if(FFMPEG_DIR)
    # Only the given folder: never mix in another FFmpeg found elsewhere.
    find_path(FFmpeg_INCLUDE_${_lib} lib${_lib}/${_lib}.h
      PATHS "${FFMPEG_DIR}/include" "${FFMPEG_DIR}"
      NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
    find_library(FFmpeg_LIBRARY_${_lib} ${_lib}
      PATHS "${FFMPEG_DIR}/lib" "${FFMPEG_DIR}"
      NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
  else()
    if(PKG_CONFIG_FOUND)
      pkg_check_modules(_pixelu_pc_${_lib} QUIET lib${_lib})
    endif()
    find_path(FFmpeg_INCLUDE_${_lib} lib${_lib}/${_lib}.h
      HINTS ${_pixelu_pc_${_lib}_INCLUDE_DIRS} PATH_SUFFIXES ffmpeg)
    find_library(FFmpeg_LIBRARY_${_lib} ${_lib}
      HINTS ${_pixelu_pc_${_lib}_LIBRARY_DIRS})
  endif()

  if(FFmpeg_INCLUDE_${_lib} AND FFmpeg_LIBRARY_${_lib})
    set(FFmpeg_${_lib}_FOUND TRUE)
    list(APPEND FFmpeg_INCLUDES "${FFmpeg_INCLUDE_${_lib}}")
    list(APPEND FFmpeg_LIBRARIES "${FFmpeg_LIBRARY_${_lib}}")
  else()
    set(FFmpeg_${_lib}_FOUND FALSE)
  endif()
endforeach()
list(REMOVE_DUPLICATES FFmpeg_INCLUDES)

set(_pixelu_ffmpeg_required)
foreach(_lib IN LISTS FFmpeg_FIND_COMPONENTS)
  list(APPEND _pixelu_ffmpeg_required FFmpeg_INCLUDE_${_lib} FFmpeg_LIBRARY_${_lib})
endforeach()
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(FFmpeg REQUIRED_VARS ${_pixelu_ffmpeg_required} HANDLE_COMPONENTS)

# Targets, created after all libraries are known so the links between them resolve.
foreach(_lib IN LISTS _pixelu_ffmpeg_libs)
  if(FFmpeg_${_lib}_FOUND AND NOT TARGET FFmpeg::${_lib})
    add_library(FFmpeg::${_lib} UNKNOWN IMPORTED)
    set_target_properties(FFmpeg::${_lib} PROPERTIES
      IMPORTED_LOCATION "${FFmpeg_LIBRARY_${_lib}}"
      INTERFACE_INCLUDE_DIRECTORIES "${FFmpeg_INCLUDE_${_lib}}")
  endif()
endforeach()
foreach(_lib IN LISTS _pixelu_ffmpeg_libs)
  if(TARGET FFmpeg::${_lib})
    set(_needs)
    foreach(_dep IN LISTS _pixelu_ffmpeg_needs_${_lib})
      if(TARGET FFmpeg::${_dep})
        list(APPEND _needs FFmpeg::${_dep})
      endif()
    endforeach()
    if(_needs)
      set_property(TARGET FFmpeg::${_lib} PROPERTY INTERFACE_LINK_LIBRARIES ${_needs})
    endif()
  endif()
endforeach()

unset(_lib)
unset(_dep)
unset(_needs)
unset(_pixelu_ffmpeg_libs)
unset(_pixelu_ffmpeg_required)
