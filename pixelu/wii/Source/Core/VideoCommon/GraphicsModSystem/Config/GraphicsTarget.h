// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <optional>
#include <string>
#include <variant>

#include <picojson.h>

#include "Common/CommonTypes.h"
#include "VideoCommon/TextureDecoder.h"
#include "VideoCommon/XFMemory.h"

// What a graphics mod feature hooks into. Texture targets match by texture name (as written
// in texture dumps); framebuffer targets match copies of a given size and format; projection
// targets match projection matrix changes, optionally only while a given texture is bound.

struct TextureTarget
{
  std::string m_texture_info_string;
};

// A draw call using the texture.
struct DrawStartedTextureTarget final : public TextureTarget
{
};

// The texture being loaded into the texture cache.
struct LoadTextureTarget final : public TextureTarget
{
};

// The texture's host texture being created.
struct CreateTextureTarget final : public TextureTarget
{
};

struct FBTarget
{
  u32 m_height = 0;
  u32 m_width = 0;
  TextureFormat m_texture_format = TextureFormat::I4;
};

struct EFBTarget final : public FBTarget
{
};

struct XFBTarget final : public FBTarget
{
};

struct ProjectionTarget
{
  std::optional<std::string> m_texture_info_string;
  ProjectionType m_projection_type = ProjectionType::Perspective;
};

using GraphicsTargetConfig =
    std::variant<DrawStartedTextureTarget, LoadTextureTarget, CreateTextureTarget, EFBTarget,
                 XFBTarget, ProjectionTarget>;

void SerializeTargetToConfig(picojson::object& json_obj, const GraphicsTargetConfig& target);
std::optional<GraphicsTargetConfig> DeserializeTargetFromConfig(const picojson::object& obj);

// Targets carry no per-user state; these are no-ops.
void SerializeTargetToProfile(picojson::object* obj, const GraphicsTargetConfig& target);
void DeserializeTargetFromProfile(const picojson::object& obj, GraphicsTargetConfig* target);
