// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/HW/Latte/Renderer/OpenGL/RendererShaderGL.h"
#include "Cafe/HW/Latte/Renderer/OpenGL/OpenGLRenderer.h"
#include "Cafe/HW/Latte/Core/LatteShader.h"

namespace
{
	// staging for remapped uniforms, up to 256 vec4 entries
	GLint s_remappedStaging[256 * 4];

	// two rolling 64-bit hashes over the vec4 entries, used to skip uploads of unchanged data
	void HashUniformEntries(const GLint* data, sint32 entryCount, uint64 hashOut[2])
	{
		uint64 lo = 0, hi = 0;
		const uint64* p = reinterpret_cast<const uint64*>(data);
		for (sint32 i = 0; i < entryCount; i++, p += 2)
		{
			lo = std::rotl<uint64>(lo ^ p[0], 11);
			hi = std::rotl<uint64>(hi ^ p[1], 11);
		}
		hashOut[0] = lo;
		hashOut[1] = hi;
	}

	struct StageUniformSource
	{
		LatteDecompilerShader* shader;
		uint32 aluConstBase; // register offset of the stage's ALU constants, UINT32_MAX if it has none
	};
}

void OpenGLRenderer::uniformData_update()
{
	const StageUniformSource stages[3] = {
		{ LatteSHRC_GetActiveVertexShader(), 0x400 },
		{ LatteSHRC_GetActivePixelShader(), 0 },
		{ LatteSHRC_GetActiveGeometryShader(), 0xFFFFFFFF }, // the GS stage has no ALU constants
	};
	for (const StageUniformSource& stage : stages)
	{
		LatteDecompilerShader* shader = stage.shader;
		if (!shader)
			continue;
		auto* hostShader = static_cast<RendererShaderGL*>(shader->shader);
		switch (shader->uniformMode)
		{
		case LATTE_DECOMPILER_UNIFORM_MODE_REMAPPED:
		{
			const sint32 entryCount = (sint32)shader->list_remappedUniformEntries.size();
			cemu_assert_debug(entryCount <= 256);
			LatteBufferCache_LoadRemappedUniforms(shader, reinterpret_cast<float*>(s_remappedStaging), true, (1 << LATTE_NUM_MAX_UNIFORM_BUFFERS) - 1);
			if (entryCount <= 0)
				break;
			uint64 hash[2];
			HashUniformEntries(s_remappedStaging, entryCount, hash);
			if (hash[0] == shader->uniformDataHash64[0] && hash[1] == shader->uniformDataHash64[1])
				break;
			shader->uniformDataHash64[0] = hash[0];
			shader->uniformDataHash64[1] = hash[1];
			hostShader->SetUniform4iv(shader->uniform.loc_remapped, s_remappedStaging, entryCount);
			break;
		}
		case LATTE_DECOMPILER_UNIFORM_MODE_FULL_CFILE:
		{
			if (stage.aluConstBase == 0xFFFFFFFF)
				assert_dbg();
			GLint* constants = reinterpret_cast<GLint*>(LatteGPUState.contextRegister + mmSQ_ALU_CONSTANT0_0 + stage.aluConstBase);
			hostShader->SetUniform4iv(shader->uniform.loc_uniformRegister, constants, shader->uniform.count_uniformRegister);
			break;
		}
		default:
			// FULL_CBANK goes through uniform buffers, NONE has nothing to upload
			break;
		}
	}
}
