// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// One compiled host shader stage. Backends derive from this (RendererShaderGL, RendererShaderVk, RendererShaderMtl).
class RendererShader
{
public:
	enum class ShaderType
	{
		kVertex,
		kFragment,
		kGeometry
	};

	virtual ~RendererShader() = default;

	ShaderType GetType() const { return m_type; }

	// makes sure the shader is compiled when this returns, compiling it right away if that is possible
	virtual void PreponeCompilation(bool isRenderThread) = 0;
	virtual bool IsCompiled() = 0;
	// blocks until compilation has finished, returns false if it failed
	virtual bool WaitForCompiled() = 0;

protected:
	// baseHash/auxHash identify game shaders (isGameShader) in the shader caches
	RendererShader(ShaderType type, uint64 baseHash, uint64 auxHash, bool isGameShader, bool isGfxPackShader)
		: m_type(type), m_baseHash(baseHash), m_auxHash(auxHash), m_isGameShader(isGameShader), m_isGfxPackShader(isGfxPackShader) {}

	// magic of the precompiled shader caches; changes with the emulator version and shader relevant settings
	static uint32 GeneratePrecompiledCacheId();
	// key of a shader inside the precompiled cache
	static void GenerateShaderPrecompiledCacheFilename(ShaderType type, uint64 baseHash, uint64 auxHash, uint64& h1, uint64& h2);

	ShaderType m_type;
	uint64 m_baseHash;
	uint64 m_auxHash;
	bool m_isGameShader;
	bool m_isGfxPackShader;
};
