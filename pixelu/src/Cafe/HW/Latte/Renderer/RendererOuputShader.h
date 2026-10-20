// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include "Cafe/HW/Latte/Renderer/RendererShader.h"
#include "Cafe/HW/Latte/Core/LatteTexture.h"
#include "util/math/vector2.h"

// Shader pair that draws a scan buffer (TV or GamePad picture) into the window, with an optional upscaling filter.
// Graphic packs can supply their own fragment code; it is appended to the preamble from PrependFragmentPreamble()
// and has to define outputShader().
class RendererOutputShader
{
public:
	// matches the "parameters" uniform block of the preamble (std140)
	struct OutputUniformVariables
	{
		Vector2f textureSrcResolution;
		Vector2f nativeResolution;
		Vector2f outputResolution;
		uint32 applySRGBEncoding;
		float targetGamma;
		float displayGamma;
	};

	enum Shader
	{
		kCopy,
		kBicubic,
		kHermit,
	};

	RendererOutputShader(const std::string& vertex_source, const std::string& fragment_source);
	virtual ~RendererOutputShader() = default;

	OutputUniformVariables FillUniformBlockBuffer(const LatteTextureView& texture_view, const Vector2i& output_res, const bool padView) const;

	RendererShader* GetVertexShader() const { return m_vertex_shader.get(); }
	RendererShader* GetFragmentShader() const { return m_fragment_shader.get(); }

	// built-in filters, normal and upside-down variants
	static void InitializeStatic();
	static void ShutdownStatic();

	static RendererOutputShader* s_copy_shader;
	static RendererOutputShader* s_copy_shader_ud;
	static RendererOutputShader* s_bicubic_shader;
	static RendererOutputShader* s_bicubic_shader_ud;
	static RendererOutputShader* s_hermit_shader;
	static RendererOutputShader* s_hermit_shader_ud;

	static std::string GetOpenGlVertexSource(bool render_upside_down);
	static std::string GetVulkanVertexSource(bool render_upside_down);

	static std::string PrependFragmentPreamble(const std::string& shaderSrc);

protected:
	std::unique_ptr<RendererShader> m_vertex_shader;
	std::unique_ptr<RendererShader> m_fragment_shader;
};
