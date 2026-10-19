// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// query types accepted by GX2QueryBegin/GX2QueryEnd (1 and 3 are stream out queries, unsupported)
#define GX2_QUERY_TYPE_OCCLUSION_CPU			0
#define GX2_QUERY_TYPE_OCCLUSION_GPU			2

namespace GX2
{
	void GX2QueryInit();
};
