// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/CPUCull.h"

#include "Common/Assert.h"
#include "Common/CPUDetect.h"
#include "Common/MathUtil.h"
#include "Common/MemoryUtil.h"
#include "Core/System.h"

#include "VideoCommon/CPMemory.h"
#include "VideoCommon/VertexManagerBase.h"
#include "VideoCommon/VertexShaderManager.h"
#include "VideoCommon/XFMemory.h"

// The culling maths relies on expressions such as a*b - c*d being evaluated as two separate
// roundings, so that they come out exactly zero for degenerate triangles. Ask the compiler not
// to fuse them. (GCC has no in-source switch for this; there a fused build can only make the
// degenerate-triangle test slightly less effective.)
#ifdef _MSC_VER
#pragma fp_contract(off)
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunknown-pragmas"
#pragma STDC FP_CONTRACT OFF
#pragma GCC diagnostic pop
#endif

#define CPUCULL_FLAVOR_SCALAR 0
#define CPUCULL_FLAVOR_NEON 1
#define CPUCULL_FLAVOR_SSE 2
#define CPUCULL_FLAVOR_SSE3 3
#define CPUCULL_FLAVOR_SSE41 4
#define CPUCULL_FLAVOR_AVX 5
#define CPUCULL_FLAVOR_FMA 6

#if defined(_M_X86) || defined(_M_X86_64)
#define CPUCULL_HOST_X86 1
#include <immintrin.h>
#elif defined(_M_ARM_64)
#define CPUCULL_HOST_NEON 1
#include <arm_neon.h>
#endif

#if defined(CPUCULL_HOST_X86)
#define CPUCULL_FLAVOR CPUCULL_FLAVOR_SSE
#include "VideoCommon/CPUCullImpl.h"
#undef CPUCULL_FLAVOR
#define CPUCULL_FLAVOR CPUCULL_FLAVOR_SSE3
#include "VideoCommon/CPUCullImpl.h"
#undef CPUCULL_FLAVOR
#define CPUCULL_FLAVOR CPUCULL_FLAVOR_SSE41
#include "VideoCommon/CPUCullImpl.h"
#undef CPUCULL_FLAVOR
#define CPUCULL_FLAVOR CPUCULL_FLAVOR_AVX
#include "VideoCommon/CPUCullImpl.h"
#undef CPUCULL_FLAVOR
#define CPUCULL_FLAVOR CPUCULL_FLAVOR_FMA
#include "VideoCommon/CPUCullImpl.h"
#undef CPUCULL_FLAVOR
#elif defined(CPUCULL_HOST_NEON)
#define CPUCULL_FLAVOR CPUCULL_FLAVOR_NEON
#include "VideoCommon/CPUCullImpl.h"
#undef CPUCULL_FLAVOR
#else
#define CPUCULL_FLAVOR CPUCULL_FLAVOR_SCALAR
#include "VideoCommon/CPUCullImpl.h"
#undef CPUCULL_FLAVOR
#endif

namespace
{
#if defined(CPUCULL_HOST_X86)
// What the build baseline already guarantees, so runtime checks can be skipped.
constexpr bool BASELINE_FMA =
#if defined(__AVX__) && defined(__FMA__)
    true;
#else
    false;
#endif
constexpr bool BASELINE_AVX =
#if defined(__AVX__)
    true;
#else
    false;
#endif
constexpr bool BASELINE_SSE41 =
#if defined(__SSE4_1__)
    true;
#else
    false;
#endif
constexpr bool BASELINE_SSE3 =
#if defined(__SSE3__)
    true;
#else
    false;
#endif
#endif

template <bool HasZ, bool PerVertexMatrix>
CPUCull::TransformFunction PickTransform()
{
#if defined(CPUCULL_HOST_X86)
  if (BASELINE_FMA || (cpu_info.bAVX && cpu_info.bFMA))
    return CPUCullFma::TransformVertices<HasZ, PerVertexMatrix>;
  if (BASELINE_AVX || cpu_info.bAVX)
    return CPUCullAvx::TransformVertices<HasZ, PerVertexMatrix>;
  // The blend-based load only pays off for three-component positions with their own matrices;
  // horizontal adds help any three-component position.
  if (HasZ && PerVertexMatrix && (BASELINE_SSE41 || cpu_info.bSSE4_1))
    return CPUCullSse41::TransformVertices<HasZ, PerVertexMatrix>;
  if (HasZ && (BASELINE_SSE3 || cpu_info.bSSE3))
    return CPUCullSse3::TransformVertices<HasZ, PerVertexMatrix>;
  return CPUCullSse::TransformVertices<HasZ, PerVertexMatrix>;
#elif defined(CPUCULL_HOST_NEON)
  return CPUCullNeon::TransformVertices<HasZ, PerVertexMatrix>;
#else
  return CPUCullScalar::TransformVertices<HasZ, PerVertexMatrix>;
#endif
}

// ---------------------------------------------------------------------------------------------
// Triangle rejection, on vertices already in clip space.

using Vertex = CPUCull::TransformedVertex;

// Twice the signed screen-space area of the triangle, up to the positive factor 1/(wa*wb*wc),
// written as the three cofactor terms of the homogeneous determinant.
inline float FacingTerm(const Vertex& a, const Vertex& b, const Vertex& c)
{
  const float t_x = (a.x * c.w - c.x * a.w) * b.y;
  const float t_y = (a.y * c.x - c.y * a.x) * b.w;
  const float t_w = (a.w * c.y - c.w * a.y) * b.x;
#if defined(CPUCULL_HOST_NEON)
  // The NEON reduction also folds in a (normally zero) z term, pairwise.
  const float t_z = (a.z * 0.0f - c.z * 0.0f) * b.x;
  return (t_x + t_y) + (t_z + t_w);
#else
  return (t_x + t_y) + t_w;
#endif
}

// True if the coordinate lies past the +w clip plane. The SSE kernels compare with w <= v,
// the other flavours with v > w; the two only differ on the plane itself.
inline bool BeyondPositive(float v, float w)
{
#if defined(CPUCULL_HOST_X86)
  return w <= v;
#else
  return v > w;
#endif
}

inline bool BeyondNegative(float v, float w)
{
  return v < -w;
}

// True when all three vertices are outside the same one of the x/y clip planes.
inline bool OutsideOnePlane(const Vertex& a, const Vertex& b, const Vertex& c)
{
  const bool left = BeyondNegative(a.x, a.w) && BeyondNegative(b.x, b.w) && BeyondNegative(c.x, c.w);
  const bool bottom =
      BeyondNegative(a.y, a.w) && BeyondNegative(b.y, b.w) && BeyondNegative(c.y, c.w);
  const bool right =
      BeyondPositive(a.x, a.w) && BeyondPositive(b.x, b.w) && BeyondPositive(c.x, c.w);
  const bool top = BeyondPositive(a.y, a.w) && BeyondPositive(b.y, b.w) && BeyondPositive(c.y, c.w);
  return left || bottom || right || top;
}

template <CullMode Mode>
inline bool TriangleRejected(const Vertex& a, const Vertex& b, const Vertex& c)
{
  if constexpr (Mode == CullMode::All)
  {
    return true;
  }
  else
  {
    const float facing = FacingTerm(a, b, c);
    // Zero-area triangles are always dropped; otherwise the cull mode picks a facing.
    bool facing_rejected;
    if constexpr (Mode == CullMode::Front)
      facing_rejected = facing <= 0;
    else if constexpr (Mode == CullMode::Back)
      facing_rejected = facing >= 0;
    else
      facing_rejected = facing == 0;
    return facing_rejected || OutsideOnePlane(a, b, c);
  }
}

template <OpcodeDecoder::Primitive Primitive, CullMode Mode>
bool EveryTriangleRejected(const Vertex* v, int count)
{
  using Prim = OpcodeDecoder::Primitive;
  if constexpr (Primitive == Prim::GX_DRAW_QUADS || Primitive == Prim::GX_DRAW_QUADS_2)
  {
    // Each quad (q, q+1, q+2, q+3) splits into (q, q+1, q+2) and (q, q+2, q+3). A trailing
    // group of exactly three vertices still forms one triangle.
    int q = 0;
    for (; q + 4 <= count; q += 4)
    {
      if (!TriangleRejected<Mode>(v[q], v[q + 1], v[q + 2]) ||
          !TriangleRejected<Mode>(v[q], v[q + 2], v[q + 3]))
      {
        return false;
      }
    }
    if (count - q == 3 && !TriangleRejected<Mode>(v[q], v[q + 1], v[q + 2]))
      return false;
    return true;
  }
  else if constexpr (Primitive == Prim::GX_DRAW_TRIANGLES)
  {
    for (int t = 0; t + 3 <= count; t += 3)
    {
      if (!TriangleRejected<Mode>(v[t], v[t + 1], v[t + 2]))
        return false;
    }
    return true;
  }
  else if constexpr (Primitive == Prim::GX_DRAW_TRIANGLE_STRIP)
  {
    // Every other strip triangle has its last two vertices swapped to keep a common winding.
    for (int t = 0; t + 3 <= count; ++t)
    {
      const bool odd = (t & 1) != 0;
      const Vertex& second = odd ? v[t + 2] : v[t + 1];
      const Vertex& third = odd ? v[t + 1] : v[t + 2];
      if (!TriangleRejected<Mode>(v[t], second, third))
        return false;
    }
    return true;
  }
  else
  {
    static_assert(Primitive == Prim::GX_DRAW_TRIANGLE_FAN);
    for (int t = 2; t < count; ++t)
    {
      if (!TriangleRejected<Mode>(v[0], v[t - 1], v[t]))
        return false;
    }
    return true;
  }
}

template <OpcodeDecoder::Primitive Primitive>
constexpr std::array<CPUCull::CullFunction, 4> CullersFor()
{
  // Ordered by the CullMode values None, Back, Front, All.
  return {EveryTriangleRejected<Primitive, CullMode::None>,
          EveryTriangleRejected<Primitive, CullMode::Back>,
          EveryTriangleRejected<Primitive, CullMode::Front>,
          EveryTriangleRejected<Primitive, CullMode::All>};
}

// A positive viewport height flips the y axis, which reverses the apparent winding.
constexpr std::array<CullMode, 4> FLIPPED_MODE = {CullMode::None, CullMode::Front, CullMode::Back,
                                                  CullMode::All};
}  // namespace

CPUCull::~CPUCull() = default;

void CPUCull::AlignedFree::operator()(TransformedVertex* ptr) const
{
  Common::FreeAlignedMemory(ptr);
}

void CPUCull::Init()
{
  m_transforms[0] = PickTransform<false, false>();
  m_transforms[1] = PickTransform<false, true>();
  m_transforms[2] = PickTransform<true, false>();
  m_transforms[3] = PickTransform<true, true>();

  using Prim = OpcodeDecoder::Primitive;
  // Both quad encodings share the quad rules.
  m_culls[static_cast<size_t>(Prim::GX_DRAW_QUADS)] = CullersFor<Prim::GX_DRAW_QUADS>();
  m_culls[static_cast<size_t>(Prim::GX_DRAW_QUADS_2)] = CullersFor<Prim::GX_DRAW_QUADS>();
  m_culls[static_cast<size_t>(Prim::GX_DRAW_TRIANGLES)] = CullersFor<Prim::GX_DRAW_TRIANGLES>();
  m_culls[static_cast<size_t>(Prim::GX_DRAW_TRIANGLE_STRIP)] =
      CullersFor<Prim::GX_DRAW_TRIANGLE_STRIP>();
  m_culls[static_cast<size_t>(Prim::GX_DRAW_TRIANGLE_FAN)] =
      CullersFor<Prim::GX_DRAW_TRIANGLE_FAN>();
}

void CPUCull::EnsureCapacity(u32 count)
{
  if (count <= m_scratch_capacity) [[likely]]
    return;
  const u32 capacity = MathUtil::NextPowerOf2(count);
  m_scratch_capacity = capacity;
  m_scratch.reset(static_cast<TransformedVertex*>(
      Common::AllocateAlignedMemory(capacity * sizeof(TransformedVertex), 32)));
}

bool CPUCull::AreAllVerticesCulled(VertexLoaderBase* loader, OpcodeDecoder::Primitive primitive,
                                   const u8* src, u32 count)
{
  ASSERT_MSG(VIDEO, primitive < OpcodeDecoder::Primitive::GX_DRAW_LINES,
             "CPUCull should not be called on lines or points");

  const PortableVertexDeclaration& decl = loader->m_native_vtx_decl;
  const bool has_z = decl.position.components >= 3;
  const bool per_vertex_matrix = decl.posmtx.enable;
  EnsureCapacity(count);

  // The kernels read the projection constants, so bring them up to date first.
  Core::System& system = Core::System::GetInstance();
  system.GetVertexShaderManager().SetProjectionMatrix(system.GetXFStateManager());

  CullMode mode = bpmem.genMode.cull_mode;
  if (xfmem.viewport.ht > 0)
    mode = FLIPPED_MODE[static_cast<size_t>(mode)];

  const TransformFunction transform =
      m_transforms[(has_z ? 2u : 0u) + (per_vertex_matrix ? 1u : 0u)];
  transform(m_scratch.get(), src, static_cast<u32>(decl.stride), static_cast<int>(count));
  const CullFunction cull = m_culls[static_cast<size_t>(primitive)][static_cast<size_t>(mode)];
  return cull(m_scratch.get(), static_cast<int>(count));
}
