// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// version numbers stored in the shader and pipeline cache files; they include the title id
uint32 LatteShaderCache_getShaderCacheExtraVersion(uint64 titleId);
uint32 LatteShaderCache_getPipelineCacheExtraVersion(uint64 titleId);
