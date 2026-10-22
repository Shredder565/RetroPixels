// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/VertexLoaderManager.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include <fmt/format.h>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/EnumMap.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"

#include "Core/DolphinAnalytics.h"
#include "Core/HW/Memmap.h"
#include "Core/System.h"

#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/BPMemory.h"
#include "VideoCommon/CPMemory.h"
#include "VideoCommon/DataReader.h"
#include "VideoCommon/NativeVertexFormat.h"
#include "VideoCommon/OpcodeDecoding.h"
#include "VideoCommon/Statistics.h"
#include "VideoCommon/VertexLoaderBase.h"
#include "VideoCommon/VertexManagerBase.h"
#include "VideoCommon/VertexShaderManager.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/XFMemory.h"

namespace VertexLoaderManager
{
std::array<u32, 3> position_matrix_index_cache;
alignas(16) std::array<std::array<float, 4>, 3> position_cache;
alignas(16) std::array<float, 4> normal_cache;
alignas(16) std::array<float, 4> tangent_cache;
alignas(16) std::array<float, 4> binormal_cache;

Common::EnumMap<u8*, CPArray::TexCoord7> cached_arraybases;

u32 g_current_components;
BitSet8 g_main_vat_dirty;
BitSet8 g_preprocess_vat_dirty;
bool g_bases_dirty;
std::array<VertexLoaderBase*, CP_NUM_VAT_REG> g_main_vertex_loaders;
std::array<VertexLoaderBase*, CP_NUM_VAT_REG> g_preprocess_vertex_loaders;
bool g_needs_cp_xf_consistency_check;

namespace
{
NativeVertexFormatMap s_formats;
NativeVertexFormat* s_current_format;

// Loaders are shared between the GPU thread and the preprocessing (CPU) thread.
std::mutex s_loaders_mutex;
std::unordered_map<VertexLoaderUID, std::unique_ptr<VertexLoaderBase>> s_loaders;

// A draw longer than this is cut into pieces for the primitives that can be cut anywhere on a
// primitive boundary; 16380 vertices divide into whole triangles and whole quads.
constexpr int SPLIT_RUN = 16380;

bool CanSplit(OpcodeDecoder::Primitive primitive)
{
  using OpcodeDecoder::Primitive;
  return primitive == Primitive::GX_DRAW_QUADS || primitive == Primitive::GX_DRAW_QUADS_2 ||
         primitive == Primitive::GX_DRAW_TRIANGLES || primitive == Primitive::GX_DRAW_LINES ||
         primitive == Primitive::GX_DRAW_POINTS;
}

// Resolves one array base if the current VCD reads the array through indices. A base outside
// emulated memory (Burnout 2 sets some for arrays it never uses) keeps the old pointer.
void ResolveArray(Memory::MemoryManager& memory, CPArray array, VertexComponentFormat usage)
{
  if (!IsIndexed(usage))
    return;
  u8* const host = memory.GetPointerForRange(g_main_cp_state.array_bases[array], sizeof(u32));
  if (host != nullptr)
    cached_arraybases[array] = host;
}

std::optional<u32> XFNormalCount(NormalCount count)
{
  switch (count)
  {
  case NormalCount::None:
    return 0;
  case NormalCount::Normal:
    return 1;
  case NormalCount::NormalTangentBinormal:
  case NormalCount::Invalid:  // https://bugs.dolphin-emu.org/issues/13070
    return 3;
  }
  return std::nullopt;
}

// Reports CP and XF settings that disagree, and VAT formats that do not exist. Only reports;
// the draw goes ahead.
void CheckCPConfiguration(int vtx_attr_group)
{
  const CPState& cp = g_main_cp_state;
  const VAT& vat = cp.vtx_attr[static_cast<size_t>(vtx_attr_group)];
  const auto is_present = [](VertexComponentFormat f) {
    return f != VertexComponentFormat::NotPresent;
  };

  const u32 cp_colors =
      static_cast<u32>(std::count_if(cp.vtx_desc.low.Color.begin(), cp.vtx_desc.low.Color.end(),
                                     is_present));
  const u32 cp_tex_coords = static_cast<u32>(std::count_if(
      cp.vtx_desc.high.TexCoord.begin(), cp.vtx_desc.high.TexCoord.end(), is_present));
  u32 cp_normals = 0;
  if (cp.vtx_desc.low.Normal != VertexComponentFormat::NotPresent)
    cp_normals = vat.g0.NormalElements == NormalComponentCount::NTB ? 3 : 1;
  const std::optional<u32> xf_normals = XFNormalCount(xfmem.invtxspec.numnormals);

  const bool colors_differ = cp_colors != xfmem.invtxspec.numcolors;
  const bool normals_differ = cp_normals != xf_normals;
  const bool tex_coords_differ = cp_tex_coords != xfmem.invtxspec.numtextures;
  if (colors_differ || normals_differ || tex_coords_differ) [[unlikely]]
  {
    PanicAlertFmt("Mismatched configuration between CP and XF stages - {}/{} colors, {}/{} "
                  "normals, {}/{} texture coordinates. Please report on the issue tracker.\n\n"
                  "VCD: {:08x} {:08x}\nVAT {}: {:08x} {:08x} {:08x}\nXF vertex spec: {:08x}",
                  cp_colors, xfmem.invtxspec.numcolors, cp_normals,
                  xf_normals.has_value() ? fmt::to_string(*xf_normals) : "invalid",
                  cp_tex_coords, xfmem.invtxspec.numtextures, cp.vtx_desc.low.Hex,
                  cp.vtx_desc.high.Hex, vtx_attr_group, vat.g0.Hex, vat.g1.Hex, vat.g2.Hex,
                  xfmem.invtxspec.hex);

    // Reported so the games that do this can be found; real hardware seems to hang.
    if (colors_differ)
      DolphinAnalytics::Instance().ReportGameQuirk(GameQuirk::MismatchedGPUColorsBetweenCPAndXF);
    if (normals_differ)
      DolphinAnalytics::Instance().ReportGameQuirk(GameQuirk::MismatchedGPUNormalsBetweenCPAndXF);
    if (tex_coords_differ)
    {
      DolphinAnalytics::Instance().ReportGameQuirk(
          GameQuirk::MismatchedGPUTexCoordsBetweenCPAndXF);
    }
  }

  if (cp.matrix_index_a.Hex != xfmem.MatrixIndexA.Hex ||
      cp.matrix_index_b.Hex != xfmem.MatrixIndexB.Hex) [[unlikely]]
  {
    WARN_LOG_FMT(VIDEO,
                 "Mismatched matrix index configuration between CP and XF stages - "
                 "index A: {:08x}/{:08x}, index B {:08x}/{:08x}.",
                 cp.matrix_index_a.Hex, xfmem.MatrixIndexA.Hex, cp.matrix_index_b.Hex,
                 xfmem.MatrixIndexB.Hex);
    DolphinAnalytics::Instance().ReportGameQuirk(
        GameQuirk::MismatchedGPUMatrixIndicesBetweenCPAndXF);
  }

  if (vat.g0.PosFormat >= ComponentFormat::InvalidFloat5)
  {
    WARN_LOG_FMT(VIDEO, "Invalid position format {} for VAT {} - {:08x} {:08x} {:08x}",
                 vat.g0.PosFormat, vtx_attr_group, vat.g0.Hex, vat.g1.Hex, vat.g2.Hex);
    DolphinAnalytics::Instance().ReportGameQuirk(GameQuirk::InvalidPositionComponentFormat);
  }
  if (vat.g0.NormalFormat >= ComponentFormat::InvalidFloat5)
  {
    WARN_LOG_FMT(VIDEO, "Invalid normal format {} for VAT {} - {:08x} {:08x} {:08x}",
                 vat.g0.NormalFormat, vtx_attr_group, vat.g0.Hex, vat.g1.Hex, vat.g2.Hex);
    DolphinAnalytics::Instance().ReportGameQuirk(GameQuirk::InvalidNormalComponentFormat);
  }
  for (size_t t = 0; t < 8; t++)
  {
    if (vat.GetTexFormat(t) < ComponentFormat::InvalidFloat5)
      continue;
    WARN_LOG_FMT(VIDEO,
                 "Invalid texture coordinate {} format {} for VAT {} - {:08x} {:08x} {:08x}", t,
                 vat.GetTexFormat(t), vtx_attr_group, vat.g0.Hex, vat.g1.Hex, vat.g2.Hex);
    DolphinAnalytics::Instance().ReportGameQuirk(
        GameQuirk::InvalidTextureCoordinateComponentFormat);
  }
  for (size_t c = 0; c < 2; c++)
  {
    if (vat.GetColorFormat(c) <= ColorFormat::RGBA8888)
      continue;
    WARN_LOG_FMT(VIDEO, "Invalid color {} format {} for VAT {} - {:08x} {:08x} {:08x}", c,
                 vat.GetColorFormat(c), vtx_attr_group, vat.g0.Hex, vat.g1.Hex, vat.g2.Hex);
    DolphinAnalytics::Instance().ReportGameQuirk(GameQuirk::InvalidColorComponentFormat);
  }
}

// Sets a dummy attribute: enabled, at offset 0.
void FillDummy(AttributeFormat& attr, ComponentFormat type, int components, bool integer)
{
  attr.type = type;
  attr.components = components;
  attr.offset = 0;
  attr.enable = true;
  attr.integer = integer;
}

// Copies attr when it is enabled; otherwise sets the dummy.
void CopyOrDummy(AttributeFormat& out, const AttributeFormat& attr, ComponentFormat type,
                 int components, bool integer)
{
  if (attr.enable)
  {
    out.type = attr.type;
    out.components = attr.components;
    out.offset = attr.offset;
    out.enable = attr.enable;
    out.integer = attr.integer;
  }
  else
  {
    FillDummy(out, type, components, integer);
  }
}
}  // namespace

void Init()
{
  MarkAllDirty();
  g_main_vertex_loaders.fill(nullptr);
  g_preprocess_vertex_loaders.fill(nullptr);
  SETSTAT(g_stats.num_vertex_loaders, 0);
}

void Clear()
{
  std::lock_guard<std::mutex> guard(s_loaders_mutex);
  s_loaders.clear();
  s_formats.clear();
}

void UpdateVertexArrayPointers()
{
  if (!g_bases_dirty) [[likely]]
    return;

  // Only arrays 0-11 feed the loaders (12-15 are for indexed XF loads), and only those the VCD
  // reads through indices are resolved.
  auto& memory = Core::System::GetInstance().GetMemory();
  const TVtxDesc& vcd = g_main_cp_state.vtx_desc;
  ResolveArray(memory, CPArray::Position, vcd.low.Position);
  ResolveArray(memory, CPArray::Normal, vcd.low.Normal);
  for (u8 c = 0; c < 2; c++)
    ResolveArray(memory, CPArray::Color0 + c, vcd.low.Color[c]);
  for (u8 t = 0; t < 8; t++)
    ResolveArray(memory, CPArray::TexCoord0 + t, vcd.high.TexCoord[t]);

  g_bases_dirty = false;
}

void MarkAllDirty()
{
  g_bases_dirty = true;
  g_main_vat_dirty = BitSet8::AllTrue(8);
  g_preprocess_vat_dirty = BitSet8::AllTrue(8);
  g_needs_cp_xf_consistency_check = true;
}

NativeVertexFormat* GetOrCreateMatchingFormat(const PortableVertexDeclaration& decl)
{
  auto found = s_formats.find(decl);
  if (found == s_formats.end())
    found = s_formats.emplace(decl, g_gfx->CreateNativeVertexFormat(decl)).first;
  return found->second.get();
}

NativeVertexFormat* GetUberVertexFormat(const PortableVertexDeclaration& decl)
{
  // Built from a zeroed declaration so that padding cannot make equal layouts compare unequal.
  PortableVertexDeclaration uber;
  static_assert(std::is_trivially_copyable_v<PortableVertexDeclaration>);
  std::memset(static_cast<void*>(&uber), 0, sizeof(uber));
  uber.stride = decl.stride;

  CopyOrDummy(uber.position, decl.position, ComponentFormat::Float, 1, false);
  for (size_t i = 0; i < uber.normals.size(); i++)
    CopyOrDummy(uber.normals[i], decl.normals[i], ComponentFormat::Float, 1, false);
  for (size_t i = 0; i < uber.colors.size(); i++)
    CopyOrDummy(uber.colors[i], decl.colors[i], ComponentFormat::UByte, 4, false);
  for (size_t i = 0; i < uber.texcoords.size(); i++)
    CopyOrDummy(uber.texcoords[i], decl.texcoords[i], ComponentFormat::Float, 1, false);
  CopyOrDummy(uber.posmtx, decl.posmtx, ComponentFormat::UByte, 1, true);

  return GetOrCreateMatchingFormat(uber);
}

namespace detail
{
template <bool IsPreprocess>
VertexLoaderBase* GetOrCreateLoader(int vtx_attr_group)
{
  const CPState& state = IsPreprocess ? g_preprocess_cp_state : g_main_cp_state;
  const size_t group = static_cast<size_t>(vtx_attr_group);
  const VAT& vat = state.vtx_attr[group];

  std::lock_guard<std::mutex> guard(s_loaders_mutex);
  const VertexLoaderUID key(state.vtx_desc, vat);
  VertexLoaderBase* loader;
  auto found = s_loaders.find(key);
  if (found != s_loaders.end())
  {
    loader = found->second.get();
  }
  else
  {
    loader = s_loaders.try_emplace(key, VertexLoaderBase::CreateVertexLoader(state.vtx_desc, vat))
                 .first->second.get();
    INCSTAT(g_stats.num_vertex_loaders);
  }

  // Backend objects may only be made on the GPU thread, so preprocessing leaves this alone.
  if constexpr (!IsPreprocess)
  {
    if (loader->m_native_vertex_format == nullptr)
      loader->m_native_vertex_format = GetOrCreateMatchingFormat(loader->m_native_vtx_decl);
  }

  (IsPreprocess ? g_preprocess_vertex_loaders : g_main_vertex_loaders)[group] = loader;
  (IsPreprocess ? g_preprocess_vat_dirty : g_main_vat_dirty)[group] = false;
  return loader;
}

template VertexLoaderBase* GetOrCreateLoader<false>(int vtx_attr_group);
template VertexLoaderBase* GetOrCreateLoader<true>(int vtx_attr_group);
}  // namespace detail

namespace
{
// The main-thread half of RunVertices: converts the draw into the vertex manager's buffers.
void LoadDraw(VertexLoaderBase* loader, int vtx_attr_group, OpcodeDecoder::Primitive primitive,
              int count, const u8* src)
{
  if (g_needs_cp_xf_consistency_check) [[unlikely]]
  {
    CheckCPConfiguration(vtx_attr_group);
    g_needs_cp_xf_consistency_check = false;
  }

  // A different host layout or component set ends the current batch.
  if (loader->m_native_vertex_format != s_current_format ||
      loader->m_native_components != g_current_components) [[unlikely]]
  {
    g_vertex_manager->Flush();
    s_current_format = loader->m_native_vertex_format;
    g_current_components = loader->m_native_components;
    Core::System::GetInstance().GetVertexShaderManager().SetVertexFormat(
        loader->m_native_components, loader->m_native_vertex_format->GetVertexDeclaration());
  }

  // Triangles and quads with culling set to "all" are still loaded (the z-freeze slope needs
  // their positions) but never drawn. CPU culling may do the same for a batch whose vertices
  // all fall outside; that only helps when it would start a new batch.
  const bool before_lines = primitive < OpcodeDecoder::Primitive::GX_DRAW_LINES;
  bool try_cpu_cull =
      g_ActiveConfig.bCPUCull && before_lines && !g_vertex_manager->HasSendableVertices();
  const bool cull_all = bpmem.genMode.cull_mode == CullMode::All && before_lines;

  const int stride = loader->m_native_vtx_decl.stride;
  const u32 ustride = static_cast<u32>(stride);
  const bool splittable = CanSplit(primitive);
  int left = count;
  do
  {
    const int run = (splittable && left > SPLIT_RUN) ? SPLIT_RUN : left;
    left -= run;

    DataReader out = g_vertex_manager->PrepareForAdditionalData(
        primitive, static_cast<u32>(run), ustride, cull_all || try_cpu_cull);
    const int loaded = loader->RunVertices(src, out.GetPointer(), run);
    const u32 uloaded = static_cast<u32>(loaded);
    // Steps a whole split run, even after the last (shorter) piece.
    src += static_cast<size_t>(loader->m_vertex_size) * SPLIT_RUN;

    if (try_cpu_cull && !cull_all)
    {
      if (!g_vertex_manager->AreAllVerticesCulled(loader, primitive, out.GetPointer(), uloaded))
      {
        DataReader drawn = g_vertex_manager->DisableCullAll(ustride);
        std::memmove(drawn.GetPointer(), out.GetPointer(),
                     static_cast<size_t>(loaded) * static_cast<size_t>(stride));
        try_cpu_cull = false;
      }
    }

    g_vertex_manager->AddIndices(primitive, uloaded);
    g_vertex_manager->FlushData(uloaded, ustride);
    ADDSTAT(g_stats.this_frame.num_prims, loaded);
  } while (left != 0);

  INCSTAT(g_stats.this_frame.num_primitive_joins);
}
}  // namespace

template <bool IsPreprocess>
int RunVertices(int vtx_attr_group, OpcodeDecoder::Primitive primitive, int count, const u8* src)
{
  if (count == 0) [[unlikely]]
    return 0;
  ASSERT(count > 0);

  VertexLoaderBase* const loader = RefreshLoader<IsPreprocess>(vtx_attr_group);
  const int stream_bytes = count * static_cast<int>(loader->m_vertex_size);
  if constexpr (!IsPreprocess)
    LoadDraw(loader, vtx_attr_group, primitive, count, src);
  return stream_bytes;
}

template int RunVertices<false>(int vtx_attr_group, OpcodeDecoder::Primitive primitive, int count,
                                const u8* src);
template int RunVertices<true>(int vtx_attr_group, OpcodeDecoder::Primitive primitive, int count,
                               const u8* src);

NativeVertexFormat* GetCurrentVertexFormat()
{
  return s_current_format;
}

}  // namespace VertexLoaderManager
