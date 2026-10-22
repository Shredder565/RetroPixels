// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "imgui.h"

// Rotating a group of draw commands: call ImRotateStart(), draw, then
// ImRotateEnd() to turn everything drawn since the start around center.
void ImRotateStart();
// middle of the bounding box of the vertices drawn since ImRotateStart()
ImVec2 ImRotationCenter();
void ImRotateEnd(float rad, ImVec2 center = ImRotationCenter());

inline ImVec2 operator-(const ImVec2& l, const ImVec2& r) { return{ l.x - r.x, l.y - r.y }; }
inline bool operator<(const ImVec2& lhs, const ImVec2& rhs) { return lhs.x < rhs.x; }
inline bool operator>=(const ImVec2& lhs, const ImVec2& rhs) { return lhs.x >= rhs.x; }

bool ImGui_BeginPadDistinct(const char* name, bool* p_open, ImGuiWindowFlags flags, bool pad);

// Fonts are made on demand: ImGui_GetFont() returns null for a size not built
// yet and queues it; ImGui_PrecacheFonts() (outside a frame) builds the queue.
void ImGui_PrecacheFonts();
void ImGui_ClearFonts();
ImFont* ImGui_GetFont(float size);
// feeds mouse, keyboard and controller navigation into imgui for the window
void ImGui_UpdateWindowInformation(bool mainWindow);
