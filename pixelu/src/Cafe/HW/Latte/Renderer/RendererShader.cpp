// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/HW/Latte/Renderer/RendererShader.h"
#include "Cafe/GameProfile/GameProfile.h"

// The value is stored as the magic of the precompiled shader cache files, existing caches are only
// accepted when it matches. The formula therefore must not change.
uint32 RendererShader::GeneratePrecompiledCacheId()
{
	uint32 id = 0;
	for (const char* c = EMULATOR_VERSION_SUFFIX; *c != '\0'; c++)
		id = std::rotl<uint32>(id, 7) + (uint32)*c;
	id += EMULATOR_VERSION_MAJOR * 1000000u + EMULATOR_VERSION_MINOR * 10000u + EMULATOR_VERSION_PATCH * 100u;
	// accurate shader multiplication changes the generated code
	id += (uint32)g_current_game_profile->GetAccurateShaderMul() * 133;
	id += 0x820a5291;
	return id;
}

void RendererShader::GenerateShaderPrecompiledCacheFilename(ShaderType type, uint64 baseHash, uint64 auxHash, uint64& h1, uint64& h2)
{
	// stage specific offsets keep the stages of one shader apart
	uint64 stageSalt = 0;
	switch (type)
	{
	case ShaderType::kVertex:
		stageSalt = 0xA16374cull;
		break;
	case ShaderType::kFragment:
		stageSalt = 0x8752deull;
		break;
	case ShaderType::kGeometry:
		stageSalt = 0x65a035ull;
		break;
	}
	h1 = baseHash;
	h2 = auxHash + stageSalt;
}
