// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/HW/Latte/Renderer/OpenGL/OpenGLRenderer.h"
#include "WindowSystem.h"

#include "Cafe/HW/Latte/Core/LatteRingBuffer.h"
#include "Cafe/HW/Latte/Core/LatteDraw.h"
#include "Cafe/HW/Latte/Core/LatteOverlay.h"
#include "Cafe/HW/Latte/Renderer/OpenGL/LatteTextureGL.h"
#include "Cafe/HW/Latte/Renderer/OpenGL/LatteTextureViewGL.h"
#include "Cafe/HW/Latte/Renderer/OpenGL/RendererShaderGL.h"
#include "Cafe/HW/Latte/Renderer/OpenGL/CachedFBOGL.h"
#include "Cafe/HW/Latte/Renderer/OpenGL/OpenGLTextureReadback.h"
#include "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompiler.h"
#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "Cafe/OS/libs/gx2/GX2.h"
#include "config/ActiveSettings.h"
#include "config/LaunchSettings.h"

#include "imgui/imgui_impl_opengl3.h"
#include "imgui/imgui_extension.h"

// GL entry points, resolved at runtime by LoadOpenGLImports()
namespace CemuGL
{
#define GLFUNC(__type, __name) __type __name;
#define EGLFUNC(__type, __name) __type __name;
#include "Common/GLInclude/glFunctions.h"
#undef GLFUNC
#undef EGLFUNC
}

// ---- canvas callbacks ----

namespace
{
	OpenGLCanvasCallbacks s_noCanvas; // used while no window is attached
	OpenGLCanvasCallbacks* s_canvas = &s_noCanvas;
}

void SetOpenGLCanvasCallbacks(OpenGLCanvasCallbacks* callbacks)
{
	cemu_assert_debug(s_canvas == &s_noCanvas);
	s_canvas = callbacks;
}

void ClearOpenGLCanvasCallbacks()
{
	cemu_assert_debug(s_canvas != &s_noCanvas);
	s_canvas = &s_noCanvas;
}

bool GLCanvas_HasPadViewOpen()
{
	return s_canvas->HasPadViewOpen();
}

bool GLCanvas_MakeCurrent(bool padView)
{
	return s_canvas->MakeCurrent(padView);
}

void GLCanvas_SwapBuffers(bool swapTV, bool swapDRC)
{
	s_canvas->SwapBuffers(swapTV, swapDRC);
}

// ---- GL function loading ----

#define PU_GL_NAME_STR2(X) #X
#define PU_GL_NAME_STR(X) PU_GL_NAME_STR2(X)

#if BOOST_OS_WINDOWS
static void LoadOpenGLImports()
{
	HMODULE openglDll = LoadLibraryA("opengl32.dll");
	// wglGetProcAddress only resolves extension/core >1.1 functions, the 1.1 ones come from the DLL exports
	auto resolve = [openglDll](const char* name) -> void* {
		void* fn = (void*)wglGetProcAddress(name);
		if (fn == nullptr)
			fn = (void*)GetProcAddress(openglDll, name);
		return fn;
	};
#define GLFUNC(__type, __name) __name = (__type)resolve(PU_GL_NAME_STR(__name));
#include "Common/GLInclude/glFunctions.h"
#undef GLFUNC
}
#elif BOOST_OS_LINUX || BOOST_OS_BSD
#include <dlfcn.h>

static void LoadOpenGLImports()
{
	void* libGL = dlopen("libGL.so.1", RTLD_NOW | RTLD_GLOBAL);
	auto getProcAddress = (PFNGLXGETPROCADDRESSPROC)dlsym(libGL, "glXGetProcAddressARB");
	if (!getProcAddress)
	{
		libGL = dlopen("libGL.so", RTLD_NOW | RTLD_GLOBAL);
		getProcAddress = (PFNGLXGETPROCADDRESSPROC)dlsym(libGL, "glXGetProcAddressARB");
	}
	void* libEGL = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
	if (!libEGL)
		libEGL = dlopen("libEGL.so", RTLD_NOW | RTLD_GLOBAL);
#define GLFUNC(__type, __name) __name = (__type)getProcAddress((const GLubyte*)PU_GL_NAME_STR(__name));
#define EGLFUNC(__type, __name) __name = (__type)dlsym(libEGL, PU_GL_NAME_STR(__name));
#include "Common/GLInclude/glFunctions.h"
#undef GLFUNC
#undef EGLFUNC
}

// wxWidgets links against eglSwapInterval and would override our vsync setting, this symbol shadows it.
// The renderer itself calls the real function through CemuGL::eglSwapInterval.
extern "C" EGLAPI EGLBoolean EGLAPIENTRY eglSwapInterval(EGLDisplay dpy, EGLint interval)
{
	return EGL_TRUE;
}
#elif BOOST_OS_MACOS
static void LoadOpenGLImports()
{
	cemu_assert_unimplemented();
}
#endif

// ---- module state ----

namespace
{
	struct
	{
		std::vector<uint8> uploadStaging; // texture_acquireTextureUploadBuffer memory
		GLuint clearFBO = 0; // for clearing textures through glClear
	} s_gl;

	// GX2 compare functions are in GL order
	constexpr GLenum kCompareFunc[8] = { GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS };

	constexpr GLenum kStencilOp[8] = { GL_KEEP, GL_ZERO, GL_REPLACE, GL_INCR, GL_DECR, GL_INVERT, GL_INCR_WRAP, GL_DECR_WRAP };

	// GX2 blend factors 0x00-0x14; 0x0B/0x0C are unused, 0x0F-0x12 are dual source factors
	constexpr GLenum kInvalidBlend = 0xFFFFFFFF;
	constexpr GLenum kBlendFactor[0x15] = {
		GL_ZERO, GL_ONE, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
		GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA, GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR, GL_SRC_ALPHA_SATURATE,
		kInvalidBlend, kInvalidBlend, GL_CONSTANT_COLOR, GL_ONE_MINUS_CONSTANT_COLOR,
		GL_SRC1_COLOR, GL_ONE_MINUS_SRC1_COLOR, GL_SRC1_ALPHA, GL_ONE_MINUS_SRC1_ALPHA,
		GL_CONSTANT_ALPHA, GL_ONE_MINUS_CONSTANT_ALPHA
	};

	constexpr GLenum kBlendEquation[5] = { GL_FUNC_ADD, GL_FUNC_SUBTRACT, GL_MIN, GL_MAX, GL_FUNC_REVERSE_SUBTRACT };

	GLenum ToGLBlendFactor(Latte::LATTE_CB_BLENDN_CONTROL::E_BLENDFACTOR factor)
	{
		const uint32 index = (uint32)factor;
		if (index >= 0x0F && index <= 0x12)
		{
			cemuLog_logDebug(LogType::Force, "OpenGL: dual source blending is not supported");
			cemu_assert_debug(false);
			return GL_ZERO;
		}
		if (index >= std::size(kBlendFactor))
		{
			cemuLog_logDebug(LogType::Force, "OpenGL: blend factor 0x{:x} out of range", index);
			return GL_ZERO;
		}
		if (kBlendFactor[index] == kInvalidBlend)
		{
			cemuLog_logDebug(LogType::Force, "OpenGL: invalid blend factor 0x{:x}", index);
			cemu_assert_debug(false);
			return GL_ZERO;
		}
		return kBlendFactor[index];
	}

	GLenum ToGLBlendEquation(Latte::LATTE_CB_BLENDN_CONTROL::E_COMBINEFUNC func)
	{
		const uint32 index = (uint32)func;
		if (index >= std::size(kBlendEquation))
		{
			cemu_assert_suspicious();
			return GL_FUNC_ADD;
		}
		return kBlendEquation[index];
	}

	void APIENTRY OnGLDebugMessage(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar* message, const void* userParam)
	{
		// known harmless Nvidia notifications
		static const char* const kNvidiaIgnored[] = {
			"Buffer", "performance warning", "Dithering is enabled",
			"Blending is enabled, but is not supported for integer framebuffers",
			"does not have a defined base level", "has depth comparisons disabled, with a texture object",
		};
		if (LatteGPUState.glVendor == GLVENDOR_NVIDIA)
		{
			for (const char* pattern : kNvidiaIgnored)
			{
				if (strstr(message, pattern))
					return;
			}
		}
		cemuLog_log(LogType::Force, "GLDEBUG: {}", message);
		cemu_assert_debug(false);
	}

	void LoadSliceDirect(LatteTextureGL* tex, sint32 width, sint32 height, void* pixels, sint32 slice, sint32 mip, uint32 imageSize)
	{
		cemu_assert_debug(!tex->overwriteInfo.hasResolutionOverwrite);
		cemu_assert_debug(!tex->overwriteInfo.hasFormatOverwrite);
		LatteTextureGL::FormatInfoGL info;
		LatteTextureGL::GetOpenGLFormatInfo(tex->isDepth, tex->overwriteInfo.hasFormatOverwrite ? (Latte::E_GX2SURFFMT)tex->overwriteInfo.format : tex->format, tex->dim, &info);
		catchOpenGLError();
		if (mip >= tex->maxPossibleMipLevels)
		{
			cemuLog_logDebug(LogType::Force, "OpenGL: upload to mip {} which is not allocated", mip);
			return;
		}
		switch (tex->dim)
		{
		case Latte::E_DIM::DIM_2D:
		case Latte::E_DIM::DIM_2D_MSAA:
			if (info.glIsCompressed)
				glCompressedTextureSubImage2DWrapper(tex->glTexTarget, tex->glId_texture, mip, 0, 0, width, height, info.glInternalFormat, imageSize, pixels);
			else
				glTextureSubImage2DWrapper(tex->glTexTarget, tex->glId_texture, mip, 0, 0, width, height, info.glSuppliedFormat, info.glSuppliedFormatType, pixels);
			break;
		case Latte::E_DIM::DIM_1D:
			if (info.glIsCompressed)
				glCompressedTextureSubImage1DWrapper(tex->glTexTarget, tex->glId_texture, mip, 0, width, info.glInternalFormat, imageSize, pixels);
			else
				glTextureSubImage1DWrapper(tex->glTexTarget, tex->glId_texture, mip, 0, width, info.glSuppliedFormat, info.glSuppliedFormatType, pixels);
			break;
		case Latte::E_DIM::DIM_2D_ARRAY:
		case Latte::E_DIM::DIM_2D_ARRAY_MSAA:
		case Latte::E_DIM::DIM_3D:
		case Latte::E_DIM::DIM_CUBEMAP:
			// one layer (or depth slice) at a time
			if (info.glIsCompressed)
				glCompressedTextureSubImage3DWrapper(tex->glTexTarget, tex->glId_texture, mip, 0, 0, slice, width, height, 1, info.glInternalFormat, imageSize, pixels);
			else
				glTextureSubImage3DWrapper(tex->glTexTarget, tex->glId_texture, mip, 0, 0, slice, width, height, 1, info.glSuppliedFormat, info.glSuppliedFormatType, pixels);
			break;
		default:
			break;
		}
		catchOpenGLError();
	}
}

void LatteDraw_resetAttributePointerCache();
void decodeBC4Block_UNORM(uint8* blockStorage, float* rOutput);

// ---- lifetime ----

OpenGLRenderer::OpenGLRenderer() : Renderer(RendererAPI::OpenGL)
{
#if BOOST_OS_WINDOWS
	try
	{
		m_dxgi_wrapper = std::make_unique<DXGIWrapper>();
	}
	catch (const std::exception& ex)
	{
		cemuLog_log(LogType::Force, "Unable to create dxgi wrapper: {} (VRAM overlay stat won't be available)", ex.what());
	}
#endif
}

OpenGLRenderer::~OpenGLRenderer()
{
	if (m_pipeline != 0)
		glDeleteProgramPipelines(1, &m_pipeline);
	glDeleteBuffers(1, &m_backbufferBlit_uniformBuffer);
}

OpenGLRenderer* OpenGLRenderer::GetInstance()
{
	cemu_assert_debug(g_renderer && dynamic_cast<OpenGLRenderer*>(g_renderer.get()));
	return (OpenGLRenderer*)g_renderer.get();
}

void OpenGLRenderer::Initialize()
{
	Renderer::Initialize();
	auto logLock = cemuLog_acquire();
	cemuLog_log(LogType::Force, "------- Init OpenGL graphics backend -------");

	GLCanvas_MakeCurrent(false);
	LoadOpenGLImports();
	GetVendorInformation();

	// S3TC support decides whether BC1-3 are uploaded compressed or decoded on the CPU
	bool hasS3TC = false, hasTextureSRGB = false, hasS3TCSRGB = false;
	GLint extCount = 0;
	glGetIntegerv(GL_NUM_EXTENSIONS, &extCount);
	for (GLint i = 0; i < extCount; i++)
	{
		const std::string_view ext = (const char*)glGetStringi(GL_EXTENSIONS, i);
		if (ext == "GL_EXT_texture_compression_s3tc")
			hasS3TC = true;
		else if (ext == "GL_EXT_texture_sRGB")
			hasTextureSRGB = true;
		else if (ext == "GL_EXT_texture_compression_s3tc_srgb")
			hasS3TCSRGB = true;
	}
	m_supportsS3TC = hasS3TC;
	m_supportsS3TCSRGB = hasS3TCSRGB || (hasS3TC && hasTextureSRGB);

#if BOOST_OS_WINDOWS
	if (wglSwapIntervalEXT)
		wglSwapIntervalEXT(0); // vsync is applied by the canvas
#endif
	if (glMaxShaderCompilerThreadsARB)
		glMaxShaderCompilerThreadsARB(0xFFFFFFFF);

	auto availability = [](bool available) { return available ? "available" : "not supported"; };
	cemuLog_log(LogType::Force, "OpenGL extensions:");
	cemuLog_log(LogType::Force, "S3TC textures: {}", m_supportsS3TC ? "available" : "software decoding");
	cemuLog_log(LogType::Force, "S3TC sRGB textures: {}", m_supportsS3TCSRGB ? "available" : "software decoding");
	cemuLog_log(LogType::Force, "ARB_clip_control: {}", availability(glClipControl != nullptr));
	cemuLog_log(LogType::Force, "ARB_get_program_binary: {}", availability(glGetProgramBinary != nullptr && glProgramBinary != nullptr));
	cemuLog_log(LogType::Force, "ARB_clear_texture: {}", availability(glClearTexImage != nullptr));
	cemuLog_log(LogType::Force, "ARB_copy_image: {}", availability(glCopyImageSubData != nullptr));
	cemuLog_log(LogType::Force, "NV_depth_buffer_float: {}", availability(glDepthRangedNV != nullptr));

	glEnable(GL_FRAMEBUFFER_SRGB);
	if (m_vendor != GfxVendor::AMD)
	{
		// shaders write gl_PointSize; point sprites are off by default in the compatibility profile.
		// Not done on AMD where GL_PROGRAM_POINT_SIZE breaks program binaries.
		glEnable(GL_PROGRAM_POINT_SIZE);
		glEnable(GL_POINT_SPRITE);
	}
	// GX2 uses an upper left origin
	if (glClipControl)
		glClipControl(GL_UPPER_LEFT, GL_NEGATIVE_ONE_TO_ONE);
	else
		cemuLog_log(LogType::Force, "ARB_CLIP_CONTROL not supported by graphics driver. This will lead to crashes or graphical artifacts.");

	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	// Nvidia clamps SNORM render targets to [0,1] in the compatibility profile unless colour clamping is off
	if (m_vendor == GfxVendor::Nvidia)
		glClampColor(GL_CLAMP_FRAGMENT_COLOR, GL_FALSE);
	glEnable(GL_PRIMITIVE_RESTART);
	glPrimitiveRestartIndex(0xFFFFFFFF);

	glGenProgramPipelines(1, &m_pipeline);
	glBindProgramPipeline(m_pipeline);

	logLock.unlock();

	if (glCreateFramebuffers)
		glCreateFramebuffers(1, &s_gl.clearFBO);
	else
	{
		glGenFramebuffers(1, &s_gl.clearFBO);
		glBindFramebuffer(GL_FRAMEBUFFER, s_gl.clearFBO); // creates the object
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
	}

	glCreateBuffers(1, &m_backbufferBlit_uniformBuffer);
	glNamedBufferStorage(m_backbufferBlit_uniformBuffer, sizeof(RendererOutputShader::OutputUniformVariables), nullptr, GL_DYNAMIC_STORAGE_BIT);

	draw_init();
	catchOpenGLError();

	glGenBuffers(1, &glStreamoutCacheRingBuffer);
	glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, glStreamoutCacheRingBuffer);
	glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, LatteStreamout_GetRingBufferSize(), nullptr, GL_DYNAMIC_DRAW);
	glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, 0);
	catchOpenGLError();

	ImGui_ImplOpenGL3_Init("#version 130");
}

void OpenGLRenderer::GetVendorInformation()
{
	const char* vendor = (const char*)glGetString(GL_VENDOR);
	const char* rendererName = (const char*)glGetString(GL_RENDERER);
	const char* version = (const char*)glGetString(GL_VERSION);
	cemuLog_log(LogType::Force, "GL_VENDOR: {}", vendor ? vendor : "unknown");
	cemuLog_log(LogType::Force, "GL_RENDERER: {}", rendererName ? rendererName : "unknown");
	cemuLog_log(LogType::Force, "GL_VERSION: {}", version ? version : "unknown");

	m_vendor = GfxVendor::Generic;
	if (version && boost::icontains(version, "Mesa"))
	{
		m_vendor = GfxVendor::Mesa;
		return;
	}
	if (!vendor)
		return;
	auto startsWithNoCase = [vendor](const char* prefix) {
		for (size_t i = 0; prefix[i]; i++)
		{
			if (toupper((unsigned char)vendor[i]) != prefix[i])
				return false;
		}
		return true;
	};
	// "ATI Technologies Inc.", "AMD", "NVIDIA Corporation", "Intel"
	if (startsWithNoCase("ATI") || startsWithNoCase("AMD"))
		m_vendor = GfxVendor::AMD;
	else if (strncmp(vendor, "NVIDIA", 6) == 0)
		m_vendor = GfxVendor::Nvidia;
	else if (strncmp(vendor, "Intel", 5) == 0)
		m_vendor = GfxVendor::Intel;
}

bool OpenGLRenderer::IsPadWindowActive()
{
	return GLCanvas_HasPadViewOpen();
}

void OpenGLRenderer::Flush(bool waitIdle)
{
	glFlush();
	if (waitIdle)
		glFinish();
}

void OpenGLRenderer::NotifyLatteCommandProcessorIdle()
{
	glFlush();
}

void OpenGLRenderer::EnableDebugMode()
{
	glEnable(GL_DEBUG_OUTPUT);
	glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
	glDebugMessageCallback(OnGLDebugMessage, nullptr);
	glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE, 0, nullptr, true);
}

// ---- overlay ----

bool OpenGLRenderer::ImguiBegin(bool mainWindow)
{
	if (!mainWindow)
	{
		// the GamePad window has its own context, ImguiEnd switches back
		GLCanvas_MakeCurrent(true);
		m_isPadViewContext = true;
	}
	if (!Renderer::ImguiBegin(mainWindow))
		return false;
	renderstate_resetColorControl();
	renderstate_resetDepthControl();
	renderstate_resetStencilMask();
	// imgui draws with the GL origin
	if (glClipControl)
		glClipControl(GL_LOWER_LEFT, GL_NEGATIVE_ONE_TO_ONE);
	ImGui_ImplOpenGL3_NewFrame();
	ImGui_UpdateWindowInformation(mainWindow);
	ImGui::NewFrame();
	return true;
}

void OpenGLRenderer::ImguiEnd()
{
	ImGui::Render();
	ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
	if (m_isPadViewContext)
	{
		GLCanvas_MakeCurrent(false);
		m_isPadViewContext = false;
	}
	if (glClipControl)
		glClipControl(GL_UPPER_LEFT, GL_NEGATIVE_ONE_TO_ONE);
}

ImTextureID OpenGLRenderer::GenerateTexture(const std::vector<uint8>& data, const Vector2i& size)
{
	GLuint tex;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
	glActiveTexture(GL_TEXTURE0);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB, size.x, size.y, 0, GL_RGB, GL_UNSIGNED_BYTE, data.data());
	return (ImTextureID)(uintptr_t)tex;
}

void OpenGLRenderer::DeleteTexture(ImTextureID id)
{
	if (!id)
		return;
	GLuint tex = (GLuint)(uintptr_t)id;
	glDeleteTextures(1, &tex);
}

void OpenGLRenderer::DeleteFontTextures()
{
	ImGui_ImplOpenGL3_DestroyFontsTexture();
}

// ---- presentation ----

void OpenGLRenderer::SwapBuffers(bool swapTV, bool swapDRC)
{
	GLCanvas_SwapBuffers(swapTV, swapDRC);
	if (swapTV)
		cleanupAfterFrame();
}

bool OpenGLRenderer::BeginFrame(bool mainWindow)
{
	if (!mainWindow && !IsPadWindowActive())
		return false;
	GLCanvas_MakeCurrent(!mainWindow);
	ClearColorbuffer(!mainWindow);
	return true;
}

void OpenGLRenderer::DrawEmptyFrame(bool mainWindow)
{
	if (BeginFrame(mainWindow))
		SwapBuffers(mainWindow, !mainWindow);
}

void OpenGLRenderer::ClearColorbuffer(bool padView)
{
	glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	glClear(GL_COLOR_BUFFER_BIT);
}

void OpenGLRenderer::HandleScreenshotRequest(LatteTextureView* texView, bool padView)
{
	if (!m_screenshot_requested && m_screenshot_state == ScreenshotState::None)
		return;

	if (IsPadWindowActive())
	{
		// with both windows open a request captures both pictures, one per call; skip the one already taken
		if ((m_screenshot_state == ScreenshotState::Main && padView) || (m_screenshot_state == ScreenshotState::Pad && !padView))
			return;
		if (m_screenshot_state == ScreenshotState::None)
			m_screenshot_state = padView ? ScreenshotState::Main : ScreenshotState::Pad; // the one still missing
		else
			m_screenshot_state = ScreenshotState::None;
	}
	else
		m_screenshot_state = ScreenshotState::None;

	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	texture_bindAndActivate(texView, 0);
	GLint width = 0, height = 0;
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
	std::vector<uint8> rgb((size_t)width * height * 3);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glGetTexImage(GL_TEXTURE_2D, 0, GL_RGB, GL_UNSIGNED_BYTE, rgb.data());
	glPixelStorei(GL_PACK_ALIGNMENT, 4);
	texture_bindAndActivate(nullptr, 0);

	// match what the output shader shows: convert when the view and the scan buffer disagree about sRGB
	const bool viewIsSRGB = HAS_FLAG(texView->format, Latte::E_GX2SURFFMT::FMT_BIT_SRGB);
	const bool scanBufferIsSRGB = padView ? LatteGPUState.drcBufferUsesSRGB : LatteGPUState.tvBufferUsesSRGB;
	if (viewIsSRGB != scanBufferIsSRGB)
	{
		uint8 (*convert)(uint8) = viewIsSRGB ? &Renderer::SRGBComponentToRGB : &Renderer::RGBComponentToSRGB;
		for (uint8& c : rgb)
			c = convert(c);
	}
	SaveScreenshot(rgb, width, height, !padView);
}

void OpenGLRenderer::DrawBackbufferQuad(LatteTextureView* texView, RendererOutputShader* shader, bool useLinearTexFilter, sint32 imageX, sint32 imageY, sint32 imageWidth, sint32 imageHeight, bool padView, bool clearBackground)
{
	if (padView && !IsPadWindowActive())
		return;
	catchOpenGLError();
	GLCanvas_MakeCurrent(padView);

	renderstate_resetColorControl();
	renderstate_resetDepthControl();
	attributeStream_reset();
	rendertarget_bindFramebufferObject(nullptr); // window framebuffer

	if (clearBackground)
	{
		int windowWidth, windowHeight;
		if (padView)
			WindowSystem::GetPadWindowPhysSize(windowWidth, windowHeight);
		else
			WindowSystem::GetWindowPhysSize(windowWidth, windowHeight);
		g_renderer->renderTarget_setViewport(0, 0, windowWidth, windowHeight, 0.0f, 1.0f);
		g_renderer->ClearColorbuffer(padView);
	}

	shader_unbind(RendererShader::ShaderType::kGeometry);
	shader_bind(shader->GetVertexShader());
	shader_bind(shader->GetFragmentShader());

	const auto uniforms = shader->FillUniformBlockBuffer(*texView, { imageWidth, imageHeight }, padView);
	glNamedBufferSubData(m_backbufferBlit_uniformBuffer, 0, sizeof(uniforms), &uniforms);
	glBindBufferBase(GL_UNIFORM_BUFFER, 0, m_backbufferBlit_uniformBuffer);

	glViewportIndexedf(0, imageX, imageY, imageWidth, imageHeight);

	// set filtering directly and invalidate the cached sampler state of the view
	auto* viewGL = static_cast<LatteTextureViewGL*>(texView);
	texture_bindAndActivate(texView, 0);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	viewGL->samplerState.clampS = 0xFF;
	viewGL->samplerState.clampT = 0xFF;
	const GLint filter = useLinearTexFilter ? GL_LINEAR : GL_NEAREST;
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
	viewGL->samplerState.filterMin = 0xFFFFFFFF;
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
	viewGL->samplerState.filterMag = 0xFFFFFFFF;

	// the output shader does its own sRGB encoding
	glDisable(GL_FRAMEBUFFER_SRGB);
	const uint16 quadIndices[6] = { 0, 1, 2, 3, 4, 5 };
	glDrawRangeElements(GL_TRIANGLES, 0, 5, 6, GL_UNSIGNED_SHORT, quadIndices);
	glEnable(GL_FRAMEBUFFER_SRGB);

	texture_bindAndActivate(nullptr, 0);
	catchOpenGLError();

	// restore the game's viewport
	glViewportIndexedf(0, m_viewState.x, m_viewState.y, m_viewState.width, m_viewState.height);
	if (padView)
		GLCanvas_MakeCurrent(false);
}

// ---- render targets ----

void OpenGLRenderer::renderTarget_setViewport(float x, float y, float width, float height, float nearZ, float farZ, bool halfZ)
{
	if (m_viewState.nearZ != nearZ || m_viewState.farZ != farZ || m_viewState.halfZ != halfZ)
	{
		// a far plane of 0.99999994 (largest float below 1) is treated as 1.0
		uint32 farBits;
		memcpy(&farBits, &farZ, sizeof(farBits));
		if (farBits == 0x3f7fffff)
			farZ = 1.0f;
		if (glDepthRangedNV)
			glDepthRangedNV(nearZ, farZ);
		else
			glDepthRange(nearZ, farZ);
		m_viewState.nearZ = nearZ;
		m_viewState.farZ = farZ;
	}

	// a negative height flips the picture, done with the clip origin
	bool invertY = false;
	if (height < 0.0)
	{
		invertY = true;
		y += height;
		height = -height;
	}
	if (glClipControl && (m_viewState.invertY != invertY || m_viewState.halfZ != halfZ))
	{
		glClipControl(invertY ? GL_LOWER_LEFT : GL_UPPER_LEFT, halfZ ? GL_ZERO_TO_ONE : GL_NEGATIVE_ONE_TO_ONE);
		m_viewState.invertY = invertY;
		m_viewState.halfZ = halfZ;
	}

	if (m_viewState.x == x && m_viewState.y == y && m_viewState.width == width && m_viewState.height == height)
		return;
	glViewportIndexedf(0, x, y, width, height);
	m_viewState.x = x;
	m_viewState.y = y;
	m_viewState.width = width;
	m_viewState.height = height;
}

void OpenGLRenderer::renderTarget_setScissor(sint32 scissorX, sint32 scissorY, sint32 scissorWidth, sint32 scissorHeight)
{
	if (!m_viewState.scissorEnabled)
	{
		glEnable(GL_SCISSOR_TEST);
		m_viewState.scissorEnabled = true;
	}
	glScissor(scissorX, scissorY, scissorWidth, scissorHeight);
}

LatteCachedFBO* OpenGLRenderer::rendertarget_createCachedFBO(uint64 key)
{
	return new CachedFBOGL(key);
}

void OpenGLRenderer::rendertarget_deleteCachedFBO(LatteCachedFBO* cfbo)
{
	auto* fbo = static_cast<CachedFBOGL*>(cfbo);
	if (prevBoundFBO == fbo->glId_fbo)
	{
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		prevBoundFBO = 0;
	}
	glDeleteFramebuffers(1, &fbo->glId_fbo);
}

void OpenGLRenderer::rendertarget_bindFramebufferObject(LatteCachedFBO* cfbo)
{
	const GLuint name = cfbo ? static_cast<CachedFBOGL*>(cfbo)->glId_fbo : 0;
	if (prevBoundFBO == name)
		return;
	glBindFramebuffer(GL_FRAMEBUFFER, name);
	prevBoundFBO = name;
}

// ---- fixed function state ----

void OpenGLRenderer::renderstate_setChannelTargetMask(uint32 renderTargetMask)
{
	const uint32 changed = renderTargetMask ^ m_blendState.colorWriteMask;
	if (changed == 0)
		return;
	for (GLuint target = 0; target < 8; target++)
	{
		if (((changed >> (target * 4)) & 0xF) == 0)
			continue;
		const uint32 rgba = (renderTargetMask >> (target * 4)) & 0xF;
		glColorMaski(target, (rgba & 1) ? GL_TRUE : GL_FALSE, (rgba & 2) ? GL_TRUE : GL_FALSE, (rgba & 4) ? GL_TRUE : GL_FALSE, (rgba & 8) ? GL_TRUE : GL_FALSE);
	}
	m_blendState.colorWriteMask = renderTargetMask;
}

void OpenGLRenderer::renderstate_setAlwaysWriteDepth()
{
	if (!m_depthStencilState.depthTest)
	{
		glEnable(GL_DEPTH_TEST);
		m_depthStencilState.depthTest = true;
	}
	glDepthFunc(GL_ALWAYS);
	m_depthStencilState.depthFunc = Latte::LATTE_DB_DEPTH_CONTROL::E_ZFUNC::ALWAYS;
}

void OpenGLRenderer::SetBlendEnableMask(uint32 mask)
{
	const uint32 changed = mask ^ m_blendState.enableMask;
	for (GLuint target = 0; target < 8; target++)
	{
		if ((changed & (1u << target)) == 0)
			continue;
		if (mask & (1u << target))
			glEnablei(GL_BLEND, target);
		else
			glDisablei(GL_BLEND, target);
	}
	m_blendState.enableMask = mask;
}

void OpenGLRenderer::ApplyStencilState()
{
	const auto& depthControl = LatteGPUState.contextNew.DB_DEPTH_CONTROL;
	const bool stencilEnable = depthControl.get_STENCIL_ENABLE();
	if (m_depthStencilState.stencilTest != stencilEnable)
	{
		if (stencilEnable)
			glEnable(GL_STENCIL_TEST);
		else
			glDisable(GL_STENCIL_TEST);
		m_depthStencilState.stencilTest = stencilEnable;
	}
	// the parameters are left alone while stencil testing is off
	if (!stencilEnable)
		return;

	using FaceState = decltype(m_depthStencilState.front);
	FaceState front;
	front.func = depthControl.get_STENCIL_FUNC_F();
	front.opFail = depthControl.get_STENCIL_FAIL_F();
	front.opZFail = depthControl.get_STENCIL_ZFAIL_F();
	front.opZPass = depthControl.get_STENCIL_ZPASS_F();
	front.compareMask = LatteGPUState.contextNew.DB_STENCILREFMASK.get_STENCILMASK_F();
	front.writeMask = LatteGPUState.contextNew.DB_STENCILREFMASK.get_STENCILWRITEMASK_F();
	front.ref = LatteGPUState.contextNew.DB_STENCILREFMASK.get_STENCILREF_F();
	FaceState back = front; // without separate back face stencil both faces use the front settings
	if (depthControl.get_BACK_STENCIL_ENABLE())
	{
		back.func = depthControl.get_STENCIL_FUNC_B();
		back.opFail = depthControl.get_STENCIL_FAIL_B();
		back.opZFail = depthControl.get_STENCIL_ZFAIL_B();
		back.opZPass = depthControl.get_STENCIL_ZPASS_B();
		back.compareMask = LatteGPUState.contextNew.DB_STENCILREFMASK_BF.get_STENCILMASK_B();
		back.writeMask = LatteGPUState.contextNew.DB_STENCILREFMASK_BF.get_STENCILWRITEMASK_B();
		back.ref = LatteGPUState.contextNew.DB_STENCILREFMASK_BF.get_STENCILREF_B();
	}

	auto applyFace = [](GLenum glFace, const FaceState& want, FaceState& have) {
		if (have.opFail != want.opFail || have.opZFail != want.opZFail || have.opZPass != want.opZPass)
		{
			glStencilOpSeparate(glFace, kStencilOp[(size_t)want.opFail], kStencilOp[(size_t)want.opZFail], kStencilOp[(size_t)want.opZPass]);
			have.opFail = want.opFail;
			have.opZFail = want.opZFail;
			have.opZPass = want.opZPass;
		}
		if (have.func != want.func || have.ref != want.ref || have.compareMask != want.compareMask)
		{
			glStencilFuncSeparate(glFace, kCompareFunc[(size_t)want.func], want.ref, want.compareMask);
			have.func = want.func;
			have.ref = want.ref;
			have.compareMask = want.compareMask;
		}
		if (have.writeMask != want.writeMask)
		{
			glStencilMaskSeparate(glFace, want.writeMask);
			have.writeMask = want.writeMask;
		}
	};
	applyFace(GL_FRONT, front, m_depthStencilState.front);
	applyFace(GL_BACK, back, m_depthStencilState.back);
}

void OpenGLRenderer::ApplyBlendState()
{
	const auto& colorControl = LatteGPUState.contextNew.CB_COLOR_CONTROL;
	SetBlendEnableMask(colorControl.get_BLEND_MASK());
	catchOpenGLError();

	const uint32* blendConstant = LatteGPUState.contextRegister + Latte::REGADDR::CB_BLEND_RED;
	if (memcmp(blendConstant, m_blendState.constantColor, sizeof(m_blendState.constantColor)) != 0)
	{
		float rgba[4];
		memcpy(rgba, blendConstant, sizeof(rgba));
		glBlendColor(rgba[0], rgba[1], rgba[2], rgba[3]);
		memcpy(m_blendState.constantColor, blendConstant, sizeof(m_blendState.constantColor));
	}

	for (GLuint target = 0; target < 8; target++)
	{
		const auto& reg = LatteGPUState.contextNew.CB_BLENDN_CONTROL[target];
		if (reg.getRawValue() == m_blendState.controlReg[target])
			continue;
		const GLenum colorSrc = ToGLBlendFactor(reg.get_COLOR_SRCBLEND());
		const GLenum colorDst = ToGLBlendFactor(reg.get_COLOR_DSTBLEND());
		if (reg.get_SEPARATE_ALPHA_BLEND())
		{
			const GLenum alphaSrc = ToGLBlendFactor(reg.get_ALPHA_SRCBLEND());
			const GLenum alphaDst = ToGLBlendFactor(reg.get_ALPHA_DSTBLEND());
			glBlendFuncSeparatei(target, colorSrc, colorDst, alphaSrc, alphaDst);
			glBlendEquationSeparatei(target, ToGLBlendEquation(reg.get_COLOR_COMB_FCN()), ToGLBlendEquation(reg.get_ALPHA_COMB_FCN()));
		}
		else
		{
			glBlendFuncSeparatei(target, colorSrc, colorDst, colorSrc, colorDst);
			const GLenum equation = ToGLBlendEquation(reg.get_COLOR_COMB_FCN());
			glBlendEquationSeparatei(target, equation, equation);
		}
		m_blendState.controlReg[target] = reg.getRawValue();
	}

	// only the logic ops GX2 games use are mapped
	using LogicOp = Latte::LATTE_CB_COLOR_CONTROL::E_LOGICOP;
	GLenum logicOp = GL_COPY;
	switch (colorControl.get_ROP())
	{
	case LogicOp::COPY: logicOp = GL_COPY; break;
	case LogicOp::SET: logicOp = GL_SET; break;
	case LogicOp::CLEAR: logicOp = GL_CLEAR; break;
	case LogicOp::OR: logicOp = GL_OR; break;
	default: cemu_assert_unimplemented(); break;
	}
	if (m_blendState.logicOp != logicOp)
	{
		if (logicOp != GL_COPY)
			glEnable(GL_COLOR_LOGIC_OP);
		else
			glDisable(GL_COLOR_LOGIC_OP);
		glLogicOp(logicOp);
		m_blendState.logicOp = logicOp;
	}
}

void OpenGLRenderer::ApplyRasterizerState()
{
	const auto& ctx = LatteGPUState.contextNew;
	const auto& modeControl = ctx.PA_SU_SC_MODE_CNTL;
	const auto frontFace = modeControl.get_FRONT_FACE();
	const uint32 cullFront = modeControl.get_CULL_FRONT();
	const uint32 cullBack = modeControl.get_CULL_BACK();
	const uint32 offsetFrontEnabled = modeControl.get_OFFSET_FRONT_ENABLED();
	auto& rs = m_rasterState;

	const uint32 cullEnable = (cullFront || cullBack) ? 1 : 0;
	if (rs.cullEnable != cullEnable)
	{
		if (cullEnable)
			glEnable(GL_CULL_FACE);
		else
			glDisable(GL_CULL_FACE);
		rs.cullEnable = cullEnable;
	}
	if (rs.frontFace != frontFace)
	{
		if (frontFace == Latte::LATTE_PA_SU_SC_MODE_CNTL::E_FRONTFACE::CCW)
			glFrontFace(GL_CCW);
		else if (frontFace == Latte::LATTE_PA_SU_SC_MODE_CNTL::E_FRONTFACE::CW)
			glFrontFace(GL_CW);
		else
			cemu_assert_unimplemented();
		rs.frontFace = frontFace;
	}
	if (rs.cullFront != cullFront || rs.cullBack != cullBack)
	{
		// with neither bit set culling is disabled above and the face selection does not matter
		if (cullFront && cullBack)
			glCullFace(GL_FRONT_AND_BACK);
		else if (cullBack)
			glCullFace(GL_BACK);
		else if (cullFront)
			glCullFace(GL_FRONT);
		rs.cullFront = cullFront;
		rs.cullBack = cullBack;
	}

	// polygon offset (only the front face settings are used)
	if (rs.offsetFillEnable != offsetFrontEnabled)
	{
		if (offsetFrontEnabled)
			glEnable(GL_POLYGON_OFFSET_FILL);
		else
			glDisable(GL_POLYGON_OFFSET_FILL);
		rs.offsetFillEnable = offsetFrontEnabled;
	}
	if (offsetFrontEnabled)
	{
		const uint32 offsetReg = ctx.PA_SU_POLY_OFFSET_FRONT_OFFSET.getRawValue();
		const uint32 scaleReg = ctx.PA_SU_POLY_OFFSET_FRONT_SCALE.getRawValue();
		const uint32 clampReg = ctx.PA_SU_POLY_OFFSET_CLAMP.getRawValue();
		if (offsetReg != rs.offsetOffsetReg || scaleReg != rs.offsetScaleReg || clampReg != rs.offsetClampReg)
		{
			// the GPU7 slope scale is in 1/16 units
			const float slopeScale = ctx.PA_SU_POLY_OFFSET_FRONT_SCALE.get_SCALE() / 16.0f;
			glPolygonOffset(slopeScale, ctx.PA_SU_POLY_OFFSET_FRONT_OFFSET.get_OFFSET());
			rs.offsetOffsetReg = offsetReg;
			rs.offsetScaleReg = scaleReg;
			rs.offsetClampReg = clampReg;
		}
	}

	// near/far clipping off is emulated with depth clamping
	catchOpenGLError();
	cemu_assert_debug(ctx.PA_CL_CLIP_CNTL.get_ZCLIP_NEAR_DISABLE() == ctx.PA_CL_CLIP_CNTL.get_ZCLIP_FAR_DISABLE());
	const uint32 zClipEnable = ctx.PA_CL_CLIP_CNTL.get_ZCLIP_NEAR_DISABLE() == false;
	if (rs.zClipEnable != zClipEnable)
	{
		if (zClipEnable)
			glDisable(GL_DEPTH_CLAMP);
		else
			glEnable(GL_DEPTH_CLAMP);
		rs.zClipEnable = zClipEnable;
	}
	catchOpenGLError();

	// point size in 1/8 pixel units, 0 means the minimum size
	const uint32 pointSizeReg = ctx.PA_SU_POINT_SIZE.getRawValue();
	if (rs.pointSizeReg != pointSizeReg)
	{
		const float pointWidth = (float)ctx.PA_SU_POINT_SIZE.get_WIDTH() / 8.0f;
		glPointSize(pointWidth == 0.0f ? (1.0f / 8.0f) : pointWidth);
		rs.pointSizeReg = pointSizeReg;
		catchOpenGLError();
	}

	const uint32 restartIndex = ctx.VGT_MULTI_PRIM_IB_RESET_INDX.get_RESTART_INDEX();
	if (rs.restartIndex != restartIndex)
	{
		glPrimitiveRestartIndex(restartIndex);
		rs.restartIndex = restartIndex;
	}
}

void OpenGLRenderer::renderstate_updateBlendingAndColorControl()
{
	catchOpenGLError();
	const auto& ctx = LatteGPUState.contextNew;
	cemu_assert_debug(!ctx.CB_COLOR_CONTROL.get_MULTIWRITE_ENABLE()); // not emulated

	// SPECIAL_OP disable turns off all colour writes
	uint32 writeMask = ctx.CB_TARGET_MASK.get_MASK();
	if (ctx.CB_COLOR_CONTROL.get_SPECIAL_OP() == Latte::LATTE_CB_COLOR_CONTROL::E_SPECIALOP::DISABLE)
		writeMask = 0;
	renderstate_setChannelTargetMask(writeMask);
	catchOpenGLError();

	ApplyStencilState();

	const bool depthEnable = ctx.DB_DEPTH_CONTROL.get_Z_ENABLE();
	const bool depthWrite = ctx.DB_DEPTH_CONTROL.get_Z_WRITE_ENABLE();
	const auto depthFunc = ctx.DB_DEPTH_CONTROL.get_Z_FUNC();
	if (m_depthStencilState.depthTest != depthEnable)
	{
		if (depthEnable)
			glEnable(GL_DEPTH_TEST);
		else
			glDisable(GL_DEPTH_TEST);
		m_depthStencilState.depthTest = depthEnable;
	}
	if (m_depthStencilState.depthWrite != depthWrite)
	{
		glDepthMask(depthWrite ? GL_TRUE : GL_FALSE);
		m_depthStencilState.depthWrite = depthWrite;
	}
	if (m_depthStencilState.depthFunc != depthFunc)
	{
		glDepthFunc(kCompareFunc[(size_t)depthFunc]);
		m_depthStencilState.depthFunc = depthFunc;
	}
	catchOpenGLError();

	ApplyBlendState();
	ApplyRasterizerState();
}

void OpenGLRenderer::renderstate_resetColorControl()
{
	renderstate_setChannelTargetMask(0xF);
	SetBlendEnableMask(0);
	// blend functions are re-applied on the next draw
	for (uint32& reg : m_blendState.controlReg)
		reg = 0xFFFFFFFF;
	if (m_blendState.alphaTest)
	{
		glDisable(GL_ALPHA_TEST);
		m_blendState.alphaTest = false;
	}
	if (m_blendState.logicOp != GL_COPY)
	{
		glDisable(GL_COLOR_LOGIC_OP);
		glLogicOp(GL_COPY);
		m_blendState.logicOp = GL_COPY;
	}
	if (m_rasterState.cullEnable != 0)
	{
		glDisable(GL_CULL_FACE);
		m_rasterState.cullEnable = 0;
	}
	if (m_rasterState.offsetFillEnable != 0)
	{
		glDisable(GL_POLYGON_OFFSET_FILL);
		m_rasterState.offsetFillEnable = 0;
	}
	if (m_viewState.scissorEnabled)
	{
		glDisable(GL_SCISSOR_TEST);
		m_viewState.scissorEnabled = false;
	}
}

void OpenGLRenderer::renderstate_resetDepthControl()
{
	if (m_depthStencilState.depthTest)
	{
		glDisable(GL_DEPTH_TEST);
		m_depthStencilState.depthTest = false;
	}
	if (!m_depthStencilState.depthWrite)
	{
		glDepthMask(GL_TRUE);
		m_depthStencilState.depthWrite = true;
	}
	if (m_depthStencilState.stencilTest)
	{
		glDisable(GL_STENCIL_TEST);
		m_depthStencilState.stencilTest = false;
	}
	// depth clamp is always switched off here, the cached state is updated to match
	glDisable(GL_DEPTH_CLAMP);
	m_rasterState.zClipEnable = 1;
	if (m_rasterState.restartIndex != 0xFFFFFFFF)
	{
		glPrimitiveRestartIndex(0xFFFFFFFF);
		m_rasterState.restartIndex = 0xFFFFFFFF;
	}
}

void OpenGLRenderer::renderstate_resetStencilMask()
{
	if (m_depthStencilState.front.writeMask != 0xFFFFFFFF)
	{
		glStencilMaskSeparate(GL_FRONT, 0xFFFFFFFF);
		m_depthStencilState.front.writeMask = 0xFFFFFFFF;
	}
	if (m_depthStencilState.back.writeMask != 0xFFFFFFFF)
	{
		glStencilMaskSeparate(GL_BACK, 0xFFFFFFFF);
		m_depthStencilState.back.writeMask = 0xFFFFFFFF;
	}
}

// ---- textures ----

void* OpenGLRenderer::texture_acquireTextureUploadBuffer(uint32 size)
{
	// uploads go through glTextureSubImage from client memory
	if (s_gl.uploadStaging.size() < size)
		s_gl.uploadStaging.resize(size);
	return s_gl.uploadStaging.data();
}

void OpenGLRenderer::texture_releaseTextureUploadBuffer(uint8* mem)
{
}

TextureDecoder* OpenGLRenderer::texture_chooseDecodedFormat(Latte::E_GX2SURFFMT format, bool isDepth, Latte::E_DIM dim, uint32 width, uint32 height)
{
	using F = Latte::E_GX2SURFFMT;
	if (isDepth)
	{
		switch (format)
		{
		case F::R32_FLOAT: return TextureDecoder_R32_FLOAT::getInstance();
		case F::D24_S8_UNORM: return TextureDecoder_D24_S8::getInstance();
		case F::D24_S8_FLOAT: return TextureDecoder_NullData64::getInstance();
		case F::D32_S8_FLOAT: return TextureDecoder_D32_S8_UINT_X24::getInstance();
		case F::R16_UNORM: return TextureDecoder_R16_FLOAT::getInstance();
		default: return nullptr;
		}
	}
	// without S3TC the BC1-3 blocks are decompressed on the CPU
	if (!SupportsS3TC(HAS_FLAG(format, Latte::E_GX2SURFFMT::FMT_BIT_SRGB)))
	{
		switch (format)
		{
		case F::BC1_UNORM: return TextureDecoder_BC1_UNORM_uncompress::getInstance();
		case F::BC1_SRGB: return TextureDecoder_BC1_SRGB_uncompress::getInstance();
		case F::BC2_UNORM: return TextureDecoder_BC2_UNORM_uncompress::getInstance();
		case F::BC2_SRGB: return TextureDecoder_BC2_SRGB_uncompress::getInstance();
		case F::BC3_UNORM: return TextureDecoder_BC3_UNORM_uncompress::getInstance();
		case F::BC3_SRGB: return TextureDecoder_BC3_SRGB_uncompress::getInstance();
		default: break;
		}
	}
	const bool rgtcAllowed = dim == Latte::E_DIM::DIM_2D || dim == Latte::E_DIM::DIM_2D_ARRAY;
	switch (format)
	{
	case F::R4_G4_UNORM: return TextureDecoder_R4_G4_UNORM_To_RGBA4::getInstance();
	case F::R4_G4_B4_A4_UNORM: return TextureDecoder_R4_G4_B4_A4_UNORM::getInstance();
	case F::R16_G16_B16_A16_FLOAT: return TextureDecoder_R16_G16_B16_A16_FLOAT::getInstance();
	case F::R16_G16_FLOAT: return TextureDecoder_R16_G16_FLOAT::getInstance();
	case F::R16_SNORM: return TextureDecoder_R16_SNORM::getInstance();
	case F::R16_FLOAT: return TextureDecoder_R16_FLOAT::getInstance();
	case F::R32_FLOAT: return TextureDecoder_R32_FLOAT::getInstance();
	case F::BC1_UNORM:
	case F::BC1_SRGB: return TextureDecoder_BC1::getInstance();
	case F::BC2_UNORM:
	case F::BC2_SRGB: return TextureDecoder_BC2::getInstance();
	case F::BC3_UNORM:
	case F::BC3_SRGB: return TextureDecoder_BC3::getInstance();
	case F::BC4_UNORM:
	case F::BC4_SNORM:
		// both signednesses are decoded as UNORM when RGTC cannot be used
		return rgtcAllowed ? (TextureDecoder*)TextureDecoder_BC4::getInstance() : (TextureDecoder*)TextureDecoder_BC4_UNORM_uncompress::getInstance();
	case F::BC5_UNORM:
	case F::BC5_SNORM: return TextureDecoder_BC5::getInstance();
	case F::R8_G8_B8_A8_UNORM:
	case F::R8_G8_B8_A8_SNORM:
	case F::R8_G8_B8_A8_SRGB: return TextureDecoder_R8_G8_B8_A8::getInstance();
	case F::R8_UNORM:
	case F::R8_SNORM: return TextureDecoder_R8::getInstance();
	case F::R8_G8_UNORM:
	case F::R8_G8_SNORM: return TextureDecoder_R8_G8::getInstance();
	case F::R16_UNORM: return TextureDecoder_R16_UNORM::getInstance();
	case F::R16_G16_B16_A16_UNORM:
	case F::R16_G16_B16_A16_SNORM: return TextureDecoder_R16_G16_B16_A16::getInstance();
	case F::R16_G16_UNORM: return TextureDecoder_R16_G16::getInstance();
	case F::R5_G6_B5_UNORM: return TextureDecoder_R5_G6_B5::getInstance();
	case F::R5_G5_B5_A1_UNORM: return TextureDecoder_R5_G5_B5_A1_UNORM_swappedOpenGL::getInstance();
	case F::A1_B5_G5_R5_UNORM: return TextureDecoder_A1_B5_G5_R5_UNORM::getInstance();
	case F::R32_G32_FLOAT: return TextureDecoder_R32_G32_FLOAT::getInstance();
	case F::R32_G32_UINT: return TextureDecoder_R32_G32_UINT::getInstance();
	case F::R32_UINT: return TextureDecoder_R32_UINT::getInstance();
	case F::R16_UINT: return TextureDecoder_R16_UINT::getInstance();
	case F::R8_UINT: return TextureDecoder_R8_UINT::getInstance();
	case F::R32_G32_B32_A32_FLOAT: return TextureDecoder_R32_G32_B32_A32_FLOAT::getInstance();
	case F::R10_G10_B10_A2_UNORM:
	case F::R10_G10_B10_A2_SRGB: return TextureDecoder_R10_G10_B10_A2_UNORM::getInstance();
	case F::A2_B10_G10_R10_UNORM: return TextureDecoder_A2_B10_G10_R10_UNORM_To_RGBA16::getInstance();
	case F::R10_G10_B10_A2_SNORM: return TextureDecoder_R10_G10_B10_A2_SNORM_To_RGBA16::getInstance();
	case F::R11_G11_B10_FLOAT: return TextureDecoder_R11_G11_B10_FLOAT::getInstance();
	case F::R32_G32_B32_A32_UINT:
	case F::R32_G32_B32_A32_SINT: return TextureDecoder_R32_G32_B32_A32_UINT::getInstance(); // same bits for both
	case F::R16_G16_B16_A16_UINT: return TextureDecoder_R16_G16_B16_A16_UINT::getInstance();
	case F::R8_G8_B8_A8_UINT: return TextureDecoder_R8_G8_B8_A8_UINT::getInstance();
	case F::R24_X8_UNORM: return TextureDecoder_R24_X8::getInstance();
	case F::X24_G8_UINT: return TextureDecoder_X24_G8_UINT::getInstance();
	case F::D32_S8_FLOAT: return TextureDecoder_D32_S8_UINT_X24::getInstance();
	default:
		cemu_assert_debug(false);
		return nullptr;
	}
}

void OpenGLRenderer::texture_loadSlice(LatteTexture* hostTexture, sint32 width, sint32 height, sint32 depth, void* pixelData, sint32 sliceIndex, sint32 mipIndex, uint32 imageSize)
{
	LoadSliceDirect(static_cast<LatteTextureGL*>(hostTexture), width, height, pixelData, sliceIndex, mipIndex, imageSize);
}

void OpenGLRenderer::texture_clearColorSlice(LatteTexture* hostTexture, sint32 sliceIndex, sint32 mipIndex, float r, float g, float b, float a)
{
	cemu_assert_debug(!hostTexture->isDepth);
	sint32 w, h;
	hostTexture->GetEffectiveSize(w, h, mipIndex);
	renderstate_resetColorControl();
	renderTarget_setViewport(0, 0, w, h, 0.0f, 1.0f);
	LatteMRT::BindColorBufferOnly(hostTexture->GetOrCreateView(mipIndex, 1, sliceIndex, 1));
	glClearColor(r, g, b, a);
	glClear(GL_COLOR_BUFFER_BIT);
}

void OpenGLRenderer::texture_clearDepthSlice(LatteTexture* hostTexture, uint32 sliceIndex, sint32 mipIndex, bool clearDepth, bool clearStencil, float depthValue, uint32 stencilValue)
{
	cemu_assert_debug(hostTexture->isDepth);
	sint32 w, h;
	hostTexture->GetEffectiveSize(w, h, mipIndex);
	renderstate_resetColorControl();
	renderstate_resetDepthControl();
	renderTarget_setViewport(0, 0, w, h, 0.0f, 1.0f);
	LatteMRT::BindDepthBufferOnly(hostTexture->GetOrCreateView(mipIndex, 1, sliceIndex, 1));
	if (!hostTexture->hasStencil)
		clearStencil = false;
	// note: the GL backend has always cleared depth to 1.0, depthValue is not used
	if (clearDepth)
		glClearDepth(1.0);
	if (clearStencil)
	{
		renderstate_resetStencilMask();
		glClearStencil((GLint)stencilValue);
	}
	GLbitfield mask = 0;
	if (clearDepth)
		mask |= GL_DEPTH_BUFFER_BIT;
	if (clearStencil)
		mask |= GL_STENCIL_BUFFER_BIT;
	glClear(mask);
	catchOpenGLError();
}

void OpenGLRenderer::texture_clearSlice(LatteTexture* hostTextureGeneric, sint32 sliceIndex, sint32 mipIndex)
{
	auto* tex = static_cast<LatteTextureGL*>(hostTextureGeneric);
	LatteTextureGL::FormatInfoGL info;
	LatteTextureGL::GetOpenGLFormatInfo(tex->isDepth, tex->format, tex->dim, &info);
	sint32 w, h;
	tex->GetEffectiveSize(w, h, mipIndex);

	// glClear on an FBO is much faster than glClearTexSubImage on Nvidia, and the fallback when the latter is missing
	if (m_vendor == GfxVendor::Nvidia || glClearTexSubImage == nullptr)
	{
		if (info.glIsCompressed)
		{
			cemuLog_logDebug(LogType::Force, "OpenGL: clearing compressed textures is not supported");
			return;
		}
		if (tex->isDepth)
		{
			cemuLog_logDebug(LogType::Force, "OpenGL: clearing depth textures through texture_clearSlice is not supported");
			return;
		}
		glBindFramebuffer(GL_FRAMEBUFFER, s_gl.clearFBO);
		if (tex->dim == Latte::E_DIM::DIM_2D)
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex->glId_texture, mipIndex);
		else
			glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, tex->glId_texture, mipIndex, sliceIndex);
		glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		glBindFramebuffer(GL_FRAMEBUFFER, prevBoundFBO);
		return;
	}
	glClearTexSubImage(tex->glId_texture, mipIndex, 0, 0, sliceIndex, w, h, 1, info.glSuppliedFormat, info.glSuppliedFormatType, nullptr);
}

LatteTexture* OpenGLRenderer::texture_createTextureEx(Latte::E_DIM dim, MPTR physAddress, MPTR physMipAddress, Latte::E_GX2SURFFMT format, uint32 width, uint32 height, uint32 depth, uint32 pitch, uint32 mipLevels,
	uint32 swizzle, Latte::E_HWTILEMODE tileMode, bool isDepth)
{
	return new LatteTextureGL(dim, physAddress, physMipAddress, format, width, height, depth, pitch, mipLevels, swizzle, tileMode, isDepth);
}

void OpenGLRenderer::texture_setActiveTextureUnit(sint32 index)
{
	if (activeTextureUnit == index)
		return;
	glActiveTexture(GL_TEXTURE0 + index);
	activeTextureUnit = index;
}

void OpenGLRenderer::texture_bindAndActivate(LatteTextureView* textureView, uint32 textureUnit)
{
	auto* viewGL = static_cast<LatteTextureViewGL*>(textureView);
	const bool alreadyBound = m_latteBoundTextures[textureUnit] == viewGL;
	m_latteBoundTextures[textureUnit] = viewGL;
	texture_setActiveTextureUnit(textureUnit);
	if (!alreadyBound && viewGL)
		glBindTexture(viewGL->glTexTarget, viewGL->glTexId);
}

void OpenGLRenderer::texture_notifyDelete(LatteTextureView* textureView)
{
	for (void*& bound : m_latteBoundTextures)
	{
		if (bound == textureView)
			bound = nullptr;
	}
}

// binds a game texture; unlike texture_bindAndActivate this does not need to touch the active unit (DSA)
void OpenGLRenderer::texture_setLatteTexture(LatteTextureView* textureView, uint32 textureUnit)
{
	auto* viewGL = static_cast<LatteTextureViewGL*>(textureView);
	cemu_assert_debug(textureUnit < Latte::GPU_LIMITS::NUM_TEXTURES_PER_STAGE * 3);
	if (m_latteBoundTextures[textureUnit] == viewGL || viewGL == nullptr)
		return;
	if (glBindTextureUnit)
	{
		glBindTextureUnit(textureUnit, viewGL->glTexId);
		activeTextureUnit = -1; // unknown now
	}
	else
	{
		texture_setActiveTextureUnit(textureUnit);
		glBindTexture(viewGL->glTexTarget, viewGL->glTexId);
	}
	m_latteBoundTextures[textureUnit] = viewGL;
}

void OpenGLRenderer::texture_copyImageSubData(LatteTexture* src, sint32 srcMip, sint32 effectiveSrcX, sint32 effectiveSrcY, sint32 srcSlice, LatteTexture* dst, sint32 dstMip, sint32 effectiveDstX, sint32 effectiveDstY,
	sint32 dstSlice, sint32 effectiveCopyWidth, sint32 effectiveCopyHeight, sint32 srcDepth)
{
	auto* srcGL = static_cast<LatteTextureGL*>(src);
	auto* dstGL = static_cast<LatteTextureGL*>(dst);
	// glCopyImageSubData requires compatible storage; substitute formats need a conversion
	if ((srcGL->isAlternativeFormat || dstGL->isAlternativeFormat) && srcGL->glInternalFormat != dstGL->glInternalFormat)
	{
		if (srcGL->format == Latte::E_GX2SURFFMT::R16_G16_B16_A16_UINT && dstGL->format == Latte::E_GX2SURFFMT::BC4_UNORM)
		{
			// BC4 stored decoded as RG16F (non 2D textures)
			cemu_assert_debug(dstGL->dim != Latte::E_DIM::DIM_2D);
			texture_syncSliceSpecialBC4(srcGL, srcSlice, srcMip, dstGL, dstSlice, dstMip);
		}
		else
			cemuLog_logDebug(LogType::Force, "OpenGL: texture copy between incompatible substitute formats");
		return;
	}
	// integer to BC3 where the destination mip is smaller than one block
	if (srcGL->format == Latte::E_GX2SURFFMT::R32_G32_B32_A32_UINT && dstGL->format == Latte::E_GX2SURFFMT::BC3_UNORM &&
		((dstGL->width >> dstMip) < 4 || (dstGL->height >> dstMip) < 4))
	{
		texture_syncSliceSpecialIntegerToBC3(srcGL, srcSlice, srcMip, dstGL, dstSlice, dstMip);
		return;
	}
	catchOpenGLError();
	glCopyImageSubData(srcGL->glId_texture, srcGL->glTexTarget, srcMip, effectiveSrcX, effectiveSrcY, srcSlice, dstGL->glId_texture, dstGL->glTexTarget, dstMip, effectiveDstX, effectiveDstY, dstSlice, effectiveCopyWidth, effectiveCopyHeight, srcDepth);
	catchOpenGLError();
}

// raw BC4 blocks (read as RGBA16UI texels) are decoded on the CPU and stored into the RG16F substitute
void OpenGLRenderer::texture_syncSliceSpecialBC4(LatteTexture* srcTexture, sint32 srcSliceIndex, sint32 srcMipIndex, LatteTexture* dstTexture, sint32 dstSliceIndex, sint32 dstMipIndex)
{
	auto* srcGL = static_cast<LatteTextureGL*>(srcTexture);
	auto* dstGL = static_cast<LatteTextureGL*>(dstTexture);
	const sint32 srcW = std::max(srcTexture->width >> srcMipIndex, 1);
	const sint32 srcH = std::max(srcTexture->height >> srcMipIndex, 1);
	const sint32 dstW = std::max(dstTexture->width >> dstMipIndex, 1);
	const sint32 dstH = std::max(dstTexture->height >> dstMipIndex, 1);
	const sint32 blocksX = std::min(srcW, std::max(1, dstW / 4));
	const sint32 blocksY = std::min(srcH, std::max(1, dstH / 4));

	std::vector<uint8> blocks((size_t)blocksX * blocksY * 8);
	std::vector<float> texels((size_t)dstW * dstH * 2);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	if (glGetTextureSubImage)
		glGetTextureSubImage(srcGL->glId_texture, 0, 0, 0, srcSliceIndex, blocksX, blocksY, 1, GL_RGBA_INTEGER, GL_UNSIGNED_SHORT, blocksX * blocksY * 8, blocks.data());
	for (sint32 by = 0; by < blocksY; by++)
	{
		for (sint32 bx = 0; bx < blocksX; bx++)
		{
			float decoded[16];
			decodeBC4Block_UNORM(blocks.data() + (bx + by * blocksX) * 8, decoded);
			const sint32 rows = std::min(4, dstH - by * 4);
			const sint32 cols = std::min(4, dstW - bx * 4);
			for (sint32 y = 0; y < rows; y++)
			{
				for (sint32 x = 0; x < cols; x++)
				{
					const size_t pixel = (size_t)(bx * 4 + x) + (size_t)(by * 4 + y) * dstW;
					texels[pixel * 2 + 0] = decoded[x + y * 4];
					texels[pixel * 2 + 1] = decoded[x + y * 4];
				}
			}
		}
	}
	if (glGetTextureSubImage && glTextureSubImage3D)
		glTextureSubImage3D(dstGL->glId_texture, dstMipIndex, 0, 0, dstSliceIndex, dstW, dstH, 1, GL_RG, GL_FLOAT, texels.data());
	catchOpenGLError();
}

// the integer texels are BC3 blocks; they are re-uploaded as compressed data
void OpenGLRenderer::texture_syncSliceSpecialIntegerToBC3(LatteTexture* srcTexture, sint32 srcSliceIndex, sint32 srcMipIndex, LatteTexture* dstTexture, sint32 dstSliceIndex, sint32 dstMipIndex)
{
	auto* srcGL = static_cast<LatteTextureGL*>(srcTexture);
	auto* dstGL = static_cast<LatteTextureGL*>(dstTexture);
	const sint32 srcW = std::max(srcTexture->width >> srcMipIndex, 1);
	const sint32 srcH = std::max(srcTexture->height >> srcMipIndex, 1);
	const sint32 dstW = std::max(dstTexture->width >> dstMipIndex, 1);
	const sint32 dstH = std::max(dstTexture->height >> dstMipIndex, 1);
	const sint32 blocksX = std::min(srcW, std::max(1, dstW / 4));
	const sint32 blocksY = std::min(srcH, std::max(1, dstH / 4));
	std::vector<uint8> blocks((size_t)blocksX * blocksY * 16);
	catchOpenGLError();
	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	catchOpenGLError();
	if (glGetTextureSubImage)
		glGetTextureSubImage(srcGL->glId_texture, 0, 0, 0, srcSliceIndex, blocksX, blocksY, 1, GL_RGBA_INTEGER, GL_UNSIGNED_INT, blocksX * blocksY * 16, blocks.data());
	catchOpenGLError();
	if (glGetTextureSubImage && glCompressedTextureSubImage3D)
		glCompressedTextureSubImage3D(dstGL->glId_texture, dstMipIndex, 0, 0, dstSliceIndex, dstW, dstH, 1, dstGL->glInternalFormat, blocksX * blocksY * 16, blocks.data());
	catchOpenGLError();
}

LatteTextureReadbackInfo* OpenGLRenderer::texture_createReadback(LatteTextureView* textureView)
{
	return new LatteTextureReadbackInfoGL(textureView);
}

// ---- vertex/uniform buffer cache ----

void OpenGLRenderer::attributeStream_reset()
{
	attributeStream_unbindVertexBuffer();
	SetArrayElementBuffer(0);
	LatteDraw_resetAttributePointerCache();
	// attributes 0 and 1 are briefly enabled before everything is turned off
	SetAttributeArrayState(0, true, -1);
	SetAttributeArrayState(1, true, -1);
	for (uint32 i = 0; i < GPU_GL_MAX_NUM_ATTRIBUTE; i++)
		SetAttributeArrayState(i, false, -1);
}

void OpenGLRenderer::bufferCache_init(const sint32 bufferSize)
{
	glGenBuffers(1, &glAttributeCacheAB);
	glBindBuffer(GL_ARRAY_BUFFER, glAttributeCacheAB);
	glBufferData(GL_ARRAY_BUFFER, bufferSize, nullptr, GL_STREAM_DRAW);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void OpenGLRenderer::attributeStream_bindVertexCacheBuffer()
{
	if (_boundArrayBuffer == glAttributeCacheAB)
		return;
	_boundArrayBuffer = glAttributeCacheAB;
	glBindBuffer(GL_ARRAY_BUFFER, glAttributeCacheAB);
}

void OpenGLRenderer::attributeStream_unbindVertexBuffer()
{
	if (_boundArrayBuffer == 0)
		return;
	_boundArrayBuffer = 0;
	glBindBuffer(GL_ARRAY_BUFFER, 0);
}

// ---- shaders ----

RendererShader* OpenGLRenderer::shader_create(RendererShader::ShaderType type, uint64 baseHash, uint64 auxHash, const std::string& source, bool isGameShader, bool isGfxPackShader)
{
	return new RendererShaderGL(type, baseHash, auxHash, isGameShader, isGfxPackShader, source);
}

void OpenGLRenderer::shader_bind(RendererShader* shader)
{
	const GLuint program = static_cast<RendererShaderGL*>(shader)->GetProgram();
	GLbitfield stageBit;
	GLuint* bound;
	switch (shader->GetType())
	{
	case RendererShader::ShaderType::kVertex:
		stageBit = GL_VERTEX_SHADER_BIT;
		bound = &m_boundPrograms.vertex;
		break;
	case RendererShader::ShaderType::kFragment:
		stageBit = GL_FRAGMENT_SHADER_BIT;
		bound = &m_boundPrograms.fragment;
		break;
	case RendererShader::ShaderType::kGeometry:
		stageBit = GL_GEOMETRY_SHADER_BIT;
		bound = &m_boundPrograms.geometry;
		break;
	default:
		UNREACHABLE;
	}
	if (*bound == program)
		return;
	*bound = program;
	catchOpenGLError();
	glUseProgramStages(m_pipeline, stageBit, program);
	catchOpenGLError();
}

void OpenGLRenderer::shader_unbind(RendererShader::ShaderType shaderType)
{
	switch (shaderType)
	{
	case RendererShader::ShaderType::kVertex:
		glUseProgramStages(m_pipeline, GL_VERTEX_SHADER_BIT, 0);
		m_boundPrograms.vertex = (GLuint)-1;
		break;
	case RendererShader::ShaderType::kFragment:
		glUseProgramStages(m_pipeline, GL_FRAGMENT_SHADER_BIT, 0);
		m_boundPrograms.fragment = (GLuint)-1;
		break;
	case RendererShader::ShaderType::kGeometry:
		glUseProgramStages(m_pipeline, GL_GEOMETRY_SHADER_BIT, 0);
		m_boundPrograms.geometry = (GLuint)-1;
		break;
	default:
		UNREACHABLE;
	}
}

// ---- housekeeping ----

void OpenGLRenderer::cleanupAfterFrame()
{
	ReleaseBufferCacheEntries();
}

void OpenGLRenderer::ReleaseBufferCacheEntries()
{
	// buffer cache memory is never handed back through the GL backend, nothing is queued
}
