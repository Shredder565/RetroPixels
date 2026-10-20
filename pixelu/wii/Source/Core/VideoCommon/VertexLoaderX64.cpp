// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/VertexLoaderX64.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstring>

#include "Common/Assert.h"
#include "Common/CPUDetect.h"
#include "Common/CommonTypes.h"
#include "Common/JitRegister.h"
#include "Common/x64ABI.h"
#include "Common/x64Emitter.h"
#include "VideoCommon/CPMemory.h"
#include "VideoCommon/VertexLoaderManager.h"

using namespace Gen;

namespace
{
// Registers of the generated loop. All are caller-saved in both the Windows and the System V
// conventions, so the loop saves nothing.
constexpr X64Reg kSrc = R10;     // current GX vertex
constexpr X64Reg kDst = R11;     // current host vertex
constexpr X64Reg kLeft = R9;     // vertices after this one
constexpr X64Reg kAnchor = RAX;  // base for addressing globals
constexpr X64Reg kIndex = RCX;   // array index, then the offset of the entry
constexpr X64Reg kArray = RDX;   // array base pointer
constexpr X64Reg kTmp = R8;
constexpr X64Reg kVec = XMM0;  // the attribute being converted
constexpr X64Reg kVecTmp = XMM1;

// Stack slots: the vertex count and the number of dropped vertices.
constexpr int kFrameBytes = 8;
constexpr int kSlotCount = 0;
constexpr int kSlotDropped = 4;

constexpr size_t kCodeBytes = 4096;
constexpr u32 kPoolBytes = 512;

// Globals are reached as [kAnchor + disp32]; they all live in the same image as this one.
const u8* AnchorAddress()
{
  return reinterpret_cast<const u8*>(&VertexLoaderManager::cached_arraybases);
}

// Colour conversion tables. Each GX colour byte contributes its own bits of the RGBA8 result
// (R in the lowest byte); OR-ing the entries of all bytes gives the colour. Channel widening
// (repeating the top bits below) splits cleanly between the bytes a channel spans.
struct ColorTables
{
  std::array<u32, 256> rgb565_hi;   // RRRRRGGG
  std::array<u32, 256> rgb565_lo;   // GGGBBBBB
  std::array<u32, 256> rgba4444_hi;  // RRRRGGGG
  std::array<u32, 256> rgba4444_lo;  // BBBBAAAA
  std::array<u32, 256> rgba6666_0;  // RRRRRRGG
  std::array<u32, 256> rgba6666_1;  // GGGGBBBB
  std::array<u32, 256> rgba6666_2;  // BBAAAAAA
};

constexpr ColorTables BuildColorTables()
{
  ColorTables t{};
  for (u32 b = 0; b < 256; b++)
  {
    const u32 r5 = b >> 3;
    const u32 g3 = b & 7;
    t.rgb565_hi[b] = ((r5 << 3) | (r5 >> 2)) | (((g3 << 5) | (g3 >> 1)) << 8) | 0xff000000u;
    t.rgb565_lo[b] = ((b >> 5) << 2 << 8) | ((((b & 31) << 3) | ((b & 31) >> 2)) << 16);

    t.rgba4444_hi[b] = ((b >> 4) * 0x11u) | (((b & 15) * 0x11u) << 8);
    t.rgba4444_lo[b] = (((b >> 4) * 0x11u) << 16) | (((b & 15) * 0x11u) << 24);

    const u32 r6 = b >> 2;
    const u32 g2 = b & 3;
    t.rgba6666_0[b] = ((r6 << 2) | (r6 >> 4)) | (((g2 << 6) | g2) << 8);
    const u32 b4 = b & 15;
    t.rgba6666_1[b] = ((b >> 4) << 2 << 8) | (((b4 << 4) | (b4 >> 2)) << 16);
    const u32 a6 = b & 63;
    t.rgba6666_2[b] = ((b >> 6) << 2 << 16) | (((a6 << 2) | (a6 >> 4)) << 24);
  }
  return t;
}

const ColorTables s_color_tables = BuildColorTables();

bool IsFloatFormat(ComponentFormat format)
{
  return format >= ComponentFormat::Float;
}

// PSHUFB control that moves `count` big-endian elements to the 32-bit lanes: unsigned values
// end up zero-extended, signed ones in the top of the lane (for an arithmetic shift down),
// floats byte-reversed. Every other byte becomes zero.
std::array<u8, 16> ElementShuffle(ComponentFormat format, int count)
{
  std::array<u8, 16> control;
  control.fill(0x80);
  const u32 size = GetElementSize(format);
  const bool high = format == ComponentFormat::Byte || format == ComponentFormat::Short;
  for (u32 lane = 0; lane < static_cast<u32>(count); lane++)
  {
    const u32 first = lane * size;
    const u32 top = lane * 4 + (high ? 3u : size - 1);
    for (u32 k = 0; k < size; k++)
      control[top - k] = static_cast<u8>(first + k);
  }
  return control;
}

std::array<u8, 16> ScaleVector(u32 frac)
{
  const float scale = std::ldexp(1.0f, -static_cast<int>(frac));
  std::array<u8, 16> bytes;
  for (u32 i = 0; i < 4; i++)
    std::memcpy(&bytes[i * 4], &scale, sizeof(scale));
  return bytes;
}

// Normals are fixed-point with a fixed fraction per format.
u32 NormalFrac(ComponentFormat format)
{
  switch (format)
  {
  case ComponentFormat::UByte:
    return 7;
  case ComponentFormat::Byte:
    return 6;
  case ComponentFormat::UShort:
    return 15;
  case ComponentFormat::Short:
    return 14;
  default:
    return 0;
  }
}
}  // namespace

VertexLoaderX64::VertexLoaderX64(const TVtxDesc& vtx_desc, const VAT& vtx_att)
    : VertexLoaderBase(vtx_desc, vtx_att)
{
  AllocCodeSpace(kCodeBytes);
  ClearCodeSpace();
  m_pool = region;
  SetCodePtr(region + kPoolBytes, region + kCodeBytes);
  Generate();
  WriteProtect(true);

  Common::JitRegister::Register(m_entry, GetCodePtr(), "VertexLoaderX64\nVtx desc: \n{}\nVAT:\n{}",
                                vtx_desc, vtx_att);
}

OpArg VertexLoaderX64::Global(const void* address) const
{
  return MDisp(kAnchor, static_cast<int>(PtrOffset(address, AnchorAddress())));
}

const void* VertexLoaderX64::PoolConstant(const Constant& value)
{
  for (u32 at = 0; at < m_pool_used; at += 16)
  {
    if (std::memcmp(m_pool + at, value.data(), value.size()) == 0)
      return m_pool + at;
  }
  ASSERT(m_pool_used + 16 <= kPoolBytes);
  u8* const slot = m_pool + m_pool_used;
  std::memcpy(slot, value.data(), value.size());
  m_pool_used += 16;
  return slot;
}

OpArg VertexLoaderX64::AttributeData(CPArray array, VertexComponentFormat source, bool may_drop)
{
  if (!IsIndexed(source))
    return MDisp(kSrc, static_cast<int>(m_in));

  const bool wide = source == VertexComponentFormat::Index16;
  LoadAndSwap(wide ? 16 : 8, kIndex, MDisp(kSrc, static_cast<int>(m_in)));
  m_in += wide ? 2 : 1;
  if (may_drop)
  {
    CMP(32, R(kIndex), Imm32(wide ? 0xffffu : 0xffu));
    m_drop_jumps.push_back(J_CC(CC_E, Jump::Near));
  }
  IMUL(32, kIndex, Global(&g_main_cp_state.array_strides[array]));
  MOV(64, R(kArray), Global(&VertexLoaderManager::cached_arraybases[array]));
  return MRegSum(kArray, kIndex);
}

void VertexLoaderX64::LoadVectorSSSE3(OpArg data, const VectorJob& job)
{
  const u32 bytes = GetElementSize(job.format) * static_cast<u32>(job.count_in);
  if (bytes <= 4)
    MOVD_xmm(kVec, data);
  else if (bytes <= 8)
    MOVQ_xmm(kVec, data);
  else
    MOVDQU(kVec, data);

  PSHUFB(kVec, M(PoolConstant(ElementShuffle(job.format, job.count_in))));
  if (job.format == ComponentFormat::Byte)
    PSRAD(kVec, 24);
  else if (job.format == ComponentFormat::Short)
    PSRAD(kVec, 16);
}

void VertexLoaderX64::LoadVectorSSE2(OpArg data, const VectorJob& job)
{
  switch (job.format)
  {
  case ComponentFormat::UByte:
    // Four source bytes, zero-extended to the four lanes.
    MOVD_xmm(kVec, data);
    PXOR(kVecTmp, R(kVecTmp));
    PUNPCKLBW(kVec, R(kVecTmp));
    PUNPCKLWD(kVec, R(kVecTmp));
    break;
  case ComponentFormat::Byte:
    // Four source bytes, each moved to the top of its lane and shifted down signed.
    MOVD_xmm(kVec, data);
    PUNPCKLBW(kVec, R(kVec));
    PUNPCKLWD(kVec, R(kVec));
    PSRAD(kVec, 24);
    break;
  default:
  {
    // 16-bit elements go to the high half of their lanes, then shift down. Three elements also
    // repeat the first one in the fourth lane.
    PXOR(kVec, R(kVec));
    const int lanes = job.count_in;
    for (int lane = 0; lane < lanes; lane++)
    {
      OpArg element = data;
      element.AddMemOffset(lane * 2);
      LoadAndSwap(16, kTmp, element);
      PINSRW(kVec, R(kTmp), static_cast<u8>(lane * 2 + 1));
    }
    if (lanes == 3)
    {
      LoadAndSwap(16, kTmp, data);
      PINSRW(kVec, R(kTmp), 7);
    }
    if (job.format == ComponentFormat::Short)
      PSRAD(kVec, 16);
    else
      PSRLD(kVec, 16);
    break;
  }
  }
}

void VertexLoaderX64::StoreToCache(Cache cache)
{
  switch (cache)
  {
  case Cache::Position:
  {
    // Rows of four floats, for the last three vertices.
    CMP(32, R(kLeft), Imm8(3));
    const FixupBranch not_last_three = J_CC(CC_AE);
    MOV(32, R(kTmp), R(kLeft));
    SHL(32, R(kTmp), Imm8(4));
    MOVUPS(MComplex(kAnchor, kTmp, SCALE_1,
                    static_cast<int>(
                        PtrOffset(VertexLoaderManager::position_cache.data(), AnchorAddress()))),
           kVec);
    SetJumpTarget(not_last_three);
    break;
  }
  case Cache::Normal:
  case Cache::Tangent:
  {
    TEST(32, R(kLeft), R(kLeft));
    const FixupBranch not_last = J_CC(CC_NZ);
    MOVUPS(Global(cache == Cache::Normal ? VertexLoaderManager::normal_cache.data() :
                                           VertexLoaderManager::tangent_cache.data()),
           kVec);
    SetJumpTarget(not_last);
    break;
  }
  case Cache::Binormal:
    // The binormal cache is written for every vertex, so it ends up with the last vertex that
    // was not dropped.
    MOVUPS(Global(VertexLoaderManager::binormal_cache.data()), kVec);
    break;
  case Cache::None:
    break;
  }
}

void VertexLoaderX64::ConvertVector(OpArg data, const VectorJob& job, AttributeFormat* attr)
{
  attr->type = ComponentFormat::Float;
  attr->components = job.count_out;
  attr->offset = static_cast<int>(m_out);
  attr->enable = true;
  attr->integer = false;

  OpArg dest = MDisp(kDst, static_cast<int>(m_out));
  m_out += 4 * static_cast<u32>(job.count_out);
  const u32 in_bytes = GetElementSize(job.format) * static_cast<u32>(job.count_in);
  if (job.source == VertexComponentFormat::Direct)
    m_in += in_bytes;

  if (cpu_info.bSSSE3)
  {
    LoadVectorSSSE3(data, job);
  }
  else if (IsFloatFormat(job.format))
  {
    // Without PSHUFB, floats are swapped and copied one by one through a GPR. Only the vectors
    // that feed the position, tangent and binormal caches are also gathered in kVec. The vector
    // store below then goes after the copied floats, as it always has without SSSE3.
    const bool gather = job.cache == Cache::Position || job.cache == Cache::Tangent ||
                        job.cache == Cache::Binormal;
    for (int i = 0; i < job.count_in; i++)
    {
      OpArg element = data;
      element.AddMemOffset(i * 4);
      LoadAndSwap(32, kTmp, element);
      OpArg out = dest;
      out.AddMemOffset(i * 4);
      MOV(32, out, R(kTmp));
      if (!gather)
        continue;
      if (cpu_info.bSSE4_1)
      {
        PINSRD(kVec, R(kTmp), static_cast<u8>(i));
      }
      else
      {
        PINSRW(kVec, R(kTmp), static_cast<u8>(2 * i));
        SHR(32, R(kTmp), Imm8(16));
        PINSRW(kVec, R(kTmp), static_cast<u8>(2 * i + 1));
      }
    }
    dest.AddMemOffset(static_cast<int>(in_bytes));
  }
  else
  {
    LoadVectorSSE2(data, job);
  }

  if (!IsFloatFormat(job.format))
  {
    CVTDQ2PS(kVec, R(kVec));
    if (job.frac != 0)
      MULPS(kVec, M(PoolConstant(ScaleVector(job.frac))));
  }

  if (job.count_out == 1)
    MOVSS(dest, kVec);
  else if (job.count_out == 2)
    MOVLPS(dest, kVec);
  else
    MOVUPS(dest, kVec);

  StoreToCache(job.cache);
}

void VertexLoaderX64::ConvertColor(OpArg data, VertexComponentFormat source, ColorFormat format)
{
  const OpArg dest = MDisp(kDst, static_cast<int>(m_out));
  const auto table = [](const std::array<u32, 256>& entries, X64Reg byte) {
    return MComplex(kAnchor, byte, SCALE_4,
                    static_cast<int>(PtrOffset(entries.data(), AnchorAddress())));
  };
  OpArg second = data;
  second.AddMemOffset(1);

  u32 consumed = 0;
  switch (format)
  {
  case ColorFormat::RGB888:
  case ColorFormat::RGB888x:
  case ColorFormat::RGBA8888:
    // Already R, G, B(, A) in memory order; four bytes are read even for RGB888.
    MOV(32, R(kTmp), data);
    if (format != ColorFormat::RGBA8888)
      OR(32, R(kTmp), Imm32(0xff000000));
    MOV(32, dest, R(kTmp));
    consumed = format == ColorFormat::RGB888 ? 3 : 4;
    break;

  case ColorFormat::RGB565:
  case ColorFormat::RGBA4444:
  {
    const ColorTables& t = s_color_tables;
    const bool is565 = format == ColorFormat::RGB565;
    MOVZX(32, 8, kTmp, data);
    MOVZX(32, 8, kIndex, second);
    MOV(32, R(kTmp), table(is565 ? t.rgb565_hi : t.rgba4444_hi, kTmp));
    OR(32, R(kTmp), table(is565 ? t.rgb565_lo : t.rgba4444_lo, kIndex));
    MOV(32, dest, R(kTmp));
    consumed = 2;
    break;
  }

  case ColorFormat::RGBA6666:
  {
    const ColorTables& t = s_color_tables;
    LEA(64, kIndex, data);
    MOVZX(32, 8, kTmp, MatR(kIndex));
    MOVZX(32, 8, kArray, MDisp(kIndex, 1));
    MOVZX(32, 8, kIndex, MDisp(kIndex, 2));
    MOV(32, R(kTmp), table(t.rgba6666_0, kTmp));
    OR(32, R(kTmp), table(t.rgba6666_1, kArray));
    OR(32, R(kTmp), table(t.rgba6666_2, kIndex));
    MOV(32, dest, R(kTmp));
    consumed = 3;
    break;
  }

  default:
    // Formats 6 and 7 do not exist: nothing is read or written.
    break;
  }

  if (source == VertexComponentFormat::Direct)
    m_in += consumed;
}

void VertexLoaderX64::WriteTexMatrix(u32 index_offset, bool coordinate_present)
{
  // The matrix index byte is used as it is (no 6-bit mask), converted through kVec.
  MOVZX(32, 8, kTmp, MDisp(kSrc, static_cast<int>(index_offset)));
  const OpArg dest = MDisp(kDst, static_cast<int>(m_out));
  if (coordinate_present)
  {
    CVTSI2SS(kVec, R(kTmp));
    MOVSS(dest, kVec);
    m_out += 4;
  }
  else
  {
    // (0, 0, index) and a zero after it.
    PXOR(kVec, R(kVec));
    CVTSI2SS(kVec, R(kTmp));
    PSLLDQ(kVec, 8);
    MOVUPS(dest, kVec);
    m_out += 12;
  }
}

void VertexLoaderX64::Generate()
{
  const TVtxDesc& desc = m_VtxDesc;
  const VAT& vat = m_VtxAttr;
  PortableVertexDeclaration& decl = m_native_vtx_decl;

  m_entry = GetCodePtr();

  // Take the arguments into the loop registers, then keep the count and a dropped-vertex
  // counter on the stack.
  MOV(64, R(kSrc), R(ABI_PARAM1));
  MOV(64, R(kDst), R(ABI_PARAM2));
  MOV(64, R(kAnchor), R(ABI_PARAM4));
  SUB(64, R(RSP), Imm8(kFrameBytes));
  MOV(32, MDisp(RSP, kSlotCount), R(ABI_PARAM3));
  MOV(32, MDisp(RSP, kSlotDropped), Imm32(0));
  LEA(32, kLeft, MDisp(ABI_PARAM3, -1));

  const u8* const vertex_start = GetCodePtr();

  if (desc.low.PosMatIdx)
  {
    MOVZX(32, 8, kIndex, MDisp(kSrc, static_cast<int>(m_in)));
    AND(32, R(kIndex), Imm8(0x3f));
    MOV(32, MDisp(kDst, static_cast<int>(m_out)), R(kIndex));
    CMP(32, R(kLeft), Imm8(3));
    const FixupBranch not_last_three = J_CC(CC_AE);
    MOV(32,
        MComplex(kAnchor, kLeft, SCALE_4,
                 static_cast<int>(PtrOffset(VertexLoaderManager::position_matrix_index_cache.data(),
                                            AnchorAddress()))),
        R(kIndex));
    SetJumpTarget(not_last_three);

    decl.posmtx.type = ComponentFormat::UByte;
    decl.posmtx.components = 4;
    decl.posmtx.offset = static_cast<int>(m_out);
    decl.posmtx.enable = true;
    decl.posmtx.integer = true;
    m_in += 1;
    m_out += 4;
  }

  std::array<u32, 8> texmtx_at{};
  for (u32 i = 0; i < 8; i++)
  {
    if (desc.low.TexMatIdx[i])
      texmtx_at[i] = m_in++;
  }

  // Position. Fixed-point positions are only scaled when ByteDequant is set.
  {
    const int n = vat.g0.PosElements == CoordComponentCount::XYZ ? 3 : 2;
    const VectorJob job{desc.low.Position,
                        vat.g0.PosFormat,
                        n,
                        n,
                        vat.g0.ByteDequant ? static_cast<u32>(vat.g0.PosFrac) : 0u,
                        Cache::Position};
    ConvertVector(AttributeData(CPArray::Position, desc.low.Position, true), job,
                  &decl.position);
  }

  // Normal, tangent, binormal. Each vector of an NTB record follows the previous one; with
  // three indices each index picks its own record, still at the vector's own offset.
  if (desc.low.Normal != VertexComponentFormat::NotPresent)
  {
    const VertexComponentFormat source = desc.low.Normal;
    const ComponentFormat format = vat.g0.NormalFormat;
    const u32 frac = NormalFrac(format);
    const OpArg normal = AttributeData(CPArray::Normal, source, false);
    ConvertVector(normal, VectorJob{source, format, 3, 3, frac, Cache::Normal}, &decl.normals[0]);

    if (vat.g0.NormalElements == NormalComponentCount::NTB)
    {
      const bool three_indices = IsIndexed(source) && vat.g0.NormalIndex3;
      const int vector_bytes = static_cast<int>(GetElementSize(format) * 3);

      OpArg tangent = three_indices ? AttributeData(CPArray::Normal, source, false) : normal;
      tangent.AddMemOffset(vector_bytes);
      ConvertVector(tangent, VectorJob{source, format, 3, 3, frac, Cache::Tangent},
                    &decl.normals[1]);

      OpArg binormal = three_indices ? AttributeData(CPArray::Normal, source, false) : normal;
      binormal.AddMemOffset(2 * vector_bytes);
      ConvertVector(binormal, VectorJob{source, format, 3, 3, frac, Cache::Binormal},
                    &decl.normals[2]);
    }
  }

  for (u8 i = 0; i < 2; i++)
  {
    const VertexComponentFormat source = desc.low.Color[i];
    if (source == VertexComponentFormat::NotPresent)
      continue;
    ConvertColor(AttributeData(CPArray::Color0 + i, source, false), source,
                 vat.GetColorFormat(i));
    AttributeFormat& color = decl.colors[i];
    color.type = ComponentFormat::UByte;
    color.components = 4;
    color.offset = static_cast<int>(m_out);
    color.enable = true;
    color.integer = false;
    m_out += 4;
  }

  // Texture coordinates. With a matrix index the coordinate is widened to (s, t, index).
  for (u8 i = 0; i < 8; i++)
  {
    const VertexComponentFormat source = desc.high.TexCoord[i];
    const bool present = source != VertexComponentFormat::NotPresent;
    const bool has_matrix = desc.low.TexMatIdx[i];
    AttributeFormat& tex = decl.texcoords[i];
    if (present)
    {
      const int n = vat.GetTexElements(i) == TexComponentCount::ST ? 2 : 1;
      const VectorJob job{source,
                          vat.GetTexFormat(i),
                          n,
                          has_matrix ? 2 : n,
                          vat.g0.ByteDequant ? static_cast<u32>(vat.GetTexFrac(i)) : 0u,
                          Cache::None};
      ConvertVector(AttributeData(CPArray::TexCoord0 + i, source, false), job, &tex);
    }
    if (has_matrix)
    {
      tex.type = ComponentFormat::Float;
      tex.components = 3;
      tex.enable = true;
      tex.integer = false;
      if (!present)
        tex.offset = static_cast<int>(m_out);
      WriteTexMatrix(texmtx_at[i], present);
    }
  }

  // Next vertex.
  ADD(64, R(kDst), Imm32(m_out));
  const u8* const next_source = GetCodePtr();
  ADD(64, R(kSrc), Imm32(m_in));
  SUB(32, R(kLeft), Imm8(1));
  J_CC(CC_NS, vertex_start);

  MOV(32, R(EAX), MDisp(RSP, kSlotCount));
  SUB(32, R(EAX), MDisp(RSP, kSlotDropped));
  ADD(64, R(RSP), Imm8(kFrameBytes));
  RET();

  // Dropped vertices skip the rest of the vertex and leave the output position alone.
  if (!m_drop_jumps.empty())
  {
    for (const FixupBranch& jump : m_drop_jumps)
      SetJumpTarget(jump);
    ADD(32, MDisp(RSP, kSlotDropped), Imm8(1));
    JMP(next_source);
  }

  ASSERT_MSG(VIDEO, m_vertex_size == m_in,
             "Vertex size from vertex loader ({}) does not match expected vertex size ({})!\nVtx "
             "desc: {:08x} {:08x}\nVtx attr: {:08x} {:08x} {:08x}",
             m_in, m_vertex_size, desc.low.Hex, desc.high.Hex, vat.g0.Hex, vat.g1.Hex, vat.g2.Hex);
  decl.stride = static_cast<int>(m_out);
}

int VertexLoaderX64::RunVertices(const u8* src, u8* dst, int count)
{
  m_numLoadedVertices += count;
  if (count <= 0)
    return 0;
  using LoopFn = int (*)(const u8* src, u8* dst, int count, const void* anchor);
  const auto loop = reinterpret_cast<LoopFn>(const_cast<u8*>(m_entry));
  return loop(src, dst, count, AnchorAddress());
}
