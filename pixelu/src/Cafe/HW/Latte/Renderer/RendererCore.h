// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include "Cafe/HW/Latte/Core/LatteRingBuffer.h"
#include "Cafe/HW/Latte/Core/Latte.h"

// Draw helpers shared by the OpenGL and Vulkan backends.

// GPU7 special state 8: the draw clears the colour buffers that alias the depth buffer
void LatteDraw_handleSpecialState8_clearAsDepth();

// RECT primitives are drawn through a geometry shader that turns each 3-vertex rect into a 4-vertex strip.
// Appends the GLSL that emits the strip in the order p0..p3; index 3 is the generated fourth corner, computed with
// gen4thVertex<variant>(), all other indices pass input vertex pN through.
struct LatteShaderPSInputTable;
void rectsEmulationGS_outputVerticesCode(std::string& gsSrc, LatteDecompilerShader* vertexShader, LatteShaderPSInputTable* psInputTable, sint32 p0, sint32 p1, sint32 p2, sint32 p3, const char* variant, const LatteContextRegister& latteRegister);
