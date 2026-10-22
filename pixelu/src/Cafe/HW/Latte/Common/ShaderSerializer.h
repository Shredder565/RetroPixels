// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "util/helpers/Serializer.h"

namespace Latte
{
	// writes a shader program in the compressed pipeline cache format
	void SerializeShaderProgram(void* shaderProg, uint32 size, MemStreamWriter& memWriter);
	// reads it back, returns false for unknown versions or damaged data
	bool DeserializeShaderProgram(std::vector<uint8>& progData, MemStreamReader& memReader);
};
