# Original code for RetroPixels (PixelU). Not derived from Cemu.
#
# Build of PixelU, RetroPixels' Wii U emulator. Windows builds use vcpkg
# (dependencies/vcpkg, static x64 triplet) and produce bin/PixelU_<config>.exe,
# see .github/workflows/pixelu.yml in the repository root.

cmake_minimum_required(VERSION 3.21.1)

# ---------------------------------------------------------------------------
# Options that have to be known before project() (toolchain selection)
# ---------------------------------------------------------------------------
option(ENABLE_VCPKG "Enable the vcpkg package manager" ON)
option(MACOS_BUNDLE "The executable when built on macOS will be created as an application bundle" OFF)
option(ALLOW_PORTABLE "Allow PixelU to be run in portable mode" ON)

# release scripts may pass a version, local builds show the commit hash instead
set(EMULATOR_VERSION_MAJOR "0" CACHE STRING "")
set(EMULATOR_VERSION_MINOR "0" CACHE STRING "")
set(EMULATOR_VERSION_PATCH "0" CACHE STRING "")

# vcpkg's packages look at this when they search for threads
set(THREADS_PREFER_PTHREAD_FLAG true)

execute_process(
	COMMAND git log --format=%h -1
	WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
	OUTPUT_VARIABLE PIXELU_COMMIT
	OUTPUT_STRIP_TRAILING_WHITESPACE
)
add_definitions(-DEMULATOR_HASH=${PIXELU_COMMIT})

if(ENABLE_VCPKG)
	set(PIXELU_VCPKG_DIR "${CMAKE_CURRENT_LIST_DIR}/dependencies/vcpkg")

	# vcpkg resolves port versions from its git history, a shallow clone lacks it
	execute_process(
		COMMAND git rev-parse --is-shallow-repository
		WORKING_DIRECTORY "${PIXELU_VCPKG_DIR}"
		OUTPUT_VARIABLE PIXELU_VCPKG_SHALLOW
		OUTPUT_STRIP_TRAILING_WHITESPACE
	)
	if(PIXELU_VCPKG_SHALLOW STREQUAL "true")
		message(STATUS "vcpkg is shallow. Unshallowing it now...")
		execute_process(COMMAND git fetch --unshallow WORKING_DIRECTORY "${PIXELU_VCPKG_DIR}")
	endif()

	# port overrides: one folder per platform plus the shared one
	if(WIN32)
		set(PIXELU_OVERLAY_PLATFORM win)
	elseif(APPLE)
		set(PIXELU_OVERLAY_PLATFORM mac)
	else()
		set(PIXELU_OVERLAY_PLATFORM linux)
	endif()
	set(VCPKG_OVERLAY_PORTS
		"${CMAKE_CURRENT_LIST_DIR}/dependencies/vcpkg_overlay_ports_${PIXELU_OVERLAY_PLATFORM};${CMAKE_CURRENT_LIST_DIR}/dependencies/vcpkg_overlay_ports")

	set(CMAKE_TOOLCHAIN_FILE "${PIXELU_VCPKG_DIR}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "Vcpkg toolchain file")
	# vcpkg installs CMake package configs, use them before any Find module
	set(CMAKE_FIND_PACKAGE_PREFER_CONFIG TRUE)
	if(WIN32)
		set(VCPKG_TARGET_TRIPLET "x64-windows-static" CACHE STRING "")
	endif()
endif()

project(PixelU VERSION 2.0.0)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

# ---------------------------------------------------------------------------
# Language and global compile settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# debug builds check internal assertions
add_compile_definitions($<$<CONFIG:Debug>:CEMU_DEBUG_ASSERT>)
add_definitions(
	-DEMULATOR_VERSION_MAJOR=${EMULATOR_VERSION_MAJOR}
	-DEMULATOR_VERSION_MINOR=${EMULATOR_VERSION_MINOR}
	-DEMULATOR_VERSION_PATCH=${EMULATOR_VERSION_PATCH}
)

# link time optimisation for the optimised configurations
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)

if(MSVC)
	set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT CemuBin)
	# C++ exceptions, precise floating point (the emulated CPU depends on it)
	add_compile_options(/EHsc /fp:precise)
	if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
		# clang-cl: thin archives make linking the large static libraries faster
		if((CMAKE_LINKER MATCHES ".*lld-link.*") AND (CMAKE_AR MATCHES ".*llvm-lib.*"))
			set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY STATIC_LIBRARY_OPTIONS /llvmlibthin)
		endif()
	else()
		add_compile_options(/GT) # fiber-safe thread local storage
	endif()
	# intrinsics and favour speed in the optimised configurations
	add_compile_options($<$<CONFIG:Release,RelWithDebInfo>:/Oi>)
	add_compile_options($<$<CONFIG:Release,RelWithDebInfo>:/Ot>)
endif()

if(CMAKE_GENERATOR STREQUAL "Xcode")
	set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY XCODE_STARTUP_PROJECT CemuBin)
endif()

if(APPLE)
	enable_language(OBJC OBJCXX)
	set(CMAKE_OSX_DEPLOYMENT_TARGET "13.4")
endif()

# ---------------------------------------------------------------------------
# Features
# ---------------------------------------------------------------------------
set(PIXELU_LINUX_DESKTOP OFF)
if(UNIX AND NOT APPLE)
	set(PIXELU_LINUX_DESKTOP ON)
	option(ENABLE_WAYLAND "Build with Wayland support" ON)
	option(ENABLE_FERAL_GAMEMODE "Enables Feral Interactive GameMode Support" ON)
	option(ENABLE_BLUEZ "Build with Bluez support" ON)
endif()

# macOS draws through Vulkan (MoltenVK) only
if(APPLE)
	set(PIXELU_OPENGL_DEFAULT OFF)
else()
	set(PIXELU_OPENGL_DEFAULT ON)
endif()
option(ENABLE_OPENGL "Enables the OpenGL backend" ${PIXELU_OPENGL_DEFAULT})
option(ENABLE_VULKAN "Enables the Vulkan backend" ON)
option(ENABLE_DISCORD_RPC "Enables the Discord Rich Presence feature" ON)

# input
if(WIN32)
	option(ENABLE_XINPUT "Enables the usage of XInput" ON)
	option(ENABLE_DIRECTINPUT "Enables the usage of DirectInput" ON)
	add_compile_definitions(HAS_DIRECTINPUT)
endif()
option(ENABLE_HIDAPI "Build with HIDAPI" ON)
option(ENABLE_SDL "Enables the SDLController backend" ON)
option(ENABLE_LIBUSB "Build with libusb support" ON)
if(ENABLE_LIBUSB)
	add_compile_definitions(HAS_LIBUSB)
endif()

# audio
if(WIN32)
	option(ENABLE_DIRECTAUDIO "Enables the directaudio backend" ON)
	option(ENABLE_XAUDIO "Enables the xaudio backend" ON)
endif()
option(ENABLE_CUBEB "Enabled cubeb backend" ON)

option(ENABLE_WXWIDGETS "Build with wxWidgets UI (Currently required)" ON)

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)
find_package(CURL REQUIRED)
find_package(pugixml REQUIRED)
find_package(RapidJSON REQUIRED)
find_package(Boost COMPONENTS program_options filesystem nowide REQUIRED)
find_package(libzip REQUIRED)
find_package(glslang REQUIRED)
find_package(ZLIB REQUIRED)
find_package(zstd MODULE REQUIRED) # cmake/Findzstd.cmake provides zstd::zstd for every zstd package layout
find_package(OpenSSL COMPONENTS Crypto SSL REQUIRED)
find_package(glm REQUIRED)
find_package(fmt 12.1 REQUIRED)
find_package(PNG REQUIRED)

if(ENABLE_SDL)
	find_package(SDL3 REQUIRED CONFIG REQUIRED COMPONENTS SDL3)
	add_compile_definitions(HAS_SDL)
endif()

# older glslang packages name the SPIR-V target without a namespace
if(NOT TARGET glslang::SPIRV AND TARGET SPIRV)
	add_library(glslang::SPIRV ALIAS SPIRV)
endif()

if(PIXELU_LINUX_DESKTOP)
	find_package(X11 REQUIRED)

	if(ENABLE_WAYLAND)
		find_package(Wayland REQUIRED Client)
		find_package(WaylandScanner REQUIRED)
		find_package(WaylandProtocols 1.15 REQUIRED)
		# viewporter protocol, used to scale the Vulkan/OpenGL surface
		ecm_add_wayland_client_protocol(PIXELU_WAYLAND_SOURCES
			PROTOCOL "${WaylandProtocols_DATADIR}/stable/viewporter/viewporter.xml"
			BASENAME viewporter)
		add_library(CemuWaylandProtocols STATIC ${PIXELU_WAYLAND_SOURCES})
		target_include_directories(CemuWaylandProtocols PUBLIC "${CMAKE_CURRENT_BINARY_DIR}")
		target_include_directories(CemuWaylandProtocols PRIVATE ${Wayland_INCLUDE_DIRS})
		add_compile_definitions(HAS_WAYLAND)
	endif()

	find_package(GTK3 REQUIRED)

	if(ENABLE_BLUEZ)
		find_package(bluez REQUIRED)
		set(SUPPORTS_WIIMOTE ON)
		add_compile_definitions(HAS_BLUEZ)
	endif()
endif()

if(ENABLE_VULKAN)
	# the loader is opened at run time, only the headers are needed
	include_directories("dependencies/Vulkan-Headers/include")
	add_compile_definitions(ENABLE_VULKAN VK_NO_PROTOTYPES)
endif()

if(ENABLE_OPENGL)
	find_package(OpenGL REQUIRED)
	add_compile_definitions(ENABLE_OPENGL)
endif()

if(ENABLE_DISCORD_RPC)
	add_compile_definitions(ENABLE_DISCORD_RPC)
endif()

if(ENABLE_HIDAPI)
	set(SUPPORTS_WIIMOTE ON)
	add_compile_definitions(HAS_HIDAPI)
endif()

if(PIXELU_LINUX_DESKTOP AND ENABLE_FERAL_GAMEMODE)
	add_compile_definitions(ENABLE_FERAL_GAMEMODE)
	add_subdirectory(dependencies/gamemode EXCLUDE_FROM_ALL)
	target_include_directories(gamemode INTERFACE ./dependencies/gamemode/lib)
endif()

if(ENABLE_WXWIDGETS)
	find_package(wxWidgets 3.3 REQUIRED COMPONENTS base core gl propgrid xrc)
endif()

if(ENABLE_CUBEB)
	# vcpkg builds use the cubeb submodule, other builds prefer an installed cubeb
	if(NOT ENABLE_VCPKG)
		find_package(cubeb)
	endif()
	if(NOT cubeb_FOUND)
		option(BUILD_TESTS "" OFF)
		option(BUILD_TOOLS "" OFF)
		option(BUNDLE_SPEEX "" OFF)
		set(USE_WINMM OFF CACHE BOOL "")
		add_subdirectory("dependencies/cubeb" EXCLUDE_FROM_ALL SYSTEM)
		set_property(TARGET cubeb PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
		add_library(cubeb::cubeb ALIAS cubeb)
	endif()
	add_compile_definitions("HAS_CUBEB=1")
endif()

# H.264 decoder for the Wii U video library
add_subdirectory("dependencies/ih264d" EXCLUDE_FROM_ALL)

if(CMAKE_OSX_ARCHITECTURES)
	set(CEMU_ARCHITECTURE ${CMAKE_OSX_ARCHITECTURES})
else()
	set(CEMU_ARCHITECTURE ${CMAKE_SYSTEM_PROCESSOR})
endif()
if(CEMU_ARCHITECTURE MATCHES "(aarch64)|(AARCH64)|(arm64)|(ARM64)")
	add_subdirectory("dependencies/xbyak_aarch64" EXCLUDE_FROM_ALL)
endif()

find_package(ZArchive QUIET)
if(NOT ZArchive_FOUND)
	add_subdirectory("dependencies/ZArchive" EXCLUDE_FROM_ALL)
endif()

add_subdirectory(src)
