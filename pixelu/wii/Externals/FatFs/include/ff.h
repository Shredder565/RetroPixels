// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.
//
// What PixelU-Wii's code gets for #include "ff.h": FatFs' own ff.h, with the
// three names FatFs and Windows both define kept apart. FatFs declares its
// path type as TCHAR (char here, as the API is UTF-8) and the string macros
// _T and _TEXT; <windows.h> and <tchar.h> declare the same names for wide
// characters. Seen through this header, FatFs' type is FATFS_TCHAR, and TCHAR,
// _T and _TEXT keep whatever meaning they had before (or get later). FatFs'
// own sources (ff.c) include the real ff.h directly and are unchanged.
//
// PIXELU_FATFS_FF_H is the path of the real ff.h (Externals/FatFs/CMakeLists.txt).

#ifndef PIXELU_FATFS_WRAPPER_H
#define PIXELU_FATFS_WRAPPER_H

#pragma push_macro("TCHAR")
#pragma push_macro("_T")
#pragma push_macro("_TEXT")
#undef TCHAR
#undef _T
#undef _TEXT

#define TCHAR FATFS_TCHAR
#include PIXELU_FATFS_FF_H

#undef TCHAR
#undef _T
#undef _TEXT
#pragma pop_macro("_TEXT")
#pragma pop_macro("_T")
#pragma pop_macro("TCHAR")

#endif
