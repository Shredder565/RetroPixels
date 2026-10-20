# Original code for RetroPixels (PixelU). Not derived from Cemu.
# CemuComponents: logging, file cache, online services (NAPI, NEX), the PPC
# assembler, the title download manager and Discord presence

add_library(CemuComponents)
pixelu_add_folder(CemuComponents ExpressionParser ExpressionParser.cpp ExpressionParser.h)
pixelu_add_folder(CemuComponents FileCache FileCache.cpp FileCache.h)
pixelu_add_folder(CemuComponents Logging CemuDebugLogging.h CemuLogging.cpp CemuLogging.h)
pixelu_add_folder(CemuComponents napi
	napi.cpp
	napi.h
	napi_act.cpp
	napi_ec.cpp
	napi_helper.cpp
	napi_helper.h
	napi_idbe.cpp
	napi_version.cpp
)
pixelu_add_folder(CemuComponents ncrypto ncrypto.cpp ncrypto.h)
pixelu_add_folder(CemuComponents nex
	nex.cpp
	nex.h
	nexFriends.cpp
	nexFriends.h
	nexThread.cpp
	nexThread.h
	nexTypes.h
	prudp.cpp
	prudp.h
)
pixelu_add_folder(CemuComponents PPCAssembler ppcAssembler.cpp ppcAssembler.h)
pixelu_add_folder(CemuComponents Tools/DownloadManager DownloadManager.cpp DownloadManager.h)

if(ENABLE_DISCORD_RPC)
	pixelu_add_folder(CemuComponents DiscordPresence
		DiscordPresence.cpp
		DiscordPresence.h
		DiscordRPCLite.cpp
		DiscordRPCLite.h
	)
endif()

pixelu_library(CemuComponents)

target_include_directories(CemuComponents PUBLIC "../")
target_link_libraries(CemuComponents PRIVATE CemuCommon CemuGui)
# public: ExpressionParser.h includes fmt
target_link_libraries(CemuComponents PUBLIC fmt::fmt)
