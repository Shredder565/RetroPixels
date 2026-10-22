// Original code for RetroPixels (PixelU). Not derived from Cemu.
#ifndef EMULATOR_NAME

#define EMULATOR_NAME "PixelU"
#define EMULATOR_VERSION_SUFFIX ""

// stringify after macro expansion
#define _STRINGFY(s) #s
#define _XSTRINGFY(s) _STRINGFY(s)

// "major.minor" when the build sets a version, otherwise the commit hash
#if EMULATOR_VERSION_MAJOR != 0
	#define PIXELU_VERSION_TEXT _XSTRINGFY(EMULATOR_VERSION_MAJOR) "." _XSTRINGFY(EMULATOR_VERSION_MINOR) EMULATOR_VERSION_SUFFIX
#else
	#define PIXELU_VERSION_TEXT _XSTRINGFY(EMULATOR_HASH) EMULATOR_VERSION_SUFFIX
#endif

#define BUILD_VERSION_STRING (PIXELU_VERSION_TEXT)
#define BUILD_VERSION_WITH_NAME_STRING (EMULATOR_NAME " " PIXELU_VERSION_TEXT)

#endif
