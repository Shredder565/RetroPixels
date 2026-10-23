// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// Fixed-size integer names and the success/failure result used across the
// emulator. C files (the 68000 core) include this too, so it stays plain C
// apart from the scoped enum, which only C++ gets.

#ifndef INCLUDED_TYPES_H
#define INCLUDED_TYPES_H

#ifdef __cplusplus
enum class Result { OKAY, FAIL };
#else
enum Result { OKAY, FAIL };
#endif

typedef unsigned char      UINT8;
typedef signed char        INT8;
typedef unsigned short     UINT16;
typedef signed short       INT16;
typedef unsigned int       UINT32;
typedef signed int         INT32;
typedef unsigned long long UINT64;
typedef signed long long   INT64;
typedef float              FLOAT32;
typedef double             FLOAT64;

#endif // INCLUDED_TYPES_H
