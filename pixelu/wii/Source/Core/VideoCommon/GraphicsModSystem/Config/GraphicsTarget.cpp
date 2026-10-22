// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/GraphicsModSystem/Config/GraphicsTarget.h"

#include <type_traits>

#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "VideoCommon/TextureCacheBase.h"

namespace
{
// The "texture_filename" member, which every target except projections requires.
std::optional<std::string> ReadTextureFilename(const picojson::object& obj)
{
  const auto it = obj.find("texture_filename");
  if (it == obj.end())
  {
    ERROR_LOG_FMT(VIDEO,
                  "Failed to load mod configuration file, option 'texture_filename' not found");
    return std::nullopt;
  }
  if (!it->second.is<std::string>())
  {
    ERROR_LOG_FMT(
        VIDEO,
        "Failed to load mod configuration file, option 'texture_filename' is not a string type");
    return std::nullopt;
  }
  return it->second.get<std::string>();
}

void ReportInvalidFilename(std::string_view why)
{
  ERROR_LOG_FMT(VIDEO, "Failed to load mod configuration file, value in 'texture_filename' is {}",
                why);
}

// Parses a framebuffer copy name "<prefix>_<width>x<height>_<format>[_...]" into a target.
// The first prefix.size() + 1 characters are skipped without being compared.
template <typename Target>
std::optional<Target> ReadFramebufferTarget(const picojson::object& obj, std::string_view prefix)
{
  static_assert(std::is_base_of_v<FBTarget, Target>);

  const std::optional<std::string> filename = ReadTextureFilename(obj);
  if (!filename)
    return std::nullopt;

  const std::string rest = filename->substr(prefix.size() + 1);
  if (SplitString(rest, '_').size() == 1)
  {
    ReportInvalidFilename("not valid");
    return std::nullopt;
  }

  const std::vector<std::string> sides = SplitString(rest, 'x');
  if (sides.size() != 2)
  {
    ReportInvalidFilename("not valid, width and height separator found more matches than expected");
    return std::nullopt;
  }
  const std::string& left = sides[0];
  const std::string& right = sides[1];

  Target target;

  // Width: whatever follows the last underscore before the 'x'.
  const std::size_t width_start = left.rfind('_');
  const std::string width = width_start == std::string::npos ? left : left.substr(width_start + 1);
  if (!TryParse(width, &target.m_width))
  {
    ReportInvalidFilename("not valid, width not a number");
    return std::nullopt;
  }

  // Height: up to the first underscore after the 'x', which must be followed by something.
  const std::size_t height_end = right.find('_');
  if (height_end == std::string::npos || height_end + 1 == right.size())
  {
    ReportInvalidFilename("not valid, underscore after height is missing or incomplete");
    return std::nullopt;
  }
  if (!TryParse(right.substr(0, height_end), &target.m_height))
  {
    ReportInvalidFilename("not valid, height not a number");
    return std::nullopt;
  }

  // Format: the next field, ending at the following underscore if there is one.
  const std::size_t format_start = height_end + 1;
  const std::size_t format_end = right.find('_', format_start);
  const std::string format_text = format_end == std::string::npos ?
                                      right.substr(format_start) :
                                      right.substr(format_start, format_end - format_start);
  u32 format = 0;
  if (!TryParse(format_text, &format))
  {
    ReportInvalidFilename("not valid, texture format is not a number");
    return std::nullopt;
  }
  if (!IsValidTextureFormat(static_cast<TextureFormat>(format)))
  {
    ReportInvalidFilename("not valid, texture format is not valid");
    return std::nullopt;
  }
  target.m_texture_format = static_cast<TextureFormat>(format);
  return target;
}

// Reads the texture name a texture target matches. Names of framebuffer copies contain a
// "_n<count>" part that differs between dumps of the same copy, so it is removed.
std::optional<std::string> ReadTextureName(const picojson::object& obj)
{
  std::optional<std::string> name = ReadTextureFilename(obj);
  if (!name)
    return std::nullopt;

  std::string_view kind;
  if (name->starts_with(EFB_DUMP_PREFIX))
    kind = "an efb";
  else if (name->starts_with(XFB_DUMP_PREFIX))
    kind = "a xfb";
  else
    return name;

  const std::size_t count_start = name->find("_n");
  if (count_start == std::string::npos)
  {
    ERROR_LOG_FMT(VIDEO,
                  "Failed to load mod configuration file, value in 'texture_filename' is {} "
                  "without a count",
                  kind);
    return std::nullopt;
  }
  const std::size_t count_end = name->find('_', count_start + 2);
  const std::size_t count_length =
      (count_end == std::string::npos ? name->size() : count_end) - count_start;
  name->erase(count_start, count_length);
  return name;
}

template <typename Target>
std::optional<GraphicsTargetConfig> ReadTextureTarget(const picojson::object& obj)
{
  std::optional<std::string> name = ReadTextureName(obj);
  if (!name)
    return std::nullopt;
  Target target;
  target.m_texture_info_string = std::move(*name);
  return target;
}

std::optional<GraphicsTargetConfig> ReadProjectionTarget(const picojson::object& obj)
{
  ProjectionTarget target;
  if (obj.contains("texture_filename"))
  {
    target.m_texture_info_string = ReadTextureName(obj);
    if (!target.m_texture_info_string)
      return std::nullopt;
  }

  const auto value = obj.find("value");
  if (value == obj.end())
  {
    ERROR_LOG_FMT(VIDEO, "Failed to load mod configuration file, option 'value' not found");
    return std::nullopt;
  }
  if (!value->second.is<std::string>())
  {
    ERROR_LOG_FMT(VIDEO,
                  "Failed to load mod configuration file, option 'value' is not a string type");
    return std::nullopt;
  }

  const std::string& kind = value->second.get<std::string>();
  if (kind == "2d")
  {
    target.m_projection_type = ProjectionType::Orthographic;
  }
  else if (kind == "3d")
  {
    target.m_projection_type = ProjectionType::Perspective;
  }
  else
  {
    ERROR_LOG_FMT(VIDEO, "Failed to load mod configuration file, option 'value' is not a valid "
                         "value, valid values are: 2d, 3d");
    return std::nullopt;
  }
  return target;
}

std::string FramebufferName(std::string_view prefix, const FBTarget& target)
{
  return fmt::format("{}_{}x{}_{}", prefix, target.m_width, target.m_height,
                     static_cast<int>(target.m_texture_format));
}
}  // namespace

void SerializeTargetToConfig(picojson::object& json_obj, const GraphicsTargetConfig& target)
{
  const auto put = [&](const char* type, std::string filename) {
    json_obj.emplace("type", type);
    json_obj.emplace("texture_filename", std::move(filename));
  };

  if (const auto* draw = std::get_if<DrawStartedTextureTarget>(&target))
  {
    put("draw_started", draw->m_texture_info_string);
  }
  else if (const auto* load = std::get_if<LoadTextureTarget>(&target))
  {
    put("load_texture", load->m_texture_info_string);
  }
  else if (const auto* create = std::get_if<CreateTextureTarget>(&target))
  {
    put("create_texture", create->m_texture_info_string);
  }
  else if (const auto* efb = std::get_if<EFBTarget>(&target))
  {
    put("efb", FramebufferName(EFB_DUMP_PREFIX, *efb));
  }
  else if (const auto* xfb = std::get_if<XFBTarget>(&target))
  {
    put("xfb", FramebufferName(XFB_DUMP_PREFIX, *xfb));
  }
  else if (const auto* t = std::get_if<ProjectionTarget>(&target))
  {
    // Projection targets are written with the projection kind as their type.
    json_obj.emplace("type", t->m_projection_type == ProjectionType::Orthographic ? "2d" : "3d");
    if (t->m_texture_info_string)
      json_obj.emplace("texture_filename", *t->m_texture_info_string);
  }
}

std::optional<GraphicsTargetConfig> DeserializeTargetFromConfig(const picojson::object& obj)
{
  const auto type_entry = obj.find("type");
  if (type_entry == obj.end())
  {
    ERROR_LOG_FMT(VIDEO, "Failed to load mod configuration file, option 'type' not found");
    return std::nullopt;
  }
  if (!type_entry->second.is<std::string>())
  {
    ERROR_LOG_FMT(VIDEO,
                  "Failed to load mod configuration file, option 'type' is not a string type");
    return std::nullopt;
  }

  const std::string& type = type_entry->second.get<std::string>();
  if (type == "draw_started")
    return ReadTextureTarget<DrawStartedTextureTarget>(obj);
  if (type == "load_texture")
    return ReadTextureTarget<LoadTextureTarget>(obj);
  if (type == "create_texture")
    return ReadTextureTarget<CreateTextureTarget>(obj);
  // Both framebuffer kinds skip a prefix of the EFB prefix's length.
  if (type == "efb")
    return ReadFramebufferTarget<EFBTarget>(obj, EFB_DUMP_PREFIX);
  if (type == "xfb")
    return ReadFramebufferTarget<XFBTarget>(obj, EFB_DUMP_PREFIX);
  if (type == "projection")
    return ReadProjectionTarget(obj);

  ERROR_LOG_FMT(VIDEO, "Failed to load mod configuration file, option 'type' is not a valid value");
  return std::nullopt;
}

void SerializeTargetToProfile(picojson::object*, const GraphicsTargetConfig&)
{
}

void DeserializeTargetFromProfile(const picojson::object&, GraphicsTargetConfig*)
{
}
