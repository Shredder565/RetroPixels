// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/HW/Latte/Renderer/RendererCore.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "Cafe/HW/Latte/Core/LatteShader.h"
#include "config/CemuConfig.h"

#ifdef ENABLE_OPENGL
#include "Common/GLInclude/GLInclude.h"
#include "Cafe/HW/Latte/Renderer/OpenGL/LatteTextureViewGL.h"
#endif

// Special state 8 is set by GX2ClearDepthStencil-like sequences that clear a buffer through the depth path while a
// colour buffer occupies the same memory. Every texture view that matches the depth buffer's memory gets cleared
// to the colour stored in the first ALU constant.
void LatteDraw_handleSpecialState8_clearAsDepth()
{
	if (LatteGPUState.contextNew.GetSpecialStateValues()[0] == 0)
		cemuLog_logDebug(LogType::Force, "Special state 8 is used without special state 0");

	const uint32* regs = LatteGPUState.contextRegister;
	const uint32 htileBase = regs[mmDB_HTILE_DATA_BASE];
	const uint32 depthSize = regs[mmDB_DEPTH_SIZE];

	// DB_DEPTH_SIZE: PITCH_TILE_MAX (pitch/8 - 1) in bits 0-9, SLICE_TILE_MAX (pitch*height/64 - 1) in bits 10-29
	const MPTR surfaceAddr = htileBase << 8;
	const uint32 pitchInTiles = (depthSize & 0x3FF) + 1;
	const uint32 heightInTiles = (((depthSize >> 10) & 0xFFFFF) + 1) / pitchInTiles;
	const uint32 pitch = pitchInTiles * 8;
	const uint32 height = heightInTiles * 8;
	const uint32 width = pitch;

	const sint32 slice = 0;
	const sint32 mip = 0;

	// clear colour comes from the ALU constant registers (c0 of the pixel shader constant file)
	const float* constColor = reinterpret_cast<const float*>(regs + 0xC000);
	uint8 rgba[4];
	for (int i = 0; i < 4; i++)
		rgba[i] = (uint8)(constColor[i] * 255.0f);

	sint32 searchCursor = 0;
	for (LatteTextureView* view = LatteTC_LookupTextureByData(surfaceAddr, width, height, pitch, 0, 1, slice, 1, &searchCursor);
		 view != nullptr;
		 view = LatteTC_LookupTextureByData(surfaceAddr, width, height, pitch, 0, 1, slice, 1, &searchCursor))
	{
		LatteTexture* tex = view->baseTexture;
		sint32 clearWidth = tex->width;
		sint32 clearHeight = tex->height;
		LatteTexture_scaleToEffectiveSize(tex, &clearWidth, &clearHeight, 0);

#ifdef ENABLE_OPENGL
		if (g_renderer->GetType() == RendererAPI::OpenGL)
		{
			if (glClearTexSubImage)
				glClearTexSubImage(static_cast<LatteTextureViewGL*>(view)->glTexId, mip, 0, 0, 0, clearWidth, clearHeight, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
			continue;
		}
#endif
		// the other backends clear the whole slice; the colour is passed on as the raw 0-255 byte values
		if (tex->isDepth)
			g_renderer->texture_clearDepthSlice(tex, slice + view->firstSlice, mip + view->firstMip, true, tex->hasStencil, 0.0f, 0);
		else
			g_renderer->texture_clearColorSlice(tex, slice + view->firstSlice, mip + view->firstMip, rgba[0], rgba[1], rgba[2], rgba[3]);
	}
}

// ---- RECT emulation geometry shader ----

// calls emit(semanticId) for every vertex shader output that the pixel shader reads
template<typename TFunc>
static void ForEachForwardedParameter(LatteDecompilerShader* vertexShader, LatteShaderPSInputTable* psInputTable, const LatteContextRegister& latteRegister, TFunc emit)
{
	const uint32 outputMask = vertexShader->outputParameterMask;
	for (sint32 index = 0; index < 32; index++)
	{
		if ((outputMask & (1u << index)) == 0)
			continue;
		const sint32 semanticId = psInputTable->getVertexShaderOutParamSemanticId(latteRegister.GetRawView(), index);
		if (semanticId < 0 || !psInputTable->hasPSImportForSemanticId(semanticId))
			continue;
		emit(semanticId);
	}
}

void rectsEmulationGS_outputVerticesCode(std::string& gsSrc, LatteDecompilerShader* vertexShader, LatteShaderPSInputTable* psInputTable, sint32 p0, sint32 p1, sint32 p2, sint32 p3, const char* variant, const LatteContextRegister& latteRegister)
{
	for (sint32 vertex : {p0, p1, p2, p3})
	{
		if (vertex == 3)
		{
			// synthesized corner
			ForEachForwardedParameter(vertexShader, psInputTable, latteRegister, [&](sint32 sem) {
				gsSrc.append(fmt::format("passParameterSem{0}Out = gen4thVertex{1}(passParameterSem{0}In[0], passParameterSem{0}In[1], passParameterSem{0}In[2]);\r\n", sem, variant));
			});
			gsSrc.append(fmt::format("gl_Position = gen4thVertex{}(gl_in[0].gl_Position, gl_in[1].gl_Position, gl_in[2].gl_Position);\r\n", variant));
		}
		else
		{
			ForEachForwardedParameter(vertexShader, psInputTable, latteRegister, [&](sint32 sem) {
				gsSrc.append(fmt::format("passParameterSem{0}Out = passParameterSem{0}In[{1}];\r\n", sem, vertex));
			});
			gsSrc.append(fmt::format("gl_Position = gl_in[{}].gl_Position;\r\n", vertex));
		}
		gsSrc.append("EmitVertex();\r\n");
	}
}
