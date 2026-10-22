# Original code for RetroPixels (PixelU). Not derived from Cemu.
# CemuCommon: platform layer, shared headers and the precompiled header

add_library(CemuCommon)
pixelu_add_folder(CemuCommon .
	betype.h
	CafeString.h
	cpu_features.cpp
	cpu_features.h
	enumFlags.h
	FileStream.h
	MemPtr.h
	platform.h
	precompiled.cpp
	precompiled.h
	socket.h
	StackAllocator.h
	SysAllocator.cpp
	SysAllocator.h
	version.h
)
pixelu_add_folder(CemuCommon ExceptionHandler ExceptionHandler.cpp ExceptionHandler.h)
pixelu_add_folder(CemuCommon GLInclude
	egl.h
	glext.h
	glFunctions.h
	GLInclude.h
	glxext.h
	khrplatform.h
	wglext.h
)
pixelu_add_folder(CemuCommon windows FileStream_win32.h)

if(WIN32)
	pixelu_add_folder(CemuCommon windows FileStream_win32.cpp platform.cpp platform.h)
	pixelu_add_folder(CemuCommon ExceptionHandler ExceptionHandler_win32.cpp)
else()
	pixelu_add_folder(CemuCommon unix
		date.h
		fast_float.h
		FileStream_unix.cpp
		FileStream_unix.h
		platform.cpp
		platform.h
	)
	pixelu_add_folder(CemuCommon ExceptionHandler ExceptionHandler_posix.cpp)
endif()

if(LINUX)
	# symbol names for crash reports
	pixelu_add_folder(CemuCommon ExceptionHandler ELFSymbolTable.cpp ELFSymbolTable.h)
endif()

set_property(TARGET CemuCommon PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
# the precompiled header is built here (MSVC reuses it in the other libraries)
target_precompile_headers(CemuCommon PRIVATE precompiled.h)
target_include_directories(CemuCommon PUBLIC "../")

target_link_libraries(CemuCommon PRIVATE glm::glm)
if(UNIX AND NOT APPLE)
	target_link_libraries(CemuCommon PRIVATE X11::X11 X11::Xrender X11::Xutil)
endif()

# public: precompiled.h and platform.h include boost (nowide, predef) and fmt;
# Threads gives every user -pthread where the platform needs it
target_link_libraries(CemuCommon PUBLIC
	Threads::Threads
	Boost::nowide
	Boost::headers
	fmt::fmt
)
