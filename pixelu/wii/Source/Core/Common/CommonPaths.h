// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

// Names of the directories and files in the user and Sys trees. These are part of the on-disk
// layout that existing user folders rely on, so the strings must not change.

#define DIR_SEP "/"
#define DIR_SEP_CHR '/'

#define ROOT_DIR "."

// User directory: NORMAL_USER_DIR lives in the platform's documents/home location;
// PORTABLE_USER_DIR sits next to the executable (portable.txt mode).
#ifdef _WIN32
#define NORMAL_USER_DIR "Dolphin Emulator"
#define PORTABLE_USER_DIR "User"
#else
#define NORMAL_USER_DIR "dolphin-emu"
#define PORTABLE_USER_DIR "user"
#define EMBEDDED_USER_DIR PORTABLE_USER_DIR
#endif

#define NOMEDIA_FILE ".nomedia"

// Region directories (present in both User and Sys). Old setups used JAP, newer ones JPN.
#define EUR_DIR "EUR"
#define USA_DIR "USA"
#define JAP_DIR "JAP"
#define JPN_DIR "JPN"
#define DEV_DIR "DEV"

// Subdirectories of the user directory
#define GC_USER_DIR "GC"
#define GBA_USER_DIR "GBA"
#define WII_USER_DIR "Wii"
#define TRI_USER_DIR "Triforce"
#define CONFIG_DIR "Config"
#define GAMESETTINGS_DIR "GameSettings"
#define MAPS_DIR "Maps"
#define CACHE_DIR "Cache"
#define COVERCACHE_DIR "GameCovers"
#define REDUMPCACHE_DIR "Redump"
#define SHADERCACHE_DIR "Shaders"
#define RETROACHIEVEMENTSCACHE_DIR "RetroAchievements"
#define STATESAVES_DIR "StateSaves"
#define SCREENSHOTS_DIR "ScreenShots"
#define LOAD_DIR "Load"
#define HIRES_TEXTURES_DIR "Textures"
#define RIIVOLUTION_DIR "Riivolution"
#define LOGS_DIR "Logs"
#define SHADERS_DIR "Shaders"
#define RESOURCES_DIR "Resources"
#define THEMES_DIR "Themes"
#define STYLES_DIR "Styles"
#define GBASAVES_DIR "Saves"
#define ANAGLYPH_DIR "Anaglyph"
#define PASSIVE_DIR "Passive"
#define PIPES_DIR "Pipes"
#define MEMORYWATCHER_DIR "MemoryWatcher"
#define WFSROOT_DIR "WFS"
#define BACKUP_DIR "Backup"
#define RESOURCEPACK_DIR "ResourcePacks"
#define DYNAMICINPUT_DIR "DynamicInputTextures"
#define GRAPHICSMOD_DIR "GraphicMods"
#define FIRMWARE_DIR "Firmware"
#define WIISDSYNC_DIR "WiiSDSync"
#define ASSEMBLY_DIR "SavedAssembly"
#define WIIBANNERS_DIR "WiiBanners"

// Dump directory and its subdirectories
#define DUMP_DIR "Dump"
#define DUMP_TEXTURES_DIR "Textures"
#define DUMP_FRAMES_DIR "Frames"
#define DUMP_OBJECTS_DIR "Objects"
#define DUMP_AUDIO_DIR "Audio"
#define DUMP_DSP_DIR "DSP"
#define DUMP_SSL_DIR "SSL"
#define DUMP_DEBUG_DIR "Debug"
#define DUMP_DEBUG_BRANCHWATCH_DIR "BranchWatch"
#define DUMP_DEBUG_JITBLOCKS_DIR "JitBlocks"

// Inside the emulated Wii NAND
#define WII_SYSCONF_DIR "shared2" DIR_SEP "sys"
#define WII_WC24CONF_DIR "shared2" DIR_SEP "wc24"

// Only looked up so that an old copy can be deleted
#define SHADERCACHE_LEGACY_DIR "ShaderCache"

#define DEFAULT_THEME_DIR "Clean"

// Config directory files
#define DOLPHIN_CONFIG "Dolphin.ini"
#define GCPAD_CONFIG "GCPadNew.ini"
#define WIIPAD_CONFIG "WiimoteNew.ini"
#define GCKEYBOARD_CONFIG "GCKeyNew.ini"
#define GFX_CONFIG "GFX.ini"
#define LOGGER_CONFIG "Logger.ini"
#define DUALSHOCKUDPCLIENT_CONFIG "DSUClient.ini"
#define FREELOOK_CONFIG "FreeLook.ini"
#define RETROACHIEVEMENTS_CONFIG "RetroAchievements.ini"

// Logs directory
#define MAIN_LOG "dolphin.log"

// Wii SYSCONF directory
#define WII_SYSCONF "SYSCONF"

// Dump directory files
#define MEM1_DUMP "mem1.raw"
#define MEM2_DUMP "mem2.raw"
#define ARAM_DUMP "aram.raw"
#define FAKEVMEM_DUMP "fakevmem.raw"

// Memory watcher directory
#define MEMORYWATCHER_LOCATIONS "Locations.txt"
#define MEMORYWATCHER_SOCKET "MemoryWatcher"

// Files the user supplies or the emulator keeps in Sys / GC / Wii directories
#define TOTALDB "totaldb.dsy"

#define FONT_WINDOWS_1252 "font_western.bin"
#define FONT_SHIFT_JIS "font_japanese.bin"

#define DSP_IROM "dsp_rom.bin"
#define DSP_COEF "dsp_coef.bin"

#define GC_IPL "IPL.bin"
#define GC_SRAM "SRAM.raw"
#define GC_MEMCARDA "MemoryCardA"
#define GC_MEMCARDB "MemoryCardB"
#define GC_MEMCARD_NETPLAY "NetPlayTemp"

#define GBA_BIOS "gba_bios.bin"
#define GBA_SAVE_NETPLAY "NetPlayTemp"

#define WII_STATE "state.dat"
#define WII_SD_CARD_IMAGE "WiiSD.raw"
#define WII_BTDINF_BACKUP "btdinf.bak"
#define WII_SETTING "setting.txt"

#define GECKO_CODE_HANDLER "codehandler.bin"

// Subdirectories of Sys
#define GC_SYS_DIR "GC"
#define WII_SYS_DIR "Wii"
#define TRI_SYS_DIR "Triforce"

// Subdirectories of Config
#define GRAPHICSMOD_CONFIG_DIR "GraphicMods"

// Custom GPU driver directories
#define GPU_DRIVERS "GpuDrivers"
#define GPU_DRIVERS_EXTRACTED "Extracted"
#define GPU_DRIVERS_TMP "Tmp"
#define GPU_DRIVERS_HOOK "Hook"
#define GPU_DRIVERS_FILE_REDIRECT "FileRedirect"
