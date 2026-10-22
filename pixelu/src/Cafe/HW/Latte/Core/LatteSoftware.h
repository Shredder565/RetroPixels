// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// CPU interpreter for a subset of vertex programs (LatteSoftware.cpp)
void LatteSoftware_setupVertexShader(struct LatteFetchShader* fetchShader, void* shaderPtr, sint32 size);
void LatteSoftware_executeVertex(sint32 index);

// position written by the last executed vertex (4 floats)
float* LatteSoftware_getPositionExport();
