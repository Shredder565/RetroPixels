// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Common/precompiled.h"
#include "DebugSymbolStorage.h"

FSpinlock DebugSymbolStorage::s_lock;
std::unordered_map<MPTR, DEBUG_SYMBOL_TYPE> DebugSymbolStorage::s_typeStorage;
