// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "Common/GLInclude/GLInclude.h"

#include <cstdint>
#include <cstdio>
#include <string>

namespace
{
	struct Backend
	{
		std::string versionLine = "#version 130\n";
		GLuint program = 0;
		GLuint vertexShader = 0;
		GLuint fragmentShader = 0;
		GLint uniformAtlas = 0;
		GLint uniformProjection = 0;
		GLint attribPosition = 0;
		GLint attribUV = 0;
		GLint attribColor = 0;
		GLuint vertexBuffer = 0;
		GLuint indexBuffer = 0;
		GLuint fontTexture = 0;
	};
	Backend s_backend;

	constexpr GLenum kIndexType = sizeof(ImDrawIdx) == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT;

	// ---- shaders ----

	// GLSL before 1.30 (and ES 1.00) uses attribute/varying and gl_FragColor
	bool IsLegacyGlsl(const std::string& versionLine)
	{
		int version = 130;
		std::sscanf(versionLine.c_str(), "#version %d", &version);
		return version < 130;
	}

	std::string VertexSource(const std::string& versionLine)
	{
		const bool legacy = IsLegacyGlsl(versionLine);
		const char* in = legacy ? "attribute" : "in";
		const char* out = legacy ? "varying" : "out";
		std::string src = versionLine;
		if (versionLine.find(" es") != std::string::npos)
			src += "precision mediump float;\n";
		src += "uniform mat4 uProjection;\n";
		src += std::string(in) + " vec2 aPosition;\n";
		src += std::string(in) + " vec2 aUV;\n";
		src += std::string(in) + " vec4 aColor;\n";
		src += std::string(out) + " vec2 vUV;\n";
		src += std::string(out) + " vec4 vColor;\n";
		src += "void main()\n{\n\tvUV = aUV;\n\tvColor = aColor;\n\tgl_Position = uProjection * vec4(aPosition, 0.0, 1.0);\n}\n";
		return src;
	}

	std::string FragmentSource(const std::string& versionLine)
	{
		const bool legacy = IsLegacyGlsl(versionLine);
		std::string src = versionLine;
		src += "#ifdef GL_ES\nprecision mediump float;\n#endif\n";
		src += "uniform sampler2D uAtlas;\n";
		if (legacy)
		{
			src += "varying vec2 vUV;\nvarying vec4 vColor;\n";
			src += "void main()\n{\n\tgl_FragColor = vColor * texture2D(uAtlas, vUV);\n}\n";
		}
		else
		{
			src += "in vec2 vUV;\nin vec4 vColor;\nout vec4 fragColor;\n";
			src += "void main()\n{\n\tfragColor = vColor * texture(uAtlas, vUV);\n}\n";
		}
		return src;
	}

	// prints the info log to stderr; returns whether compile/link succeeded
	template<typename TGetIv, typename TGetLog>
	bool ReportStatus(GLuint object, GLenum statusQuery, TGetIv getIv, TGetLog getLog, const char* what)
	{
		GLint ok = GL_FALSE, logLength = 0;
		getIv(object, statusQuery, &ok);
		getIv(object, GL_INFO_LOG_LENGTH, &logLength);
		if (ok == GL_FALSE)
			std::fprintf(stderr, "imgui OpenGL backend: %s failed (%s)\n", what, s_backend.versionLine.c_str());
		if (logLength > 1)
		{
			std::string log(logLength, '\0');
			getLog(object, logLength, nullptr, log.data());
			std::fprintf(stderr, "%s\n", log.c_str());
		}
		return ok == GL_TRUE;
	}

	GLuint CompileShader(GLenum stage, const std::string& source, const char* what)
	{
		const GLuint shader = glCreateShader(stage);
		const GLchar* text = source.c_str();
		glShaderSource(shader, 1, &text, nullptr);
		glCompileShader(shader);
		ReportStatus(shader, GL_COMPILE_STATUS, glGetShaderiv, glGetShaderInfoLog, what);
		return shader;
	}

	// ---- state the renderer must leave as it found it ----

	struct SavedGLState
	{
		GLenum activeTexture;
		GLint program, texture, arrayBuffer, vertexArray;
#ifdef GL_SAMPLER_BINDING
		GLint sampler;
#endif
#ifdef GL_POLYGON_MODE
		GLint polygonMode[2];
#endif
		GLint viewport[4], scissorBox[4];
		GLboolean cullFace, depthTest, scissorTest;
		bool lowerLeftOrigin = true;

		// also selects texture unit 0, which the draw uses
		void Capture()
		{
			glGetIntegerv(GL_ACTIVE_TEXTURE, (GLint*)&activeTexture);
			glActiveTexture(GL_TEXTURE0);
			glGetIntegerv(GL_CURRENT_PROGRAM, &program);
			glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
#ifdef GL_SAMPLER_BINDING
			glGetIntegerv(GL_SAMPLER_BINDING, &sampler);
#endif
			glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &arrayBuffer);
			glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vertexArray);
#ifdef GL_POLYGON_MODE
			glGetIntegerv(GL_POLYGON_MODE, polygonMode);
#endif
			glGetIntegerv(GL_VIEWPORT, viewport);
			glGetIntegerv(GL_SCISSOR_BOX, scissorBox);
			cullFace = glIsEnabled(GL_CULL_FACE);
			depthTest = glIsEnabled(GL_DEPTH_TEST);
			scissorTest = glIsEnabled(GL_SCISSOR_TEST);
#if defined(GL_CLIP_ORIGIN) && !defined(__APPLE__)
			// glClipControl (GL 4.5) can flip the window origin to the top
			GLint origin = 0;
			glGetIntegerv(GL_CLIP_ORIGIN, &origin);
			lowerLeftOrigin = origin != GL_UPPER_LEFT;
#endif
		}

		void Restore() const
		{
			glUseProgram(program);
			glBindTexture(GL_TEXTURE_2D, texture);
#ifdef GL_SAMPLER_BINDING
			glBindSampler(0, sampler);
#endif
			glActiveTexture(activeTexture);
			glBindVertexArray(vertexArray);
			glBindBuffer(GL_ARRAY_BUFFER, arrayBuffer);
			// the renderer expects blending off afterwards, whatever it was before
			glDisablei(GL_BLEND, 0);
			auto setEnabled = [](GLenum cap, GLboolean on) { on ? glEnable(cap) : glDisable(cap); };
			setEnabled(GL_CULL_FACE, cullFace);
			setEnabled(GL_DEPTH_TEST, depthTest);
			setEnabled(GL_SCISSOR_TEST, scissorTest);
#ifdef GL_POLYGON_MODE
			glPolygonMode(GL_FRONT_AND_BACK, (GLenum)polygonMode[0]);
#endif
			glViewport(viewport[0], viewport[1], (GLsizei)viewport[2], (GLsizei)viewport[3]);
			glScissor(scissorBox[0], scissorBox[1], (GLsizei)scissorBox[2], (GLsizei)scissorBox[3]);
		}
	};

	// blending, no culling/depth, scissored fill; projection maps display space to clip space
	void PrepareDraw(const ImDrawData* drawData, int fbWidth, int fbHeight, GLuint vertexArray)
	{
		glEnablei(GL_BLEND, 0);
		glBlendEquationSeparatei(0, GL_FUNC_ADD, GL_FUNC_ADD);
		glBlendFuncSeparatei(0, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		glDisable(GL_CULL_FACE);
		glDisable(GL_DEPTH_TEST);
		glEnable(GL_SCISSOR_TEST);
#ifdef GL_POLYGON_MODE
		glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
#endif
		glViewport(0, 0, (GLsizei)fbWidth, (GLsizei)fbHeight);

		const float left = drawData->DisplayPos.x;
		const float right = drawData->DisplayPos.x + drawData->DisplaySize.x;
		const float top = drawData->DisplayPos.y;
		const float bottom = drawData->DisplayPos.y + drawData->DisplaySize.y;
		// column major, y flipped so the top of the display is up
		const float projection[16] = {
			2.0f / (right - left), 0.0f, 0.0f, 0.0f,
			0.0f, 2.0f / (top - bottom), 0.0f, 0.0f,
			0.0f, 0.0f, -1.0f, 0.0f,
			(right + left) / (left - right), (top + bottom) / (bottom - top), 0.0f, 1.0f,
		};
		glUseProgram(s_backend.program);
		glUniform1i(s_backend.uniformAtlas, 0);
		glUniformMatrix4fv(s_backend.uniformProjection, 1, GL_FALSE, projection);
#ifdef GL_SAMPLER_BINDING
		glBindSampler(0, 0); // sampling state comes from the texture itself
#endif

		glBindVertexArray(vertexArray);
		glBindBuffer(GL_ARRAY_BUFFER, s_backend.vertexBuffer);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_backend.indexBuffer);
		const GLsizei stride = sizeof(ImDrawVert);
		glEnableVertexAttribArray(s_backend.attribPosition);
		glEnableVertexAttribArray(s_backend.attribUV);
		glEnableVertexAttribArray(s_backend.attribColor);
		glVertexAttribPointer(s_backend.attribPosition, 2, GL_FLOAT, GL_FALSE, stride, (const void*)offsetof(ImDrawVert, pos));
		glVertexAttribPointer(s_backend.attribUV, 2, GL_FLOAT, GL_FALSE, stride, (const void*)offsetof(ImDrawVert, uv));
		glVertexAttribPointer(s_backend.attribColor, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, (const void*)offsetof(ImDrawVert, col));
	}

	// all command lists go into one vertex and one index buffer
	void UploadGeometry(const ImDrawData* drawData)
	{
		const GLsizeiptr vertexBytes = (GLsizeiptr)drawData->TotalVtxCount * sizeof(ImDrawVert);
		const GLsizeiptr indexBytes = (GLsizeiptr)drawData->TotalIdxCount * sizeof(ImDrawIdx);
		glBufferData(GL_ARRAY_BUFFER, vertexBytes, nullptr, GL_STREAM_DRAW);
		glBufferData(GL_ELEMENT_ARRAY_BUFFER, indexBytes, nullptr, GL_STREAM_DRAW);
		GLintptr vertexAt = 0, indexAt = 0;
		for (int i = 0; i < drawData->CmdListsCount; i++)
		{
			const ImDrawList* list = drawData->CmdLists[i];
			const GLsizeiptr vBytes = (GLsizeiptr)list->VtxBuffer.Size * sizeof(ImDrawVert);
			const GLsizeiptr iBytes = (GLsizeiptr)list->IdxBuffer.Size * sizeof(ImDrawIdx);
			glBufferSubData(GL_ARRAY_BUFFER, vertexAt, vBytes, list->VtxBuffer.Data);
			glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, indexAt, iBytes, list->IdxBuffer.Data);
			vertexAt += vBytes;
			indexAt += iBytes;
		}
	}
}

bool ImGui_ImplOpenGL3_Init(const char* glsl_version)
{
	ImGuiIO& io = ImGui::GetIO();
	io.BackendRendererName = "pixelu_opengl3";
	io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset; // draws use a base vertex
	s_backend.versionLine = std::string(glsl_version ? glsl_version : "#version 130") + "\n";
	return true;
}

void ImGui_ImplOpenGL3_Shutdown()
{
	ImGui_ImplOpenGL3_DestroyDeviceObjects();
}

void ImGui_ImplOpenGL3_NewFrame()
{
	if (!s_backend.program)
		ImGui_ImplOpenGL3_CreateDeviceObjects();
	if (!s_backend.fontTexture)
		ImGui_ImplOpenGL3_CreateFontsTexture();
}

void ImGui_ImplOpenGL3_RenderDrawData(ImDrawData* draw_data)
{
	const ImVec2 scale = draw_data->FramebufferScale;
	const int fbWidth = (int)(draw_data->DisplaySize.x * scale.x);
	const int fbHeight = (int)(draw_data->DisplaySize.y * scale.y);
	if (fbWidth <= 0 || fbHeight <= 0)
		return;

	SavedGLState saved;
	saved.Capture();

	// VAOs are per context (main and pad window), so use a throwaway one
	GLuint vertexArray = 0;
	glGenVertexArrays(1, &vertexArray);
	PrepareDraw(draw_data, fbWidth, fbHeight, vertexArray);
	UploadGeometry(draw_data);

	const ImVec2 origin = draw_data->DisplayPos;
	int listVertexBase = 0, listIndexBase = 0;
	for (int i = 0; i < draw_data->CmdListsCount; i++)
	{
		const ImDrawList* list = draw_data->CmdLists[i];
		for (const ImDrawCmd& cmd : list->CmdBuffer)
		{
			if (cmd.UserCallback)
			{
				if (cmd.UserCallback == ImDrawCallback_ResetRenderState)
					PrepareDraw(draw_data, fbWidth, fbHeight, vertexArray);
				else
					cmd.UserCallback(list, &cmd);
				continue;
			}
			// clip rectangle in framebuffer pixels
			const float x0 = (cmd.ClipRect.x - origin.x) * scale.x;
			const float y0 = (cmd.ClipRect.y - origin.y) * scale.y;
			const float x1 = (cmd.ClipRect.z - origin.x) * scale.x;
			const float y1 = (cmd.ClipRect.w - origin.y) * scale.y;
			if (x0 >= fbWidth || y0 >= fbHeight || x1 < 0.0f || y1 < 0.0f)
				continue;
			if (saved.lowerLeftOrigin)
				glScissor((int)x0, (int)(fbHeight - y1), (int)(x1 - x0), (int)(y1 - y0));
			else
				glScissor((int)x0, (int)y0, (int)(x1 - x0), (int)(y1 - y0));

			glBindTexture(GL_TEXTURE_2D, (GLuint)(intptr_t)cmd.TextureId);
			const uintptr_t firstIndexByte = (uintptr_t)(listIndexBase + cmd.IdxOffset) * sizeof(ImDrawIdx);
			glDrawElementsBaseVertex(GL_TRIANGLES, (GLsizei)cmd.ElemCount, kIndexType, (const void*)firstIndexByte, (GLint)(listVertexBase + cmd.VtxOffset));
		}
		listVertexBase += list->VtxBuffer.Size;
		listIndexBase += list->IdxBuffer.Size;
	}

	glDeleteVertexArrays(1, &vertexArray);
	saved.Restore();
}

bool ImGui_ImplOpenGL3_CreateFontsTexture()
{
	if (s_backend.fontTexture)
		return false;

	ImGuiIO& io = ImGui::GetIO();
	unsigned char* pixels;
	int width, height;
	io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

	GLint previous;
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous);
	glGenTextures(1, &s_backend.fontTexture);
	glBindTexture(GL_TEXTURE_2D, s_backend.fontTexture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	io.Fonts->TexID = (ImTextureID)(intptr_t)s_backend.fontTexture;
	glBindTexture(GL_TEXTURE_2D, previous);
	return true;
}

void ImGui_ImplOpenGL3_DestroyFontsTexture()
{
	if (!s_backend.fontTexture)
		return;
	glDeleteTextures(1, &s_backend.fontTexture);
	ImGui::GetIO().Fonts->TexID = 0;
	s_backend.fontTexture = 0;
}

bool ImGui_ImplOpenGL3_CreateDeviceObjects()
{
	GLint previousTexture, previousArrayBuffer, previousVertexArray;
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
	glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &previousArrayBuffer);
	glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &previousVertexArray);

	Backend& b = s_backend;
	b.vertexShader = CompileShader(GL_VERTEX_SHADER, VertexSource(b.versionLine), "vertex shader compile");
	b.fragmentShader = CompileShader(GL_FRAGMENT_SHADER, FragmentSource(b.versionLine), "fragment shader compile");
	b.program = glCreateProgram();
	glAttachShader(b.program, b.vertexShader);
	glAttachShader(b.program, b.fragmentShader);
	glLinkProgram(b.program);
	ReportStatus(b.program, GL_LINK_STATUS, glGetProgramiv, glGetProgramInfoLog, "shader program link");

	b.uniformAtlas = glGetUniformLocation(b.program, "uAtlas");
	b.uniformProjection = glGetUniformLocation(b.program, "uProjection");
	b.attribPosition = glGetAttribLocation(b.program, "aPosition");
	b.attribUV = glGetAttribLocation(b.program, "aUV");
	b.attribColor = glGetAttribLocation(b.program, "aColor");

	glGenBuffers(1, &b.vertexBuffer);
	glGenBuffers(1, &b.indexBuffer);
	ImGui_ImplOpenGL3_CreateFontsTexture();

	glBindTexture(GL_TEXTURE_2D, previousTexture);
	glBindBuffer(GL_ARRAY_BUFFER, previousArrayBuffer);
	glBindVertexArray(previousVertexArray);
	return true;
}

void ImGui_ImplOpenGL3_DestroyDeviceObjects()
{
	Backend& b = s_backend;
	for (GLuint* buffer : {&b.vertexBuffer, &b.indexBuffer})
	{
		if (*buffer)
			glDeleteBuffers(1, buffer);
		*buffer = 0;
	}
	for (GLuint* shader : {&b.vertexShader, &b.fragmentShader})
	{
		if (*shader)
		{
			if (b.program)
				glDetachShader(b.program, *shader);
			glDeleteShader(*shader);
		}
		*shader = 0;
	}
	if (b.program)
		glDeleteProgram(b.program);
	b.program = 0;
	ImGui_ImplOpenGL3_DestroyFontsTexture();
}
