// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "Cafe/HW/Latte/Core/LatteConst.h"
#include "util/containers/SmallBitset.h"

namespace Latte
{
	// Context independent facts about a shader program, gathered from its control flow.
	class ShaderDescription
	{
	public:
		// scans the control flow program, returns false if it cannot be understood
		bool analyzeShaderCode(void* shaderProgram, size_t sizeInBytes, LatteConst::ShaderType shaderType);

		// pixel shaders: bit n is set when colour target n is exported
		SmallBitset<8> renderTargetWriteMask;
	};
};
