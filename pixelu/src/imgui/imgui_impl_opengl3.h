// Original code for RetroPixels (PixelU). Not derived from Cemu.
// Draws imgui frames with OpenGL 3 (shaders, VAO, base-vertex draws).
#pragma once

#include "imgui.h"
#include "Common/GLInclude/GLInclude.h"

// glsl_version: the "#version ..." line for the shaders, "#version 130" when null
IMGUI_IMPL_API bool     ImGui_ImplOpenGL3_Init(const char* glsl_version = NULL);
IMGUI_IMPL_API void     ImGui_ImplOpenGL3_Shutdown();
// creates the program, buffers and font texture when missing
IMGUI_IMPL_API void     ImGui_ImplOpenGL3_NewFrame();
// draws into the bound framebuffer; GL state the caller had is put back, except that blending ends up disabled
IMGUI_IMPL_API void     ImGui_ImplOpenGL3_RenderDrawData(ImDrawData* draw_data);

IMGUI_IMPL_API bool     ImGui_ImplOpenGL3_CreateFontsTexture();
IMGUI_IMPL_API void     ImGui_ImplOpenGL3_DestroyFontsTexture();
IMGUI_IMPL_API bool     ImGui_ImplOpenGL3_CreateDeviceObjects();
IMGUI_IMPL_API void     ImGui_ImplOpenGL3_DestroyDeviceObjects();
