// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// Vertex position transform kernels for CPUCull.cpp, one instruction set at a time.
//
// This file has no include guard on purpose: CPUCull.cpp includes it once per kernel flavour
// with CPUCULL_FLAVOR set to one of the CPUCULL_FLAVOR_* values, and each inclusion produces a
// namespace of the same templates compiled for that flavour. On GCC/Clang every function is
// tagged with a target attribute so the wider instructions are usable without raising the
// baseline of the whole build.
//
// The order of floating point operations in each flavour is fixed: the culling decision on a
// degenerate triangle depends on rounding, so a given flavour must always produce the same bits.

#if !defined(CPUCULL_FLAVOR)
#error CPUCullImpl.h is only meant to be included from CPUCull.cpp
#endif

// ---------------------------------------------------------------------------------------------
// Per-flavour configuration
#if CPUCULL_FLAVOR == CPUCULL_FLAVOR_SCALAR
#define CPUCULL_NS CPUCullScalar
#elif CPUCULL_FLAVOR == CPUCULL_FLAVOR_NEON
#define CPUCULL_NS CPUCullNeon
#define CPUCULL_NEON 1
#define CPUCULL_FUSED 1
#elif CPUCULL_FLAVOR == CPUCULL_FLAVOR_SSE
#define CPUCULL_NS CPUCullSse
#define CPUCULL_X86 1
#elif CPUCULL_FLAVOR == CPUCULL_FLAVOR_SSE3
#define CPUCULL_NS CPUCullSse3
#define CPUCULL_X86 1
#define CPUCULL_HADD 1
#if defined(__GNUC__) && !defined(__SSE3__)
#define CPUCULL_TARGET __attribute__((target("sse3")))
#endif
#elif CPUCULL_FLAVOR == CPUCULL_FLAVOR_SSE41
#define CPUCULL_NS CPUCullSse41
#define CPUCULL_X86 1
#define CPUCULL_HADD 1
#define CPUCULL_BLEND 1
#if defined(__GNUC__) && !defined(__SSE4_1__)
#define CPUCULL_TARGET __attribute__((target("sse4.1")))
#endif
#elif CPUCULL_FLAVOR == CPUCULL_FLAVOR_AVX
#define CPUCULL_NS CPUCullAvx
#define CPUCULL_X86 1
#define CPUCULL_HADD 1
#define CPUCULL_BLEND 1
#define CPUCULL_PAIRS 1
#if defined(__GNUC__) && !defined(__AVX__)
#define CPUCULL_TARGET __attribute__((target("avx")))
#endif
#elif CPUCULL_FLAVOR == CPUCULL_FLAVOR_FMA
#define CPUCULL_NS CPUCullFma
#define CPUCULL_X86 1
#define CPUCULL_HADD 1
#define CPUCULL_BLEND 1
#define CPUCULL_PAIRS 1
#define CPUCULL_FUSED 1
#if defined(__GNUC__) && !(defined(__AVX__) && defined(__FMA__))
#define CPUCULL_TARGET __attribute__((target("avx,fma")))
#endif
#else
#error Unknown CPUCULL_FLAVOR
#endif

#ifndef CPUCULL_TARGET
#define CPUCULL_TARGET
#endif
#define CPUCULL_INLINE CPUCULL_TARGET DOLPHIN_FORCE_INLINE static

namespace CPUCULL_NS
{
// ---------------------------------------------------------------------------------------------
// Four-wide float vector primitives

#if defined(CPUCULL_X86)
using F4 = __m128;

CPUCULL_INLINE F4 Make(float a, float b, float c, float d)
{
  return _mm_setr_ps(a, b, c, d);
}
// Four floats from unaligned memory.
CPUCULL_INLINE F4 Load4(const float* p)
{
  return _mm_loadu_ps(p);
}
// Two floats from memory in the low lanes; the high lanes come from `high`.
CPUCULL_INLINE F4 Load2(const float* p, F4 high)
{
  return _mm_loadl_pi(high, reinterpret_cast<const __m64*>(p));
}
template <int Lane>
CPUCULL_INLINE F4 Splat(F4 v)
{
  return _mm_shuffle_ps(v, v, _MM_SHUFFLE(Lane, Lane, Lane, Lane));
}
CPUCULL_INLINE F4 Mul(F4 a, F4 b)
{
  return _mm_mul_ps(a, b);
}
CPUCULL_INLINE F4 Add(F4 a, F4 b)
{
  return _mm_add_ps(a, b);
}
// acc + a * b, rounded once when the flavour has fused multiply-add.
CPUCULL_INLINE F4 MulAcc(F4 acc, F4 a, F4 b)
{
#if defined(CPUCULL_FUSED)
  return _mm_fmadd_ps(a, b, acc);
#else
  return _mm_add_ps(acc, _mm_mul_ps(a, b));
#endif
}
CPUCULL_INLINE void Store(CPUCull::TransformedVertex* out, F4 v)
{
  _mm_store_ps(&out->x, v);
}

#elif defined(CPUCULL_NEON)
using F4 = float32x4_t;

CPUCULL_INLINE F4 Make(float a, float b, float c, float d)
{
  const float lanes[4] = {a, b, c, d};
  return vld1q_f32(lanes);
}
CPUCULL_INLINE F4 Load4(const float* p)
{
  return vld1q_f32(p);
}
CPUCULL_INLINE F4 Load2(const float* p, F4 high)
{
  return vcombine_f32(vld1_f32(p), vget_high_f32(high));
}
template <int Lane>
CPUCULL_INLINE F4 Splat(F4 v)
{
  return vdupq_laneq_f32(v, Lane);
}
CPUCULL_INLINE F4 Mul(F4 a, F4 b)
{
  return vmulq_f32(a, b);
}
CPUCULL_INLINE F4 Add(F4 a, F4 b)
{
  return vaddq_f32(a, b);
}
CPUCULL_INLINE F4 MulAcc(F4 acc, F4 a, F4 b)
{
  return vfmaq_f32(acc, a, b);
}
CPUCULL_INLINE void Store(CPUCull::TransformedVertex* out, F4 v)
{
  vst1q_f32(&out->x, v);
}

#else  // portable scalar
struct alignas(16) F4
{
  float x, y, z, w;
};

CPUCULL_INLINE F4 Make(float a, float b, float c, float d)
{
  return F4{a, b, c, d};
}
CPUCULL_INLINE F4 Load4(const float* p)
{
  return F4{p[0], p[1], p[2], p[3]};
}
CPUCULL_INLINE F4 Load2(const float* p, F4 high)
{
  return F4{p[0], p[1], high.z, high.w};
}
template <int Lane>
CPUCULL_INLINE F4 Splat(F4 v)
{
  const float s = Lane == 0 ? v.x : Lane == 1 ? v.y : Lane == 2 ? v.z : v.w;
  return F4{s, s, s, s};
}
CPUCULL_INLINE F4 Mul(F4 a, F4 b)
{
  return F4{a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w};
}
CPUCULL_INLINE F4 Add(F4 a, F4 b)
{
  return F4{a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
}
CPUCULL_INLINE F4 MulAcc(F4 acc, F4 a, F4 b)
{
  return Add(acc, Mul(a, b));
}
CPUCULL_INLINE void Store(CPUCull::TransformedVertex* out, F4 v)
{
  *out = CPUCull::TransformedVertex{v.x, v.y, v.z, v.w};
}
#endif

static_assert(sizeof(F4) == 16);

// ---------------------------------------------------------------------------------------------
// Matrices

// A matrix held as its columns; column j carries the coefficients applied to input lane j.
struct Columns
{
  F4 c0, c1, c2, c3;
};

// Columns of a row-major 4x4 matrix.
CPUCULL_INLINE Columns ColumnsOf4x4(const float* m)
{
  return Columns{Make(m[0], m[4], m[8], m[12]), Make(m[1], m[5], m[9], m[13]),
                 Make(m[2], m[6], m[10], m[14]), Make(m[3], m[7], m[11], m[15])};
}

// Columns of a row-major 3x4 affine matrix extended with the row (0, 0, 0, 1).
CPUCULL_INLINE Columns ColumnsOf3x4(const float* m)
{
  return Columns{Make(m[0], m[4], m[8], 0.0f), Make(m[1], m[5], m[9], 0.0f),
                 Make(m[2], m[6], m[10], 0.0f), Make(m[3], m[7], m[11], 1.0f)};
}

// out = P * v, accumulated lane by lane starting from the x column.
CPUCULL_INLINE F4 Project(F4 v, const Columns& p)
{
  F4 out = Mul(Splat<0>(v), p.c0);
  out = MulAcc(out, Splat<1>(v), p.c1);
  out = MulAcc(out, Splat<2>(v), p.c2);
  out = MulAcc(out, Splat<3>(v), p.c3);
  return out;
}

// out = M * (x, y, z, 1) using the column form; z is skipped for two-component positions.
template <bool HasZ>
CPUCULL_INLINE F4 ModelFromColumns(F4 v, const Columns& m)
{
#if defined(CPUCULL_X86) || defined(CPUCULL_NEON)
  F4 out = m.c3;
  out = MulAcc(out, Splat<0>(v), m.c0);
  out = MulAcc(out, Splat<1>(v), m.c1);
  if constexpr (HasZ)
    out = MulAcc(out, Splat<2>(v), m.c2);
  return out;
#else
  // The portable flavour sums the x and y terms before adding the translation and leaves w at
  // the translation column's 1.
  F4 out = m.c3;
  out.x += v.x * m.c0.x + v.y * m.c1.x;
  out.y += v.x * m.c0.y + v.y * m.c1.y;
  out.z += v.x * m.c0.z + v.y * m.c1.z;
  if constexpr (HasZ)
  {
    out.x += v.z * m.c2.x;
    out.y += v.z * m.c2.y;
    out.z += v.z * m.c2.z;
  }
  return out;
#endif
}

#if !defined(CPUCULL_NEON)
// out = M * v as three dot products against the matrix rows (v.w must be 1); w becomes 1.
CPUCULL_INLINE F4 ModelFromRows(F4 v, const float* rows)
{
#if defined(CPUCULL_X86)
  const F4 p0 = Mul(v, Load4(rows + 0));
  const F4 p1 = Mul(v, Load4(rows + 4));
  const F4 p2 = Mul(v, Load4(rows + 8));
  const F4 p3 = Make(0.0f, 0.0f, 0.0f, 1.0f);
#if defined(CPUCULL_HADD)
  // Per row: (x + y) + (z + w).
  return _mm_hadd_ps(_mm_hadd_ps(p0, p1), _mm_hadd_ps(p2, p3));
#else
  // Per row: (x + z) + (y + w).
  F4 t0 = p0, t1 = p1, t2 = p2, t3 = p3;
  _MM_TRANSPOSE4_PS(t0, t1, t2, t3);
  return Add(Add(t0, t2), Add(t1, t3));
#endif
#else
  // Per row: ((x + y) + z) + w, left to right.
  const auto dot = [&v](const float* r) {
    return v.x * r[0] + v.y * r[1] + v.z * r[2] + v.w * r[3];
  };
  return F4{dot(rows + 0), dot(rows + 4), dot(rows + 8), 1.0f};
#endif
}
#endif

// Position data starts the vertex, preceded by one u32 of matrix index when per-vertex matrices
// are enabled. Only the low six bits of that index are used.
CPUCULL_INLINE const float* VertexMatrix(const u8* vertex)
{
  return &xfmem.posMatrices[(vertex[0] & 0x3fu) * 4];
}

template <bool HasZ, bool PerVertexMatrix>
CPUCULL_INLINE F4 TransformOne(const u8* vertex, const Columns& model, const Columns& proj)
{
  if constexpr (!PerVertexMatrix)
  {
    const float* pos = reinterpret_cast<const float*>(vertex);
    // For three components the fourth lane picks up whatever follows; it is never used.
    const F4 v = HasZ ? Load4(pos) : Load2(pos, Make(0.0f, 0.0f, 0.0f, 0.0f));
    return Project(ModelFromColumns<HasZ>(v, model), proj);
  }
  else
  {
    const float* rows = VertexMatrix(vertex);
    const float* pos = reinterpret_cast<const float*>(vertex + sizeof(u32));
#if defined(CPUCULL_NEON)
    // NEON reuses the column form with the vertex's own matrix.
    const F4 v = HasZ ? Load4(pos) : Load2(pos, Make(0.0f, 0.0f, 0.0f, 0.0f));
    return Project(ModelFromColumns<HasZ>(v, ColumnsOf3x4(rows)), proj);
#else
    F4 v;
    if constexpr (HasZ)
    {
#if defined(CPUCULL_X86) && defined(CPUCULL_BLEND)
      v = _mm_blend_ps(Load4(pos), _mm_set1_ps(1.0f), 0x8);
#elif defined(CPUCULL_X86)
      const F4 keep_xyz = _mm_castsi128_ps(_mm_setr_epi32(-1, -1, -1, 0));
      v = _mm_or_ps(_mm_and_ps(Load4(pos), keep_xyz), Make(0.0f, 0.0f, 0.0f, 1.0f));
#else
      v = Make(pos[0], pos[1], pos[2], 1.0f);
#endif
    }
    else
    {
      v = Load2(pos, Make(0.0f, 0.0f, 0.0f, 1.0f));
    }
    return Project(ModelFromRows(v, rows), proj);
#endif
  }
}

// ---------------------------------------------------------------------------------------------
// Two vertices per 256-bit register (AVX flavours). Each 128-bit half follows exactly the same
// sequence of operations as the four-wide code above.
#if defined(CPUCULL_PAIRS)
using F8 = __m256;

CPUCULL_INLINE F8 Join(F4 low, F4 high)
{
  return _mm256_insertf128_ps(_mm256_castps128_ps256(low), high, 1);
}
CPUCULL_INLINE F8 Both(F4 v)
{
  return Join(v, v);
}
template <int Lane>
CPUCULL_INLINE F8 Splat8(F8 v)
{
  return _mm256_shuffle_ps(v, v, _MM_SHUFFLE(Lane, Lane, Lane, Lane));
}
CPUCULL_INLINE F8 MulAcc8(F8 acc, F8 a, F8 b)
{
#if defined(CPUCULL_FUSED)
  return _mm256_fmadd_ps(a, b, acc);
#else
  return _mm256_add_ps(acc, _mm256_mul_ps(a, b));
#endif
}

struct Columns8
{
  F8 c0, c1, c2, c3;
};

CPUCULL_INLINE Columns8 Widen(const Columns& m)
{
  return Columns8{Both(m.c0), Both(m.c1), Both(m.c2), Both(m.c3)};
}

CPUCULL_INLINE F8 Project8(F8 v, const Columns8& p)
{
  F8 out = _mm256_mul_ps(Splat8<0>(v), p.c0);
  out = MulAcc8(out, Splat8<1>(v), p.c1);
  out = MulAcc8(out, Splat8<2>(v), p.c2);
  out = MulAcc8(out, Splat8<3>(v), p.c3);
  return out;
}

template <bool HasZ, bool PerVertexMatrix>
CPUCULL_INLINE F8 TransformTwo(const u8* first, const u8* second, const Columns8& model,
                               const Columns8& proj)
{
  if constexpr (!PerVertexMatrix)
  {
    const float* a = reinterpret_cast<const float*>(first);
    const float* b = reinterpret_cast<const float*>(second);
    const F4 zero = _mm_setzero_ps();
    const F8 v = HasZ ? Join(Load4(a), Load4(b)) : Join(Load2(a, zero), Load2(b, zero));
    F8 world = model.c3;
    world = MulAcc8(world, Splat8<0>(v), model.c0);
    world = MulAcc8(world, Splat8<1>(v), model.c1);
    if constexpr (HasZ)
      world = MulAcc8(world, Splat8<2>(v), model.c2);
    return Project8(world, proj);
  }
  else
  {
    const float* ra = VertexMatrix(first);
    const float* rb = VertexMatrix(second);
    const float* a = reinterpret_cast<const float*>(first + sizeof(u32));
    const float* b = reinterpret_cast<const float*>(second + sizeof(u32));
    F8 v;
    if constexpr (HasZ)
      v = _mm256_blend_ps(Join(Load4(a), Load4(b)), _mm256_set1_ps(1.0f), 0x88);
    else
    {
      const F4 zw = _mm_setr_ps(0.0f, 0.0f, 0.0f, 1.0f);
      v = Join(Load2(a, zw), Load2(b, zw));
    }
    const F8 p0 = _mm256_mul_ps(v, Join(Load4(ra + 0), Load4(rb + 0)));
    const F8 p1 = _mm256_mul_ps(v, Join(Load4(ra + 4), Load4(rb + 4)));
    const F8 p2 = _mm256_mul_ps(v, Join(Load4(ra + 8), Load4(rb + 8)));
    const F8 p3 = _mm256_setr_ps(0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f);
    const F8 world = _mm256_hadd_ps(_mm256_hadd_ps(p0, p1), _mm256_hadd_ps(p2, p3));
    return Project8(world, proj);
  }
}
#endif

// ---------------------------------------------------------------------------------------------
// Entry point: transforms `count` vertices spaced `stride` bytes apart into `output`, which
// must be 32-byte aligned.
template <bool HasZ, bool PerVertexMatrix>
CPUCULL_TARGET static void TransformVertices(void* output, const void* vertices, u32 stride,
                                             int count)
{
  const VertexShaderManager& vsm = Core::System::GetInstance().GetVertexShaderManager();
  const Columns proj = ColumnsOf4x4(vsm.constants.projection[0].data());
  const u32 matrix = g_main_cp_state.matrix_index_a.PosNormalMtxIdx & 0x3f;
  const Columns model = ColumnsOf3x4(&xfmem.posMatrices[matrix * 4]);

  const u8* in = static_cast<const u8*>(vertices);
  auto* out = static_cast<CPUCull::TransformedVertex*>(output);
  int done = 0;

#if defined(CPUCULL_PAIRS)
  const Columns8 model8 = Widen(model);
  const Columns8 proj8 = Widen(proj);
  for (; done + 1 < count; done += 2)
  {
    const F8 two = TransformTwo<HasZ, PerVertexMatrix>(in, in + stride, model8, proj8);
    _mm256_store_ps(&out[done].x, two);
    in += 2 * stride;
  }
#endif

  for (; done < count; ++done)
  {
    Store(&out[done], TransformOne<HasZ, PerVertexMatrix>(in, model, proj));
    in += stride;
  }
}
}  // namespace CPUCULL_NS

#undef CPUCULL_INLINE
#undef CPUCULL_TARGET
#undef CPUCULL_NS
#undef CPUCULL_X86
#undef CPUCULL_HADD
#undef CPUCULL_BLEND
#undef CPUCULL_PAIRS
#undef CPUCULL_FUSED
#undef CPUCULL_NEON
