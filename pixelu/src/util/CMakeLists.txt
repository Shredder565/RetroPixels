# Original code for RetroPixels (PixelU). Not derived from Cemu.
# CemuUtil: helpers, containers, crypto, the Zir shader IR and per-OS system code

add_library(CemuUtil)
pixelu_add_folder(CemuUtil . EventService.h)
pixelu_add_folder(CemuUtil boost bluetooth.h)
pixelu_add_folder(CemuUtil bootSound BootSoundReader.cpp BootSoundReader.h)
pixelu_add_folder(CemuUtil ChunkedHeap ChunkedHeap.h)
pixelu_add_folder(CemuUtil containers
	flat_hash_map.hpp
	IntervalBucketContainer.h
	LookupTableL3.h
	RangeStore.h
	robin_hood.h
	SmallBitset.h
)
pixelu_add_folder(CemuUtil crypto
	aes128.cpp
	aes128.h
	crc32.cpp
	crc32.h
	md5.cpp
	md5.h
)
pixelu_add_folder(CemuUtil DXGIWrapper DXGIWrapper.h)
pixelu_add_folder(CemuUtil Fiber Fiber.h)
pixelu_add_folder(CemuUtil helpers
	ClassWrapper.h
	ConcurrentQueue.h
	enum_array.hpp
	fixedSizeList.h
	fspinlock.h
	helpers.cpp
	helpers.h
	MapAdaptor.h
	MemoryPool.h
	ringbuffer.h
	Semaphore.h
	Serializer.cpp
	Serializer.h
	Singleton.h
	StateHasher.h
	StringBuf.h
	StringHelpers.h
	StringParser.h
	SystemException.h
	TempState.h
	ZipArchive.h
)
pixelu_add_folder(CemuUtil highresolutiontimer HighResolutionTimer.cpp HighResolutionTimer.h)
pixelu_add_folder(CemuUtil ImageWriter bmp.h tga.h)
pixelu_add_folder(CemuUtil IniParser IniParser.cpp IniParser.h)
pixelu_add_folder(CemuUtil math
	glm.h
	quaternion.h
	vector2.h
	vector3.h
)
pixelu_add_folder(CemuUtil MemMapper MemMapper.h)
pixelu_add_folder(CemuUtil ScreenSaver ScreenSaver.h)
pixelu_add_folder(CemuUtil SystemInfo SystemInfo.cpp SystemInfo.h)
pixelu_add_folder(CemuUtil ThreadPool ThreadPool.h)
pixelu_add_folder(CemuUtil tinyxml2 tinyxml2.cpp tinyxml2.h)
pixelu_add_folder(CemuUtil VirtualHeap VirtualHeap.cpp VirtualHeap.h)
pixelu_add_folder(CemuUtil Zir/Core
	IR.cpp
	IR.h
	ZirUtility.h
	ZpIRBuilder.h
	ZpIRDebug.h
	ZpIRPasses.h
	ZpIRScheduler.h
)
pixelu_add_folder(CemuUtil Zir/EmitterGLSL ZpIREmitGLSL.cpp ZpIREmitGLSL.h)
pixelu_add_folder(CemuUtil Zir/Passes RegisterAllocatorForGLSL.cpp ZpIRRegisterAllocator.cpp)

if(WIN32)
	pixelu_add_folder(CemuUtil Fiber FiberWin.cpp)
	pixelu_add_folder(CemuUtil MemMapper MemMapperWin.cpp)
	pixelu_add_folder(CemuUtil SystemInfo SystemInfoWin.cpp)
elseif(UNIX)
	pixelu_add_folder(CemuUtil Fiber FiberUnix.cpp)
	pixelu_add_folder(CemuUtil MemMapper MemMapperUnix.cpp)
	if(APPLE)
		pixelu_add_folder(CemuUtil SystemInfo SystemInfoUnix.cpp SystemInfoMac.cpp)
	else()
		pixelu_add_folder(CemuUtil SystemInfo SystemInfoUnix.cpp SystemInfoLinux.cpp)
	endif()
else()
	pixelu_add_folder(CemuUtil SystemInfo SystemInfoStub.cpp)
endif()

pixelu_library(CemuUtil)

if(ENABLE_LIBUSB)
	pixelu_add_folder(CemuUtil libusbWrapper libusbWrapper.cpp libusbWrapper.h)
endif()

target_include_directories(CemuUtil PUBLIC "../")
target_link_libraries(CemuUtil PRIVATE CemuCommon)
