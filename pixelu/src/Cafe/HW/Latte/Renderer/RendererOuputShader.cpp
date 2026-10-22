// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/HW/Latte/Renderer/RendererOuputShader.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "config/ActiveSettings.h"

RendererOutputShader* RendererOutputShader::s_copy_shader;
RendererOutputShader* RendererOutputShader::s_copy_shader_ud;
RendererOutputShader* RendererOutputShader::s_bicubic_shader;
RendererOutputShader* RendererOutputShader::s_bicubic_shader_ud;
RendererOutputShader* RendererOutputShader::s_hermit_shader;
RendererOutputShader* RendererOutputShader::s_hermit_shader_ud;

namespace
{
	// ---- GLSL filters (OpenGL and Vulkan), appended to the preamble ----

	// plain copy, alpha forced to 1
	const char* const kGlslCopy = R"(
void outputShader()
{
	colorOut0 = vec4(texture(textureSrc, passUV).rgb, 1.0);
}
)";

	// Cubic B-spline reconstruction. The 4x4 footprint is folded into 2x2 bilinear fetches: each pair of
	// taps is replaced by one fetch placed between the two texels according to their weight ratio.
	const char* const kGlslBicubic = R"(
vec4 bsplineWeights(float t)
{
	float t2 = t * t;
	float t3 = t2 * t;
	float s = 1.0 - t;
	return vec4(s * s * s, 3.0 * t3 - 6.0 * t2 + 4.0, -3.0 * t3 + 3.0 * t2 + 3.0 * t + 1.0, t3) / 6.0;
}

vec3 sampleBSpline(vec2 uv, vec2 texSize)
{
	vec2 pos = uv * texSize - 0.5;
	vec2 cell = floor(pos);
	vec2 t = pos - cell;
	vec4 wx = bsplineWeights(t.x);
	vec4 wy = bsplineWeights(t.y);
	// combined weights of the left/right (top/bottom) texel pairs
	vec2 gx = vec2(wx.x + wx.y, wx.z + wx.w);
	vec2 gy = vec2(wy.x + wy.y, wy.z + wy.w);
	// fetch positions in texel units: between texels (cell-1, cell) and (cell+1, cell+2)
	vec2 px = cell.x + vec2(-0.5 + wx.y / gx.x, 1.5 + wx.w / gx.y);
	vec2 py = cell.y + vec2(-0.5 + wy.y / gy.x, 1.5 + wy.w / gy.y);
	vec2 invSize = 1.0 / texSize;
	vec4 c00 = texture(textureSrc, vec2(px.x, py.x) * invSize);
	vec4 c10 = texture(textureSrc, vec2(px.y, py.x) * invSize);
	vec4 c01 = texture(textureSrc, vec2(px.x, py.y) * invSize);
	vec4 c11 = texture(textureSrc, vec2(px.y, py.y) * invSize);
	float fx = gx.x / (gx.x + gx.y);
	float fy = gy.x / (gy.x + gy.y);
	vec4 rowTop = c00 * fx + c10 * (1.0 - fx);
	vec4 rowBottom = c01 * fx + c11 * (1.0 - fx);
	return (rowTop * fy + rowBottom * (1.0 - fy)).rgb;
}

void outputShader()
{
	colorOut0 = vec4(sampleBSpline(passUV, textureSrcResolution), 1.0);
}
)";

	// Catmull-Rom (cubic Hermite) reconstruction with 16 point-sampled taps
	const char* const kGlslHermite = R"(
vec4 catmullRomWeights(float t)
{
	float t2 = t * t;
	float t3 = t2 * t;
	return vec4(-t3 + 2.0 * t2 - t, 3.0 * t3 - 5.0 * t2 + 2.0, -3.0 * t3 + 4.0 * t2 + t, t3 - t2) * 0.5;
}

vec3 hermiteRow(vec2 base, float dy, vec2 texel, vec4 w)
{
	vec3 a = texture(textureSrc, base + vec2(-texel.x, dy)).rgb;
	vec3 b = texture(textureSrc, base + vec2(0.0, dy)).rgb;
	vec3 c = texture(textureSrc, base + vec2(texel.x, dy)).rgb;
	vec3 d = texture(textureSrc, base + vec2(2.0 * texel.x, dy)).rgb;
	return a * w.x + b * w.y + c * w.z + d * w.w;
}

vec3 sampleHermite(vec2 uv, vec2 texSize)
{
	vec2 texel = 1.0 / texSize;
	vec2 pos = uv * texSize + 0.5;
	vec2 t = fract(pos);
	// centre of the second texel of the 4x4 block
	vec2 base = floor(pos) / texSize - texel * 0.5;
	vec4 wx = catmullRomWeights(t.x);
	vec4 wy = catmullRomWeights(t.y);
	vec3 r0 = hermiteRow(base, -texel.y, texel, wx);
	vec3 r1 = hermiteRow(base, 0.0, texel, wx);
	vec3 r2 = hermiteRow(base, texel.y, texel, wx);
	vec3 r3 = hermiteRow(base, 2.0 * texel.y, texel, wx);
	return r0 * wy.x + r1 * wy.y + r2 * wy.z + r3 * wy.w;
}

void outputShader()
{
	colorOut0 = vec4(sampleHermite(passUV, textureSrcResolution), 1.0);
}
)";

	// The quad is drawn as two triangles (6 vertices, no vertex buffer). Corner order:
	// (1,1) (-1,1) (-1,-1) (-1,-1) (1,-1) (1,1); UVs follow the position, flipped vertically when upside down.
	std::string BuildQuadVertexShader(const char* versionLine, const char* outDecl, const char* vertexIdName, bool upsideDown)
	{
		std::string src;
		src.reserve(640);
		src += versionLine;
		src += "\n";
		src += outDecl;
		src += R"(
out gl_PerVertex
{
	vec4 gl_Position;
};

const vec2 quadCorners[6] = vec2[6](vec2(1.0, 1.0), vec2(-1.0, 1.0), vec2(-1.0, -1.0), vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0));

void main()
{
	vec2 corner = quadCorners[)";
		src += vertexIdName;
		src += R"(];
	passUV = vec2(corner.x * 0.5 + 0.5, )";
		src += upsideDown ? "0.5 - corner.y * 0.5" : "corner.y * 0.5 + 0.5";
		src += R"();
	gl_Position = vec4(corner, 0.0, 1.0);
}
)";
		return src;
	}
}

std::string RendererOutputShader::GetOpenGlVertexSource(bool render_upside_down)
{
	return BuildQuadVertexShader("#version 420", "layout(location = 0) smooth out vec2 passUV;", "gl_VertexID", render_upside_down);
}

std::string RendererOutputShader::GetVulkanVertexSource(bool render_upside_down)
{
	return BuildQuadVertexShader("#version 450", "layout(location = 0) out vec2 passUV;", "gl_VertexIndex", render_upside_down);
}

// Declarations shared by the built-in filters and graphic pack output shaders. The names (passUV, textureSrc,
// colorOut0, the members of "parameters", sRGBEncode, outputShader) are what graphic packs are written against.
std::string RendererOutputShader::PrependFragmentPreamble(const std::string& shaderSrc)
{
	static const char* const kPreamble = R"(#version 430
layout(location = 0) smooth in vec2 passUV;
layout(binding = 0) uniform sampler2D textureSrc;
layout(location = 0) out vec4 colorOut0;

#ifdef VULKAN
layout(binding = 1, std140)
#else
layout(binding = 0, std140)
#endif
uniform parameters
{
	vec2 textureSrcResolution;
	vec2 nativeResolution;
	vec2 outputResolution;
	bool applySRGBEncoding;
	float targetGamma;
	float displayGamma;
};

float sRGBEncode(float linear)
{
	return linear <= 0.0031308 ? 12.92 * linear : 1.055 * pow(linear, 1.0 / 2.4) - 0.055;
}

vec3 sRGBEncode(vec3 linear)
{
	return vec3(sRGBEncode(linear.r), sRGBEncode(linear.g), sRGBEncode(linear.b));
}

void outputShader();

void main()
{
	outputShader();
	if (applySRGBEncoding)
		colorOut0 = vec4(sRGBEncode(colorOut0.rgb), 1.0);
	if (displayGamma > 0.0)
		colorOut0 = pow(colorOut0, vec4(targetGamma / displayGamma));
	else
		colorOut0 = vec4(sRGBEncode(pow(colorOut0.rgb, vec3(targetGamma))), 1.0);
}

)";
	return kPreamble + shaderSrc;
}

RendererOutputShader::RendererOutputShader(const std::string& vertex_source, const std::string& fragment_source)
{
	const std::string fragmentCode = PrependFragmentPreamble(fragment_source);

	m_vertex_shader.reset(g_renderer->shader_create(RendererShader::ShaderType::kVertex, 0, 0, vertex_source, false, false));
	m_fragment_shader.reset(g_renderer->shader_create(RendererShader::ShaderType::kFragment, 0, 0, fragmentCode, false, false));
	m_vertex_shader->PreponeCompilation(true);
	m_fragment_shader->PreponeCompilation(true);
	if (!m_vertex_shader->WaitForCompiled() || !m_fragment_shader->WaitForCompiled())
		throw std::exception();
}

RendererOutputShader::OutputUniformVariables RendererOutputShader::FillUniformBlockBuffer(const LatteTextureView& texture_view, const Vector2i& output_res, const bool padView) const
{
	OutputUniformVariables vars;
	sint32 scaledWidth, scaledHeight;
	texture_view.baseTexture->GetEffectiveSize(scaledWidth, scaledHeight, 0);
	vars.textureSrcResolution = Vector2f((float)scaledWidth, (float)scaledHeight);
	vars.nativeResolution = Vector2f((float)texture_view.baseTexture->width, (float)texture_view.baseTexture->height);
	vars.outputResolution = output_res;
	if (padView)
	{
		vars.applySRGBEncoding = LatteGPUState.drcBufferUsesSRGB;
		vars.targetGamma = ActiveSettings::GetDRCGamma();
	}
	else
	{
		vars.applySRGBEncoding = LatteGPUState.tvBufferUsesSRGB;
		vars.targetGamma = ActiveSettings::GetTVGamma();
	}
	vars.displayGamma = GetConfig().userDisplayGamma;
	return vars;
}

void RendererOutputShader::InitializeStatic()
{
	const char* copySrc = kGlslCopy;
	const char* bicubicSrc = kGlslBicubic;
	const char* hermiteSrc = kGlslHermite;
	std::string vs, vsUpsideDown;
	switch (g_renderer->GetType())
	{
#ifdef ENABLE_OPENGL
	case RendererAPI::OpenGL:
		vs = GetOpenGlVertexSource(false);
		vsUpsideDown = GetOpenGlVertexSource(true);
		break;
#endif
#ifdef ENABLE_VULKAN
	case RendererAPI::Vulkan:
		vs = GetVulkanVertexSource(false);
		vsUpsideDown = GetVulkanVertexSource(true);
		break;
#endif
	default:
		return;
	}
	s_copy_shader = new RendererOutputShader(vs, copySrc);
	s_copy_shader_ud = new RendererOutputShader(vsUpsideDown, copySrc);
	s_bicubic_shader = new RendererOutputShader(vs, bicubicSrc);
	s_bicubic_shader_ud = new RendererOutputShader(vsUpsideDown, bicubicSrc);
	s_hermit_shader = new RendererOutputShader(vs, hermiteSrc);
	s_hermit_shader_ud = new RendererOutputShader(vsUpsideDown, hermiteSrc);
}

void RendererOutputShader::ShutdownStatic()
{
	for (RendererOutputShader* shader : {s_copy_shader, s_copy_shader_ud, s_bicubic_shader, s_bicubic_shader_ud, s_hermit_shader, s_hermit_shader_ud})
		delete shader;
}
