// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <span>
#include <string>

#include "Common/CommonTypes.h"
#include "Common/Matrix.h"
#include "VideoCommon/ConstantManager.h"
#include "VideoCommon/NativeVertexFormat.h"

class PointerWrap;
struct PortableVertexDeclaration;
class XFStateManager;

// Keeps VertexShaderConstants in sync with XF memory and the current vertex format; backend
// independent.
class alignas(16) VertexShaderManager
{
public:
  void Init();
  void DoState(PointerWrap& p);

  // Reloads the projection matrix if XF or the free-look camera changed it.
  void SetProjectionMatrix(XFStateManager& xf_state_manager);
  // Uploads every XF change recorded by xf_state_manager. `textures` are the names of the bound
  // textures, used to look up graphics-mod projection actions.
  void SetConstants(std::span<const std::string> textures, XFStateManager& xf_state_manager);

  // Transforms a model-space position (x, y, z in data[0..2]) by position matrix mtxIdx and the
  // current projection into the 4 clip-space floats at out. The projection is the one computed
  // by the last SetConstants() call, so that must have run first.
  void TransformToClipSpace(const float* data, float* out, u32 mtxIdx);

  // Whether depth has to be range-mapped in the vertex shader instead of by the viewport.
  static bool UseVertexDepthRange();

  VertexShaderConstants constants{};
  bool dirty = false;

  static DOLPHIN_FORCE_INLINE void UpdateValue(bool* dirty, u32* old_value, u32 new_value)
  {
    if (*old_value != new_value)
    {
      *old_value = new_value;
      *dirty = true;
    }
  }

  // Disabled attributes keep their previous offset. Offsets are in 32-bit words; the component
  // count goes in the upper half when requested.
  static DOLPHIN_FORCE_INLINE void UpdateOffset(bool* dirty, bool include_components,
                                                u32* old_value, const AttributeFormat& attribute)
  {
    if (!attribute.enable)
      return;
    const u32 word_offset = attribute.offset / 4;
    const u32 packed =
        include_components ? (word_offset | (attribute.components << 16)) : word_offset;
    UpdateValue(dirty, old_value, packed);
  }

  template <size_t N>
  static DOLPHIN_FORCE_INLINE void UpdateOffsets(bool* dirty, bool include_components,
                                                 std::array<u32, N>* old_value,
                                                 const std::array<AttributeFormat, N>& attribute)
  {
    for (size_t n = 0; n < N; ++n)
      UpdateOffset(dirty, include_components, &(*old_value)[n], attribute[n]);
  }

  // Records the layout the vertex ubershader fetches from.
  DOLPHIN_FORCE_INLINE void SetVertexFormat(u32 components, const PortableVertexDeclaration& format)
  {
    UpdateValue(&dirty, &constants.components, components);
    UpdateValue(&dirty, &constants.vertex_stride, format.stride / 4);
    UpdateOffset(&dirty, true, &constants.vertex_offset_position, format.position);
    UpdateOffset(&dirty, false, &constants.vertex_offset_posmtx, format.posmtx);
    UpdateOffsets(&dirty, true, &constants.vertex_offset_texcoords, format.texcoords);
    UpdateOffsets(&dirty, false, &constants.vertex_offset_colors, format.colors);
    UpdateOffsets(&dirty, false, &constants.vertex_offset_normals, format.normals);
  }

private:
  // The projection as last loaded, without the free-look view (row major).
  alignas(16) std::array<float, 16> m_projection_matrix;

  // A graphics mod changed the projection last time, so it must be reloaded once more.
  bool m_projection_graphics_mod_change = false;

  Common::Matrix44 LoadProjectionMatrix();
};
