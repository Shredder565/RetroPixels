# Original code for RetroPixels (PixelU). Not derived from Cemu.
# CemuConfig: settings.xml, network_services.xml and the command line

add_library(CemuConfig)
pixelu_add_folder(CemuConfig .
	ActiveSettings.cpp
	ActiveSettings.h
	CemuConfig.cpp
	CemuConfig.h
	ConfigValue.h
	LaunchSettings.cpp
	LaunchSettings.h
	NetworkSettings.cpp
	NetworkSettings.h
	XMLConfig.h
)

pixelu_library(CemuConfig)

target_include_directories(CemuConfig PUBLIC "../")
target_link_libraries(CemuConfig PRIVATE CemuCommon CemuGui Boost::program_options)
