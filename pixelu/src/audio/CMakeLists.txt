# Original code for RetroPixels (PixelU). Not derived from Cemu.
# CemuAudio: sound output and microphone input backends

add_library(CemuAudio)
pixelu_add_folder(CemuAudio . IAudioAPI.cpp IAudioAPI.h IAudioInputAPI.cpp IAudioInputAPI.h)

pixelu_library(CemuAudio)

if(WIN32)
	# DirectSound, XAudio 2.9 and XAudio 2.7 (xaudio2_7/ holds the old SDK headers)
	pixelu_add_folder(CemuAudio .
		DirectSoundAPI.cpp
		DirectSoundAPI.h
		XAudio27API.cpp
		XAudio27API.h
		XAudio2API.cpp
		XAudio2API.h
	)
	pixelu_add_folder(CemuAudio xaudio2_7 audiodefs.h comdecl.h dxsdkver.h xma2defs.h)
endif()

if(ENABLE_CUBEB)
	pixelu_add_folder(CemuAudio . CubebAPI.cpp CubebAPI.h CubebInputAPI.cpp CubebInputAPI.h)
endif()

target_include_directories(CemuAudio PUBLIC "../")
target_link_libraries(CemuAudio PRIVATE CemuCommon CemuGui)
if(ENABLE_CUBEB)
	# public: CubebAPI.h includes cubeb.h
	target_link_libraries(CemuAudio PUBLIC cubeb::cubeb)
endif()
