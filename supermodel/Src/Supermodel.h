// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// Header included by nearly every emulator source file: the C library
// basics, the fixed-size integer names (Types.h, from the OSD folder of the
// port being built), logging, and a few small helpers and enums shared by
// the video and sound code.

#ifndef INCLUDED_SUPERMODEL_H
#define INCLUDED_SUPERMODEL_H

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "Version.h"

// Case-insensitive C string compare under the name the code uses.
#if defined(_MSC_VER)
#define stricmp _stricmp
#elif defined(__GNUC__)
#define stricmp strcasecmp
#endif

// Rotates the 32-bit lvalue val by shift bits, in place. (Only ever used
// with a shift of 16, where the direction does not matter.)
#define rotl(val, shift) val = ((val) >> (shift)) | ((val) << (32 - (shift)))

// UINT8..UINT64, INT8..INT64, FLOAT32, FLOAT64 and Result (OKAY/FAIL).
// Types.h is shared with C files, so it holds no C++.
#include "Types.h"

// ErrorLog(), InfoLog(), DebugLog().
#include "OSD/Logger.h"

// Colour transforms that imitate a CRT or TV standard (config "CRTcolors").
#ifdef __cplusplus
enum class CRTcolor
#else
enum CRTcolor
#endif
{
  None = 0,
  ARI = 1,
  PVM = 2,
  BT601JP = 3,
  BT601US = 4,
  BT601EA = 5
};

// Filter used when the 2D layers are scaled up (config "UpscaleMode").
#ifdef __cplusplus
enum class UpscaleMode
#else
enum UpscaleMode
#endif
{
  Nearest = 0,
  Biquintic = 1,
  Bilinear = 2,
  Bicubic = 3
};

// Byte order reversal of a 16- or 32-bit value.
static inline UINT16 FLIPENDIAN16(UINT16 v)
{
  return (UINT16)((v << 8) | (v >> 8));
}

static inline UINT32 FLIPENDIAN32(UINT32 v)
{
  return (v << 24) | ((v & 0x0000FF00) << 8) | ((v >> 8) & 0x0000FF00) | (v >> 24);
}

#endif // INCLUDED_SUPERMODEL_H
