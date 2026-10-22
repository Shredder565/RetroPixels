// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "Cafe/HW/Latte/Core/LatteConst.h"
#include "Cafe/HW/Latte/ISA/LatteReg.h"

// Index buffer conversion (guest index formats and primitive types to host index buffers)

// drops cached conversions that read from the given guest memory
void LatteIndices_invalidate(const void* memPtr, uint32 size);
void LatteIndices_invalidateAll();
// converts `count` indices and reports the highest index, the host index type and the number of output indices
void LatteIndices_decode(const void* indexData, LatteIndexType indexType, uint32 count, LattePrimitiveMode primitiveMode, uint32& indexMax, Renderer::INDEX_TYPE& renderIndexType, uint32& outputCount, Renderer::IndexAllocation& indexAllocation);
