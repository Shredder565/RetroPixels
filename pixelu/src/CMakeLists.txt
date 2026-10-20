# Original code for RetroPixels (PixelU). Not derived from Cemu.
#
# The emulator is split into static libraries (one per folder) that the
# executable CemuBin links; the target names are kept for existing build
# scripts and IDE setups.

option(CEMU_CXX_FLAGS "Additional flags used for compiling Cemu source code")
separate_arguments(CEMU_CXX_FLAGS NATIVE_COMMAND ${CEMU_CXX_FLAGS})
if(CEMU_CXX_FLAGS)
	add_compile_options(${CEMU_CXX_FLAGS})
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
	message(FATAL_ERROR "Pointers are not 64bit")
endif()

# ---------------------------------------------------------------------------
# Settings for every PixelU library
# ---------------------------------------------------------------------------
if(MSVC)
	add_compile_definitions(WIN32_LEAN_AND_MEAN CURL_STATICLIB)
elseif(UNIX)
	if(APPLE)
		add_compile_definitions(_XOPEN_SOURCE)
		if(ENABLE_VULKAN)
			# MoltenVK surfaces
			add_compile_definitions(VK_USE_PLATFORM_MACOS_MVK VK_USE_PLATFORM_METAL_EXT)
		endif()
	elseif(ENABLE_VULKAN)
		# X11 (Xlib and XCB) surfaces, Wayland when enabled
		add_compile_definitions(VK_USE_PLATFORM_XLIB_KHR VK_USE_PLATFORM_XCB_KHR)
		if(ENABLE_WAYLAND)
			add_compile_definitions(VK_USE_PLATFORM_WAYLAND_KHR)
		endif()
	endif()

	if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
		add_compile_options(-Wno-ambiguous-reversed-operator)
	endif()
	add_compile_options(-Wno-multichar -Wno-invalid-offsetof -Wno-switch -Wno-ignored-attributes -Wno-deprecated-enum-enum-conversion)
endif()

set(CMAKE_INCLUDE_CURRENT_DIR ON)

# pixelu_add_folder(<target> <folder> <file>...)
# adds the files, given relative to <folder> ("." for this directory), to the target
function(pixelu_add_folder target folder)
	set(files ${ARGN})
	if(NOT folder STREQUAL ".")
		list(TRANSFORM files PREPEND "${folder}/")
	endif()
	target_sources(${target} PRIVATE ${files})
endfunction()

# pixelu_library(<target>): static runtime and Common/precompiled.h. CemuCommon
# builds the precompiled header; MSVC can share it, the other compilers build
# their own copy for each library.
function(pixelu_library target)
	set_property(TARGET ${target} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
	if(MSVC)
		target_precompile_headers(${target} REUSE_FROM CemuCommon)
	else()
		target_precompile_headers(${target} PRIVATE "${CMAKE_SOURCE_DIR}/src/Common/precompiled.h")
	endif()
endfunction()

add_subdirectory(Common)
add_subdirectory(gui)
add_subdirectory(Cafe)
add_subdirectory(Cemu)
add_subdirectory(config)
add_subdirectory(input)
add_subdirectory(audio)
add_subdirectory(util)
add_subdirectory(imgui)
add_subdirectory(resource)

# ---------------------------------------------------------------------------
# The executable: bin/PixelU_<config>
# ---------------------------------------------------------------------------
add_executable(CemuBin main.cpp mainLLE.cpp)
pixelu_library(CemuBin)

if(WIN32)
	# icons, version block and the font, plus the application manifest
	target_sources(CemuBin PRIVATE resource/cemu.rc ../dist/windows/Cemu.manifest)
endif()

if(MSVC AND MSVC_VERSION EQUAL 1940)
	# VS 17.10 writes oversized incremental link (.ilk) files
	set_target_properties(CemuBin PROPERTIES LINK_FLAGS "/INCREMENTAL:NO")
endif()

if(APPLE AND CMAKE_GENERATOR STREQUAL "Xcode")
	set_target_properties(CemuBin PROPERTIES XCODE_ATTRIBUTE_ENABLE_DEBUG_DYLIB[variant=Debug] "YES")
endif()

set_target_properties(CemuBin PROPERTIES
	# a console window only in debug builds
	WIN32_EXECUTABLE $<NOT:$<CONFIG:Debug>>
	OUTPUT_NAME "PixelU_$<LOWER_CASE:$<CONFIG>>"
	# straight into bin/ (the $<1:> keeps multi-config generators from adding a config folder)
	RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../bin/$<1:>"
)

# ---- macOS: optional .app bundle with MoltenVK (and libusb) inside, else plain rpaths ----
if(APPLE AND MACOS_BUNDLE)
	set(PIXELU_ICNS "${CMAKE_SOURCE_DIR}/src/resource/pixelu.icns")
	target_sources(CemuBin PRIVATE "${PIXELU_ICNS}")
	set(PIXELU_VERSION_STRING "${EMULATOR_VERSION_MAJOR}.${EMULATOR_VERSION_MINOR}.${EMULATOR_VERSION_PATCH}")
	# values for CMake's own Info.plist template
	set(MACOSX_BUNDLE_ICON_FILE "pixelu.icns")
	set(MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.shredder565.PixelU")
	set(MACOSX_BUNDLE_BUNDLE_NAME "PixelU")
	set(MACOSX_BUNDLE_SHORT_VERSION_STRING "${PIXELU_VERSION_STRING}")
	set(MACOSX_BUNDLE_BUNDLE_VERSION "${PIXELU_VERSION_STRING}")
	set(MACOSX_BUNDLE_COPYRIGHT "RetroPixels project")
	set_target_properties(CemuBin PROPERTIES
		MACOSX_BUNDLE true
		RESOURCE "${PIXELU_ICNS}"
		BUILD_WITH_INSTALL_RPATH TRUE
		INSTALL_RPATH "@executable_path/../Frameworks"
	)

	set(PIXELU_BUNDLE_FRAMEWORKS "$<TARGET_BUNDLE_DIR:CemuBin>/Contents/Frameworks")
	foreach(folder gameProfiles resources)
		add_custom_command(TARGET CemuBin POST_BUILD
			COMMAND ${CMAKE_COMMAND} ARGS -E copy_directory "${CMAKE_SOURCE_DIR}/bin/${folder}" "$<TARGET_BUNDLE_DIR:CemuBin>/Contents/SharedSupport/${folder}")
	endforeach()

	if(ENABLE_VULKAN)
		find_file(PIXELU_MOLTENVK libMoltenVK.dylib PATHS /usr/local/lib /opt/homebrew/lib NO_DEFAULT_PATH)
		if(NOT PIXELU_MOLTENVK)
			message(FATAL_ERROR "failed to find libMoltenVK.dylib")
		endif()
		add_custom_command(TARGET CemuBin POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy "${PIXELU_MOLTENVK}" "${PIXELU_BUNDLE_FRAMEWORKS}/libMoltenVK.dylib")
	endif()

	if(ENABLE_LIBUSB)
		if(CMAKE_BUILD_TYPE STREQUAL "Debug")
			set(PIXELU_LIBUSB "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/debug/lib/libusb-1.0.0.dylib")
		else()
			set(PIXELU_LIBUSB "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/lib/libusb-1.0.0.dylib")
		endif()
		add_custom_command(TARGET CemuBin POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy "${PIXELU_LIBUSB}" "${PIXELU_BUNDLE_FRAMEWORKS}/libusb-1.0.0.dylib")
		add_custom_command(TARGET CemuBin POST_BUILD
			COMMAND install_name_tool -change @rpath/libusb-1.0.0.dylib @executable_path/../Frameworks/libusb-1.0.0.dylib "$<TARGET_FILE:CemuBin>")
	endif()
elseif(APPLE AND ENABLE_VULKAN)
	find_library(MOLTENVK_LIBRARY NAMES MoltenVK moltenvk libMoltenVK.dylib PATHS /usr/local/lib /opt/homebrew/lib)
	if(MOLTENVK_LIBRARY)
		message(STATUS "Found MoltenVK: ${MOLTENVK_LIBRARY}")
		target_link_libraries(CemuBin PRIVATE ${MOLTENVK_LIBRARY})
	else()
		message(WARNING "libMoltenVK.dylib not found")
	endif()
	set_target_properties(CemuBin PROPERTIES
		BUILD_WITH_INSTALL_RPATH TRUE
		INSTALL_RPATH "/usr/local/lib;/opt/homebrew/lib"
	)
endif()

if(ENABLE_SDL)
	target_link_libraries(CemuBin PRIVATE SDL3::SDL3)
endif()
target_link_libraries(CemuBin PRIVATE
	CemuAudio
	CemuCafe
	CemuCommon
	CemuComponents
	CemuConfig
	CemuGui
	CemuInput
	CemuUtil
)

if(UNIX AND NOT APPLE)
	# the nasm objects of ih264d would otherwise make the stack executable
	target_link_options(CemuBin PRIVATE -z noexecstack)
	# drop leftover debug info of the static dependencies in release builds
	target_link_options(CemuBin PRIVATE "$<$<CONFIG:Release>:-Xlinker;--strip-debug>")
endif()

if(BSD)
	target_link_libraries(CemuBin PRIVATE execinfo SPIRV-Tools SPIRV-Tools-opt)
endif()
