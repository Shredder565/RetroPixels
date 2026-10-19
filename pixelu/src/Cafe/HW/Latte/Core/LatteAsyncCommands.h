// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// Work that CPU threads hand to the GPU thread (LatteAsyncCommands.cpp)

struct LatteSurfaceCopyParam;
struct LatteSurfaceCopyRect;

void LatteAsyncCommands_queueForceTextureReadback(MPTR physAddr, MPTR mipAddr, uint32 swizzle, sint32 format, sint32 width, sint32 height, sint32 depth, uint32 pitch, uint32 slice, sint32 dim, Latte::E_HWTILEMODE tilemode, sint32 aa, sint32 level);
void LatteAsyncCommand_queueTextureCopy(const LatteSurfaceCopyParam& src, const LatteSurfaceCopyParam& dst, const LatteSurfaceCopyRect& rect);
void LatteAsyncCommands_queueDeleteShader(uint64 shaderBaseHash, uint64 shaderAuxHash, LatteConst::ShaderType shaderType);
// blocks the calling (CPU) thread until the GPU thread ran every queued command
void LatteAsyncCommands_waitUntilAllProcessed();

// GPU thread: runs the queued commands
void LatteAsyncCommands_checkAndExecute();
