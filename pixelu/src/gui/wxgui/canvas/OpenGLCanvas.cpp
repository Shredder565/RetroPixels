// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "wxgui/canvas/OpenGLCanvas.h"

#include "wxgui/canvas/IRenderCanvas.h"

#include "Cafe/HW/Latte/Renderer/OpenGL/OpenGLRenderer.h"

#include "config/CemuConfig.h"

#include "Common/GLInclude/GLInclude.h"
// wx/glcanvas.h pulls in the system GL/gl.h, which clashes with GLInclude's definitions: only here, never in a header
#include <wx/glcanvas.h>

namespace
{
	// RGBA8 double-buffered surface with 16-bit depth and 8-bit stencil
	const int kPixelFormat[] = {
		WX_GL_RGBA,
		WX_GL_DOUBLEBUFFER,
		WX_GL_DEPTH_SIZE, 16,
		WX_GL_MIN_RED, 8,
		WX_GL_MIN_GREEN, 8,
		WX_GL_MIN_BLUE, 8,
		WX_GL_MIN_ALPHA, 8,
		WX_GL_STENCIL_SIZE, 8,
		0,
	};

	class GLWindowCanvas;

	// One GL context shared by the TV canvas and the GamePad canvas. The OpenGL renderer reaches
	// the canvases through these callbacks (installed for the lifetime of the program).
	class SharedGLContext final : public OpenGLCanvasCallbacks
	{
	  public:
		SharedGLContext() { SetOpenGLCanvasCallbacks(this); }
		~SharedGLContext() override { ClearOpenGLCanvasCallbacks(); }

		void Attach(GLWindowCanvas* canvas, bool isTV)
		{
			(isTV ? m_tv : m_pad) = canvas;
		}

		void Detach(bool isTV)
		{
			(isTV ? m_tv : m_pad) = nullptr;
			// the context goes once neither canvas is left
			if (!m_tv && !m_pad)
			{
				delete m_context;
				m_context = nullptr;
			}
		}

		void CreateContext(wxGLCanvas* canvas)
		{
			m_context = new wxGLContext(canvas);
		}

		bool HasPadViewOpen() const override { return m_pad != nullptr; }
		bool MakeCurrent(bool padView) override;
		void SwapBuffers(bool swapTV, bool swapDRC) override;

	  private:
		wxGLContext* m_context = nullptr;
		GLWindowCanvas* m_tv = nullptr;
		GLWindowCanvas* m_pad = nullptr;
	};

	SharedGLContext s_sharedContext;

	class GLWindowCanvas : public IRenderCanvas, public wxGLCanvas
	{
	  public:
		GLWindowCanvas(wxWindow* parent, const wxSize& size, bool is_main_window)
			: IRenderCanvas(is_main_window),
			  wxGLCanvas(parent, wxID_ANY, kPixelFormat, wxDefaultPosition, size, wxFULL_REPAINT_ON_RESIZE | wxWANTS_CHARS)
		{
			s_sharedContext.Attach(this, m_is_main_window);
			if (m_is_main_window)
			{
				// the TV canvas owns the renderer
				s_sharedContext.CreateContext(this);
				g_renderer = std::make_unique<OpenGLRenderer>();
			}
			wxWindow::EnableTouchEvents(wxTOUCH_PAN_GESTURES);
		}

		~GLWindowCanvas() override
		{
			s_sharedContext.Detach(m_is_main_window);
		}

		// apply the vsync setting (any value above 0 means on) when it changed since the last frame
		void ApplyVSync()
		{
			const int wanted = GetConfig().vsync.GetValue() > 0 ? 1 : 0;
			if (wanted == m_vsync)
				return;
#if BOOST_OS_WINDOWS
			if (wglSwapIntervalEXT)
				wglSwapIntervalEXT(wanted);
#elif BOOST_OS_LINUX || BOOST_OS_BSD
			if (eglSwapInterval && eglSwapInterval(eglGetCurrentDisplay(), wanted) == EGL_FALSE)
				cemuLog_log(LogType::Force, "OpenGL: could not change vsync through EGL");
#else
			cemuLog_log(LogType::Force, "OpenGL: vsync is not supported on this platform");
#endif
			m_vsync = wanted;
		}

	  private:
		int m_vsync = -1; // unknown until the first frame
	};

	bool SharedGLContext::MakeCurrent(bool padView)
	{
		GLWindowCanvas* target = padView ? m_pad : m_tv;
		if (target == nullptr)
			return false;
		m_context->SetCurrent(*target);
		return true;
	}

	void SharedGLContext::SwapBuffers(bool swapTV, bool swapDRC)
	{
		const std::pair<bool, GLWindowCanvas*> targets[] = {{swapTV, m_tv}, {swapDRC, m_pad}};
		for (size_t i = 0; i < std::size(targets); i++)
		{
			auto [wanted, canvas] = targets[i];
			if (!wanted || !canvas)
				continue;
			MakeCurrent(i == 1);
			canvas->SwapBuffers();
			canvas->ApplyVSync();
		}
		MakeCurrent(false);
	}
}

wxWindow* GLCanvas_Create(wxWindow* parent, const wxSize& size, bool is_main_window)
{
	return new GLWindowCanvas(parent, size, is_main_window);
}
