// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"

#include "VideoCommon/Assets/CustomTextureData.h"

// Short text notices drawn in the top-left corner of the render window (through Dear ImGui).
namespace OSD
{
// Kinds of message. A typed message replaces the previous message of the same type; typed
// messages are listed before the untyped ones, in this order, so Typeless has to stay last.
enum class MessageType
{
  NetPlayPing,
  NetPlayBuffer,
  Typeless,
};

// Text colours, as 0xAARRGGBB.
namespace Color
{
constexpr u32 CYAN = 0xFF00FFFF;
constexpr u32 GREEN = 0xFF00FF00;
constexpr u32 RED = 0xFFFF0000;
constexpr u32 YELLOW = 0xFFFFFF30;
}  // namespace Color

// How long a message stays up, in milliseconds.
namespace Duration
{
constexpr u32 SHORT = 2000;
constexpr u32 NORMAL = 5000;
constexpr u32 VERY_LONG = 10000;
}  // namespace Duration

// Queues a message. `icon` (optional, RGBA8) is drawn to the left of the text; it must outlive
// the message.
void AddMessage(std::string message, u32 ms = Duration::SHORT, u32 argb = Color::YELLOW,
                const VideoCommon::CustomTextureData::ArraySlice::Level* icon = nullptr);
void AddTypedMessage(MessageType type, std::string message, u32 ms = Duration::SHORT,
                     u32 argb = Color::YELLOW,
                     const VideoCommon::CustomTextureData::ArraySlice::Level* icon = nullptr);

// Emits the ImGui windows of the live messages and drops the expired ones. Video thread, once
// per frame.
void DrawMessages();
void ClearMessages();

// Pixels at the left / top of the window that are covered by something else (the messages are
// moved past them).
void SetObscuredPixelsLeft(int width);
void SetObscuredPixelsTop(int height);
}  // namespace OSD
