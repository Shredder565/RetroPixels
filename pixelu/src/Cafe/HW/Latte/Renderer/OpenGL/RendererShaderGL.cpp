// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cafe/HW/Latte/Renderer/OpenGL/RendererShaderGL.h"
#include "Cemu/FileCache/FileCache.h"
#include "config/ActiveSettings.h"
#include "config/LaunchSettings.h"

extern std::atomic_int g_compiled_shaders_total;
extern std::atomic_int g_compiled_shaders_async;

FileCache* RendererShaderGL::s_programBinaryCache{};

// true between ShaderCacheLoading_begin and _end: binaries may be taken from the cache and compilations are not counted
static bool s_glShaderCacheLoading = false;

// Cache entry layout: uint32 binary format followed by the program binary. The file name is
// GenerateShaderPrecompiledCacheFilename(), the file magic GeneratePrecompiledCacheId().

bool RendererShaderGL::TryLoadFromBinaryCache()
{
	if (!s_programBinaryCache || !glProgramBinary)
		return false;
	if (!m_isGameShader || m_isGfxPackShader)
		return false; // only unmodified game shaders are cached
	cemu_assert_debug(m_baseHash != 0);
	uint64 name1, name2;
	GenerateShaderPrecompiledCacheFilename(m_type, m_baseHash, m_auxHash, name1, name2);
	std::vector<uint8> entry;
	if (!s_programBinaryCache->GetFile({ name1, name2 }, entry) || entry.size() <= sizeof(uint32))
		return false;
	uint32 binaryFormat;
	memcpy(&binaryFormat, entry.data(), sizeof(uint32));
	m_program = glCreateProgram();
	glProgramBinary(m_program, binaryFormat, entry.data() + sizeof(uint32), (GLsizei)(entry.size() - sizeof(uint32)));
	GLint linked = -1;
	glGetProgramiv(m_program, GL_LINK_STATUS, &linked);
	if (linked != GL_TRUE)
	{
		// stale binary (driver update etc.), compile from source instead
		glDeleteProgram(m_program);
		m_program = 0;
		return false;
	}
	m_isCompiled = true;
	return true;
}

void RendererShaderGL::StoreInBinaryCache()
{
	if (!s_programBinaryCache || !glGetProgramBinary || m_program == 0)
		return;
	if (!m_isGameShader || m_isGfxPackShader)
		return;
	GLint binarySize = 0;
	glGetProgramiv(m_program, GL_PROGRAM_BINARY_LENGTH, &binarySize);
	if (binarySize <= 0)
		return;
	uint64 name1, name2;
	GenerateShaderPrecompiledCacheFilename(m_type, m_baseHash, m_auxHash, name1, name2);
	std::vector<uint8> entry(sizeof(uint32) + (size_t)binarySize, 0);
	GLenum binaryFormat = 0;
	glGetProgramBinary(m_program, binarySize, nullptr, &binaryFormat, entry.data() + sizeof(uint32));
	const uint32 formatU32 = (uint32)binaryFormat;
	memcpy(entry.data(), &formatU32, sizeof(uint32));
	s_programBinaryCache->AddFileAsync({ name1, name2 }, entry.data(), (sint32)entry.size());
}

static GLenum ToGLShaderStage(RendererShader::ShaderType type)
{
	switch (type)
	{
	case RendererShader::ShaderType::kVertex: return GL_VERTEX_SHADER;
	case RendererShader::ShaderType::kFragment: return GL_FRAGMENT_SHADER;
	case RendererShader::ShaderType::kGeometry: return GL_GEOMETRY_SHADER;
	}
	cemu_assert_debug(false);
	return GL_VERTEX_SHADER;
}

RendererShaderGL::RendererShaderGL(ShaderType type, uint64 baseHash, uint64 auxHash, bool isGameShader, bool isGfxPackShader, const std::string& glslSource)
	: RendererShader(type, baseHash, auxHash, isGameShader, isGfxPackShader), m_glslSource(glslSource)
{
	const GLenum stage = ToGLShaderStage(type);
	if (!(s_glShaderCacheLoading && TryLoadFromBinaryCache()))
	{
		m_shader_object = glCreateShader(stage);
		const GLchar* text = m_glslSource.c_str();
		const GLint length = (GLint)m_glslSource.size();
		glShaderSource(m_shader_object, 1, &text, &length);
		glCompileShader(m_shader_object);

		GLint logLength = 0;
		glGetShaderiv(m_shader_object, GL_INFO_LOG_LENGTH, &logLength);
		if (logLength > 0)
		{
			char log[2048]{};
			GLsizei written = 0;
			glGetShaderInfoLog(m_shader_object, std::min<GLint>(logLength, (GLint)sizeof(log) - 1), &written, log);
			cemuLog_log(LogType::Force, "OpenGL: shader compiler output:");
			cemuLog_log(LogType::Force, log);
		}
		if (LaunchSettings::NSightModeEnabled())
		{
			const std::string label = fmt::format("shader_{:016x}_{:016x}", m_baseHash, m_auxHash);
			glObjectLabel(GL_SHADER, m_shader_object, (GLsizei)label.size(), label.c_str());
		}

		m_program = glCreateProgram();
		glProgramParameteri(m_program, GL_PROGRAM_SEPARABLE, GL_TRUE);
		glProgramParameteri(m_program, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
		glAttachShader(m_program, m_shader_object);
		m_shader_attached = true;
		glLinkProgram(m_program);
		StoreInBinaryCache();
		if (!s_glShaderCacheLoading)
			++g_compiled_shaders_total;
	}
	// the source is not needed anymore once compiled
	m_glslSource.clear();
	m_glslSource.shrink_to_fit();
}

RendererShaderGL::~RendererShaderGL()
{
	CleanupShaderObj();
	if (m_program != 0)
		glDeleteProgram(m_program);
}

void RendererShaderGL::CleanupShaderObj()
{
	if (m_shader_object == 0)
		return;
	if (m_shader_attached)
		glDetachShader(m_program, m_shader_object);
	glDeleteShader(m_shader_object);
	m_shader_object = 0;
}

void RendererShaderGL::PreponeCompilation(bool isRenderThread)
{
	// compilation is started in the constructor, only the result has to be collected
	if (!m_isCompiled)
		WaitForCompiled();
}

bool RendererShaderGL::IsCompiled()
{
	cemu_assert_debug(false); // not used by the GL backend
	return true;
}

bool RendererShaderGL::ReportCompileFailure()
{
	m_isCompiled = true;
	CleanupShaderObj();
	return false;
}

bool RendererShaderGL::WaitForCompiled()
{
	if (m_isCompiled)
		return true;
	char log[8 * 1024];
	GLint ok = GL_FALSE;
	glGetShaderiv(m_shader_object, GL_COMPILE_STATUS, &ok);
	if (!ok)
	{
		GLint logLength = 0;
		glGetShaderiv(m_shader_object, GL_INFO_LOG_LENGTH, &logLength);
		if (logLength != 0)
		{
			GLsizei written = 0;
			glGetShaderInfoLog(m_shader_object, std::min<GLint>(logLength, (GLint)sizeof(log) - 1), &written, log);
			log[written] = '\0';
			cemuLog_log(LogType::Force, "OpenGL: shader failed to compile:");
			cemuLog_log(LogType::Force, log);
		}
		return ReportCompileFailure();
	}
	glGetProgramiv(m_program, GL_LINK_STATUS, &ok);
	if (!ok)
	{
		GLint logLength = 0;
		glGetProgramiv(m_program, GL_INFO_LOG_LENGTH, &logLength);
		if (logLength != 0)
		{
			GLsizei written = 0;
			glGetProgramInfoLog(m_program, std::min<GLint>(logLength, (GLint)sizeof(log) - 1), &written, log);
			log[written] = '\0';
			cemuLog_log(LogType::Force, "OpenGL: shader failed to link:");
			cemuLog_log(LogType::Force, log);
		}
		return ReportCompileFailure();
	}
	m_isCompiled = true;
	CleanupShaderObj();
	return true;
}

sint32 RendererShaderGL::GetUniformLocation(const char* name)
{
	return glGetUniformLocation(m_program, name);
}

void RendererShaderGL::SetUniform1i(sint32 location, sint32 value)
{
	glProgramUniform1i(m_program, location, value);
}

void RendererShaderGL::SetUniform1f(sint32 location, float value)
{
	glProgramUniform1f(m_program, location, value);
}

void RendererShaderGL::SetUniform2fv(sint32 location, void* data, sint32 count)
{
	glProgramUniform2fv(m_program, location, count, static_cast<const GLfloat*>(data));
}

void RendererShaderGL::SetUniform4iv(sint32 location, void* data, sint32 count)
{
	glProgramUniform4iv(m_program, location, count, static_cast<const GLint*>(data));
}

void RendererShaderGL::ShaderCacheLoading_begin(uint64 cacheTitleId)
{
	cemu_assert_debug(!s_programBinaryCache); // ShaderCacheLoading_Close() missing?
	bool useBinaries;
	switch (ActiveSettings::GetPrecompiledShadersOption())
	{
	case PrecompiledShaderOption::Auto:
		// program binaries tend to be slower than the driver's own cache on Nvidia
		useBinaries = g_renderer->GetVendor() != GfxVendor::Nvidia;
		break;
	case PrecompiledShaderOption::Enable:
		useBinaries = true;
		break;
	case PrecompiledShaderOption::Disable:
		useBinaries = false;
		break;
	default:
		UNREACHABLE;
	}
	cemuLog_log(LogType::Force, "Using precompiled shaders: {}", useBinaries ? "true" : "false");
	if (useBinaries)
	{
		const std::string fileName = fmt::format("{:016x}_gl.bin", cacheTitleId);
		s_programBinaryCache = FileCache::Open(ActiveSettings::GetCachePath("shaderCache/precompiled/{}", fileName), true, GeneratePrecompiledCacheId());
		if (!s_programBinaryCache)
			cemuLog_log(LogType::Force, "Unable to open OpenGL precompiled cache {}", fileName);
	}
	s_glShaderCacheLoading = true;
}

void RendererShaderGL::ShaderCacheLoading_end()
{
	s_glShaderCacheLoading = false;
}

void RendererShaderGL::ShaderCacheLoading_Close()
{
	delete s_programBinaryCache;
	s_programBinaryCache = nullptr;
	g_compiled_shaders_total = 0;
	g_compiled_shaders_async = 0;
}
