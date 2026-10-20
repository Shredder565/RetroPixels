// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <memory>
#include <unordered_map>

#include "Common/BitSet.h"
#include "Common/CommonTypes.h"
#include "Common/EnumMap.h"
#include "VideoCommon/CPMemory.h"

class NativeVertexFormat;
class VertexLoaderBase;
struct PortableVertexDeclaration;

namespace OpcodeDecoder
{
enum class Primitive : u8;
}

// Owns the vertex loaders and native vertex formats, picks the loader for each draw and feeds
// the converted vertices to the vertex manager.
namespace VertexLoaderManager
{
using NativeVertexFormatMap =
    std::unordered_map<PortableVertexDeclaration, std::unique_ptr<NativeVertexFormat>>;

void Init();
void Clear();

// Forces every VAT group to look its loader up again and the array bases to be resolved again.
void MarkAllDirty();

// The backend format object for decl, created on first use and shared afterwards.
NativeVertexFormat* GetOrCreateMatchingFormat(const PortableVertexDeclaration& decl);

// The format used with vertex ubershaders: decl with every attribute it lacks filled in with a
// dummy (enabled, offset 0), since ubershaders read all of them.
NativeVertexFormat* GetUberVertexFormat(const PortableVertexDeclaration& decl);

// Converts and queues `count` vertices of a draw. Returns the bytes the vertices take in the
// command stream. With IsPreprocess only the size is worked out.
template <bool IsPreprocess = false>
int RunVertices(int vtx_attr_group, OpcodeDecoder::Primitive primitive, int count, const u8* src);

namespace detail
{
// Finds or creates the loader for the current VCD and the group's VAT and records it as the
// group's loader. Use RefreshLoader, which skips this while the group is clean.
template <bool IsPreprocess = false>
VertexLoaderBase* GetOrCreateLoader(int vtx_attr_group);
}  // namespace detail

NativeVertexFormat* GetCurrentVertexFormat();

// Host pointers to the vertex array bases, as used by the loaders.
extern Common::EnumMap<u8*, CPArray::TexCoord7> cached_arraybases;
void UpdateVertexArrayPointers();

// Data the loaders keep from the end of each batch. The position rows and matrix indices are
// for the last three vertices, last vertex first (for the z-freeze slope); each vector is four
// floats so SIMD code can store it whole. The normal, tangent and binormal stand in for
// formats without them: LIT lights text that has no normals, and RS2/RS3 use emboss texgens
// without tangents (https://bugs.dolphin-emu.org/issues/13635).
extern std::array<std::array<float, 4>, 3> position_cache;
extern std::array<u32, 3> position_matrix_index_cache;
extern std::array<float, 4> normal_cache;
extern std::array<float, 4> tangent_cache;
extern std::array<float, 4> binormal_cache;

// VB_HAS_* mask of the current vertex format.
extern u32 g_current_components;

extern BitSet8 g_main_vat_dirty;
extern BitSet8 g_preprocess_vat_dirty;
// The array bases changed (main state only).
extern bool g_bases_dirty;
extern std::array<VertexLoaderBase*, CP_NUM_VAT_REG> g_main_vertex_loaders;
extern std::array<VertexLoaderBase*, CP_NUM_VAT_REG> g_preprocess_vertex_loaders;
extern bool g_needs_cp_xf_consistency_check;

// The loader for a VAT group, looked up again only when the group is dirty. For the main
// state the array pointers are brought up to date as well.
template <bool IsPreprocess = false>
VertexLoaderBase* RefreshLoader(int vtx_attr_group)
{
  const BitSet8& dirty = IsPreprocess ? g_preprocess_vat_dirty : g_main_vat_dirty;
  const auto& current = IsPreprocess ? g_preprocess_vertex_loaders : g_main_vertex_loaders;

  VertexLoaderBase* loader = current[static_cast<size_t>(vtx_attr_group)];
  if (dirty[static_cast<size_t>(vtx_attr_group)]) [[unlikely]]
    loader = detail::GetOrCreateLoader<IsPreprocess>(vtx_attr_group);

  if constexpr (!IsPreprocess)
    UpdateVertexArrayPointers();
  return loader;
}

}  // namespace VertexLoaderManager
