// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Common/GLInclude/GLInclude.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LatteDraw.h"
#include "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompiler.h"
#include "Cafe/HW/Latte/Renderer/OpenGL/OpenGLRenderer.h"
#include "Cafe/HW/Latte/Renderer/OpenGL/RendererShaderGL.h"
#include "Cafe/HW/Latte/Renderer/OpenGL/OpenGLSurfaceCopy.h"

// Looks up the uniform locations of the helper uniforms the GLSL emitter declares (OpenGL only)
void LatteShader_prepareSeparableUniforms(LatteDecompilerShader* shader)
{
	if (g_renderer->GetType() != RendererAPI::OpenGL)
		return;

	const GLuint program = static_cast<RendererShaderGL*>(shader->shader)->GetProgram();
	const char* stageSuffix = nullptr;
	switch (shader->shaderType)
	{
	case LatteConst::ShaderType::Vertex: stageSuffix = "VS"; break;
	case LatteConst::ShaderType::Geometry: stageSuffix = "GS"; break;
	case LatteConst::ShaderType::Pixel: stageSuffix = "PS"; break;
	default: break;
	}
	if (stageSuffix)
	{
		shader->uniform.loc_remapped = glGetUniformLocation(program, fmt::format("uf_remapped{}", stageSuffix).c_str());
		shader->uniform.loc_uniformRegister = glGetUniformLocation(program, fmt::format("uf_uniformRegister{}", stageSuffix).c_str());
	}
	catchOpenGLError();
	shader->uniform.loc_windowSpaceToClipSpaceTransform = glGetUniformLocation(program, "uf_windowSpaceToClipSpaceTransform");
	shader->uniform.loc_alphaTestRef = glGetUniformLocation(program, "uf_alphaTestRef");
	shader->uniform.loc_pointSize = glGetUniformLocation(program, "uf_pointSize");
	shader->uniform.loc_fragCoordScale = glGetUniformLocation(program, "uf_fragCoordScale");

	// per texture unit scale factors, only for units the shader actually declares
	cemu_assert_debug(shader->uniform.list_ufTexRescale.empty());
	for (sint32 unit = 0; unit < LATTE_NUM_MAX_TEX_UNITS; unit++)
	{
		const GLint location = glGetUniformLocation(program, fmt::format("uf_tex{}Scale", unit).c_str());
		if (location < 0)
			continue;
		LatteUniformTextureScaleEntry_t entry{};
		entry.texUnit = unit;
		entry.uniformLocation = location;
		shader->uniform.list_ufTexRescale.push_back(entry);
	}
}

static GLuint CompileRawStage(GLenum stage, const std::string& source)
{
	const GLuint shaderObj = glCreateShader(stage);
	const GLchar* text = source.c_str();
	const GLint length = (GLint)source.size();
	glShaderSource(shaderObj, 1, &text, &length);
	glCompileShader(shaderObj);
	GLint logLength = 0;
	glGetShaderiv(shaderObj, GL_INFO_LOG_LENGTH, &logLength);
	if (logLength > 0)
	{
		std::vector<char> log((size_t)logLength + 1, '\0');
		glGetShaderInfoLog(shaderObj, logLength, nullptr, log.data());
		cemuLog_log(LogType::Force, "OpenGL: compiler output for internal {} shader:", stage == GL_VERTEX_SHADER ? "vertex" : "fragment");
		cemuLog_log(LogType::Force, log.data());
	}
	return shaderObj;
}

GLuint OpenGL_LinkRawProgram(const std::string& vertexSource, const std::string& fragmentSource)
{
	const GLuint program = glCreateProgram();
	glAttachShader(program, CompileRawStage(GL_VERTEX_SHADER, vertexSource));
	glAttachShader(program, CompileRawStage(GL_FRAGMENT_SHADER, fragmentSource));
	glLinkProgram(program);
	GLint linked = GL_FALSE;
	glGetProgramiv(program, GL_LINK_STATUS, &linked);
	if (linked == GL_TRUE)
		return program;
	GLint logLength = 0;
	glGetProgramiv(program, GL_INFO_LOG_LENGTH, &logLength);
	std::vector<char> log((size_t)std::max(logLength, 0) + 1, '\0');
	if (logLength > 0)
		glGetProgramInfoLog(program, logLength, nullptr, log.data());
	cemuLog_log(LogType::Force, "OpenGL: failed to link internal program");
	cemuLog_log(LogType::Force, log.data());
	return 0;
}
