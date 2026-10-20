// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// BP register writes: masking, change detection and the side effects of each register (state
// invalidation, EFB copies, TMEM loads, PE tokens), plus their text descriptions.

#include "VideoCommon/BPStructs.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <string>
#include <utility>

#include <fmt/format.h>

#include "Common/CommonTypes.h"
#include "Common/EnumMap.h"
#include "Common/Logging/Log.h"

#include "Core/DolphinAnalytics.h"
#include "Core/FifoPlayer/FifoPlayer.h"
#include "Core/FifoPlayer/FifoRecorder.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/VideoInterface.h"
#include "Core/System.h"

#include "VideoCommon/BPFunctions.h"
#include "VideoCommon/BPMemory.h"
#include "VideoCommon/BoundingBox.h"
#include "VideoCommon/Fifo.h"
#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/GeometryShaderManager.h"
#include "VideoCommon/OpcodeDecoding.h"
#include "VideoCommon/PerfQueryBase.h"
#include "VideoCommon/PixelEngine.h"
#include "VideoCommon/PixelShaderManager.h"
#include "VideoCommon/Present.h"
#include "VideoCommon/Statistics.h"
#include "VideoCommon/TMEM.h"
#include "VideoCommon/TextureCacheBase.h"
#include "VideoCommon/TextureDecoder.h"
#include "VideoCommon/VideoBackendBase.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/VideoEvents.h"
#include "VideoCommon/XFStateManager.h"

void BPInit()
{
  std::memset(reinterpret_cast<u8*>(&bpmem), 0, sizeof(bpmem));
  bpmem.bpMask = 0xFFFFFF;
}

namespace
{
// Exponent used for each EFB copy gamma setting (the reserved value acts like 2.2).
constexpr Common::EnumMap<float, GammaCorrection::Invalid2_2> COPY_GAMMA = {1.0f, 1.7f, 2.2f,
                                                                            2.2f};

// The BP register file as words.
u32* BPWords()
{
  return reinterpret_cast<u32*>(&bpmem);
}

// Registers that act on every write ("commands"), even when the value does not change.
bool IsCommandRegister(int address)
{
  switch (address)
  {
  case BPMEM_TRIGGER_EFB_COPY:
  case BPMEM_CLEARBBOX1:
  case BPMEM_CLEARBBOX2:
  case BPMEM_SETDRAWDONE:
  case BPMEM_PE_TOKEN_ID:
  case BPMEM_PE_TOKEN_INT_ID:
  case BPMEM_LOADTLUT0:
  case BPMEM_LOADTLUT1:
  case BPMEM_TEXINVALIDATE:
  case BPMEM_PRELOAD_MODE:
  case BPMEM_CLEAR_PIXEL_PERF:
    return true;
  default:
    return false;
  }
}

// Registers whose value is only read later (when copying, clearing, drawing...), so writing
// them needs nothing beyond the store.
bool IsPassiveRegister(int address)
{
  switch (address)
  {
  case BPMEM_DISPLAYCOPYFILTER:
  case BPMEM_DISPLAYCOPYFILTER + 1:
  case BPMEM_DISPLAYCOPYFILTER + 2:
  case BPMEM_DISPLAYCOPYFILTER + 3:
  case BPMEM_COPYFILTER0:
  case BPMEM_COPYFILTER1:
  case BPMEM_IND_IMASK:
  case BPMEM_PERF0_TRI:
  case BPMEM_PERF0_QUAD:
  case BPMEM_PERF1:
  case BPMEM_BUSCLOCK0:
  case BPMEM_BUSCLOCK1:
  case BPMEM_FIELDMASK:  // field rendering is not emulated
  case BPMEM_FIELDMODE:
  case BPMEM_EFB_TL:
  case BPMEM_EFB_WH:
  case BPMEM_EFB_ADDR:
  case BPMEM_EFB_STRIDE:
  case BPMEM_COPYYSCALE:
  case BPMEM_CLEAR_AR:
  case BPMEM_CLEAR_GB:
  case BPMEM_CLEAR_Z:
  case BPMEM_REVBITS:
  case BPMEM_PRELOAD_ADDR:
  case BPMEM_PRELOAD_TMEMEVEN:
  case BPMEM_PRELOAD_TMEMODD:
  case BPMEM_LOADTLUT0:  // source address for the next BPMEM_LOADTLUT1
  case BPMEM_BP_MASK:    // applied by LoadBPReg
    return true;
  default:
    return false;
  }
}

// Draw done and tokens report that everything before them has been drawn, so pending EFB copies
// must land and EFB peeks must see fresh data.
void FinishPendingEFBWork()
{
  g_texture_cache->FlushEFBCopies();
  g_texture_cache->FlushStaleBinds();
  g_framebuffer_manager->InvalidatePeekCache(false);
  g_framebuffer_manager->RefreshPeekCache();
}

// In deterministic mode the PE events were already raised while preprocessing on the CPU.
bool PixelEngineEventsFromGPU(Core::System& system)
{
  return !system.GetFifo().UseDeterministicGPUThread();
}

void OnDrawDone(u32 value, int cycles_into_future)
{
  if ((value & 0xFF) != 0x02)
  {
    WARN_LOG_FMT(VIDEO, "GXSetDrawDone ??? (value {:#04X})", value & 0xFFFF);
    return;
  }

  INCSTAT(g_stats.this_frame.num_draw_done);
  FinishPendingEFBWork();
  auto& system = Core::System::GetInstance();
  if (PixelEngineEventsFromGPU(system))
    system.GetPixelEngine().SetFinish(cycles_into_future);
  DEBUG_LOG_FMT(VIDEO, "GXSetDrawDone SetPEFinish (value: {:#04X})", value & 0xFFFF);
}

void OnToken(u32 value, bool interrupt, int cycles_into_future)
{
  if (interrupt)
    INCSTAT(g_stats.this_frame.num_token_int);
  else
    INCSTAT(g_stats.this_frame.num_token);
  FinishPendingEFBWork();

  auto& system = Core::System::GetInstance();
  const u16 token = static_cast<u16>(value & 0xFFFF);
  if (PixelEngineEventsFromGPU(system))
    system.GetPixelEngine().SetToken(token, interrupt, cycles_into_future);
  if (interrupt)
    DEBUG_LOG_FMT(VIDEO, "SetPEToken + INT {:#06X}", value & 0xFFFF);
  else
    DEBUG_LOG_FMT(VIDEO, "SetPEToken {:#06X}", value & 0xFFFF);
}

// Display copy (EFB to XFB): the end of a frame for most games.
void CopyToXFB(PixelShaderManager& pixel_shader_manager, const UPE_Copy& copy, u32 dest_addr,
               u32 dest_stride, u32 copy_width, const MathUtil::Rectangle<s32>& src)
{
  // Bounding box tracking stops at a display copy.
  g_bounding_box->Disable(pixel_shader_manager);

  // The vertical scale register is 8.8 fixed point; the invert bit uses its reciprocal.
  const float scale_reg = static_cast<float>(bpmem.dispcopyyscale);
  const float y_scale = copy.scale_invert ? 256.0f / scale_reg : scale_reg / 256.0f;
  const float xfb_lines = 1.0f + static_cast<float>(bpmem.copyTexSrcWH.y.Value()) * y_scale;
  const u32 xfb_height = static_cast<u32>(xfb_lines);

  DEBUG_LOG_FMT(VIDEO,
                "RenderToXFB: destAddr: {:08x} | srcRect [{} {} {} {}] | fbWidth: {} | "
                "fbStride: {} | fbHeight: {} | yScale: {}",
                dest_addr, src.left, src.top, src.right, src.bottom, bpmem.copyTexSrcWH.x + 1,
                dest_stride, xfb_height, y_scale);

  const bool from_depth = bpmem.zcontrol.pixel_format == PixelFormat::Z24;
  g_texture_cache->CopyRenderTargetToTexture(
      dest_addr, EFBCopyFormat::XFB, copy_width, xfb_height, dest_stride, from_depth, src, false,
      false, y_scale, COPY_GAMMA[copy.gamma], bpmem.triggerEFBCopy.clamp_top,
      bpmem.triggerEFBCopy.clamp_bottom, bpmem.copyfilter.GetCoefficients());

  // Treat every display copy as a frame boundary. Games that assemble one XFB from several
  // smaller copies (e.g. to exceed the EFB size) produce several "frames" per real frame; telling
  // those apart would need tracking the VI configuration.
  auto& system = Core::System::GetInstance();
  system.GetVideoEvents().after_frame_event.Trigger(system);

  if (g_ActiveConfig.bImmediateXFB)
  {
    // The stride is in bytes; two bytes per YUYV pixel gives the width.
    g_presenter->ImmediateSwap(dest_addr, dest_stride / 2, dest_stride, xfb_height);
  }
  else if (system.GetFifoPlayer().IsRunningWithFakeVideoInterfaceUpdates())
  {
    system.GetVideoInterface().FakeVIUpdate(dest_addr, static_cast<u32>(src.GetWidth()),
                                            dest_stride, xfb_height);
  }
}

// BPMEM_TRIGGER_EFB_COPY: copy the source rectangle to a texture in RAM or to the XFB, then
// optionally clear it.
void ExecuteEFBCopy(PixelShaderManager& pixel_shader_manager)
{
  const u32 dest_addr = bpmem.copyTexDest << 5;
  const u32 dest_stride = bpmem.copyDestStride << 5;
  const UPE_Copy copy = bpmem.triggerEFBCopy;

  // Source rectangle; the size registers hold size - 1.
  MathUtil::Rectangle<s32> src;
  src.left = static_cast<s32>(bpmem.copyTexSrcXY.x.Value());
  src.top = static_cast<s32>(bpmem.copyTexSrcXY.y.Value());
  src.right = static_cast<s32>(bpmem.copyTexSrcXY.x + bpmem.copyTexSrcWH.x + 1);
  src.bottom = static_cast<s32>(bpmem.copyTexSrcXY.y + bpmem.copyTexSrcWH.y + 1);

  // The 10-bit registers allow regions up to 1024x1024. On hardware an oversized copy still
  // writes as many bytes as the size says, filling the excess with format-dependent junk read
  // past the EFB. That junk is not reproduced: a copy starting outside the EFB writes nothing,
  // and one extending past it is clamped to the EFB edges. (Some arcade Virtual Console titles
  // do this.)
  if (static_cast<u32>(src.right) > EFB_WIDTH || static_cast<u32>(src.bottom) > EFB_HEIGHT)
  {
    WARN_LOG_FMT(VIDEO, "Oversized EFB copy: {}x{} (offset {},{} stride {})", src.GetWidth(),
                 src.GetHeight(), src.left, src.top, dest_stride);

    if (static_cast<u32>(src.left) >= EFB_WIDTH || static_cast<u32>(src.top) >= EFB_HEIGHT)
    {
      // Nothing of the EFB is covered (seen from at least one online mode). A display copy
      // still ends bounding box tracking, as the normal path would.
      if (copy.copy_to_xfb)
        g_bounding_box->Disable(pixel_shader_manager);
      return;
    }

    src.right = std::clamp<int>(src.right, 0, EFB_WIDTH);
    src.bottom = std::clamp<int>(src.bottom, 0, EFB_HEIGHT);
  }

  const u32 copy_width = static_cast<u32>(src.GetWidth());
  const u32 copy_height = static_cast<u32>(src.GetHeight());

  // A zero stride copies nothing (but a requested clear still happens).
  if (dest_stride != 0)
  {
    if (copy.copy_to_xfb)
    {
      CopyToXFB(pixel_shader_manager, copy, dest_addr, dest_stride, copy_width, src);
    }
    else
    {
      // With a Z24 EFB the depth buffer is the copy source.
      const bool from_depth = bpmem.zcontrol.pixel_format == PixelFormat::Z24;
      g_texture_cache->CopyRenderTargetToTexture(
          dest_addr, copy.tp_realFormat(), copy_width, copy_height, dest_stride, from_depth, src,
          copy.intensity_fmt && copy.auto_conv, copy.half_scale, 1.0f, COPY_GAMMA[copy.gamma],
          bpmem.triggerEFBCopy.clamp_top, bpmem.triggerEFBCopy.clamp_bottom,
          bpmem.copyfilter.GetCoefficients());
    }
  }

  if (!copy.clear)
    return;

  // The clear happens after the copy has read the region.
  BPFunctions::ClearScreen(g_framebuffer_manager.get(), src, bpmem.blendmode.color_update != 0,
                           bpmem.blendmode.alpha_update != 0, bpmem.zmode.update_enable != 0,
                           bpmem.zcontrol.pixel_format, bpmem.clearcolorAR, bpmem.clearcolorGB,
                           bpmem.clearZValue);
  // Clearing changes the scissor, so put the game's back.
  BPFunctions::SetScissorAndViewport(g_framebuffer_manager.get(), bpmem.scissorTL,
                                     bpmem.scissorBR, bpmem.scissorOffset, xfmem.viewport);
}

void NoteTMEMSource(Core::System& system, u32 address, u32 size)
{
  if (OpcodeDecoder::g_record_fifo_data)
    system.GetFifoRecorder().UseMemory(address, size, MemoryUpdate::Type::TMEM);
}

// BPMEM_LOADTLUT1: copy a palette from RAM into TMEM.
void LoadTLUT()
{
  const auto& tlut = bpmem.tmem_config.tlut_dest;
  const u32 tmem_offset = tlut.tmem_addr << 9;
  const u32 byte_count = tlut.tmem_line_count * TMEM_LINE_SIZE;
  u32 source = bpmem.tmem_config.tlut_src << 5;

  // The GameCube decodes fewer address bits; some games leave junk in the high ones.
  auto& system = Core::System::GetInstance();
  if (!system.IsWii())
    source &= 0x01FFFFFF;

  // The largest offset plus the largest size still fits in TMEM, so no bounds check is needed.
  constexpr u32 LARGEST_OFFSET = (1u << decltype(tlut.tmem_addr)::NumBits()) << 9;
  constexpr u32 LARGEST_SIZE = (1u << decltype(tlut.tmem_line_count)::NumBits()) * TMEM_LINE_SIZE;
  static_assert(LARGEST_OFFSET + LARGEST_SIZE < TMEM_SIZE);

  system.GetMemory().CopyFromEmu(s_tex_mem.data() + tmem_offset, source, byte_count);
  NoteTMEMSource(system, source, byte_count);
  TMEM::InvalidateAll();
}

// BPMEM_PRELOAD_MODE with a non-zero value: load texture data straight into TMEM (manual TMEM
// management). How exactly the hardware does this is not well understood.
void PreloadTMEM()
{
  const BPS_TmemConfig& config = bpmem.tmem_config;
  const u32 source = config.preload_addr << 5;
  const u32 lines = config.preload_tile_info.count;
  u32 even = config.preload_tmem_even * TMEM_LINE_SIZE;
  u32 consumed = 0;

  auto& system = Core::System::GetInstance();
  auto& memory = system.GetMemory();

  if (config.preload_tile_info.type == 3)
  {
    // RGBA8 (and apparently CI14): the AR and GB halves of each tile go to different TMEM
    // banks, a line at a time.
    u32 odd = config.preload_tmem_odd * TMEM_LINE_SIZE;
    for (u32 line = 0; line < lines; ++line)
    {
      if (even + TMEM_LINE_SIZE > TMEM_SIZE || odd + TMEM_LINE_SIZE > TMEM_SIZE)
        break;
      memory.CopyFromEmu(s_tex_mem.data() + even, source + consumed, TMEM_LINE_SIZE);
      memory.CopyFromEmu(s_tex_mem.data() + odd, source + consumed + TMEM_LINE_SIZE,
                         TMEM_LINE_SIZE);
      even += TMEM_LINE_SIZE;
      odd += TMEM_LINE_SIZE;
      consumed += 2 * TMEM_LINE_SIZE;
    }
  }
  else if (even < TMEM_SIZE)
  {
    // Everything else is one contiguous block, cut off at the end of TMEM.
    consumed = std::min(lines * TMEM_LINE_SIZE, TMEM_SIZE - even);
    memory.CopyFromEmu(s_tex_mem.data() + even, source, consumed);
  }

  NoteTMEMSource(Core::System::GetInstance(), source, consumed);
  TMEM::InvalidateAll();
}

// BPMEM_TEV_COLOR_RA/BG: each writes two components of one TEV colour register, either the
// colour or the konstant set depending on the type bit.
void UpdateTevColor(PixelShaderManager& pixel_shader_manager, int address)
{
  const int reg = (address >> 1) & 0x3;
  const TevReg& tev = bpmem.tevregs[reg];
  const bool is_red_alpha = ((address - BPMEM_TEV_COLOR_RA) & 1) == 0;

  // Components: 0 red, 1 green, 2 blue, 3 alpha.
  const std::array<std::pair<int, s32>, 2> parts =
      is_red_alpha ? std::array<std::pair<int, s32>, 2>{{{0, tev.ra.red}, {3, tev.ra.alpha}}} :
                     std::array<std::pair<int, s32>, 2>{{{1, tev.bg.green}, {2, tev.bg.blue}}};
  const bool konst =
      (is_red_alpha ? tev.ra.type.Value() : tev.bg.type.Value()) == TevRegType::Constant;
  for (const auto& [component, value] : parts)
  {
    if (konst)
      pixel_shader_manager.SetTevKonstColor(reg, component, value);
    else
      pixel_shader_manager.SetTevColor(reg, component, value);
  }
}

struct Managers
{
  PixelShaderManager& pixel;
  XFStateManager& xf;
  GeometryShaderManager& geometry;
};

// Registers with a fixed address (or a short contiguous run). Returns false for addresses it
// does not know.
bool WriteFixedRegister(const Managers& m, const BPCmd& bp, int cycles_into_future)
{
  const int address = bp.address;
  const int changes = bp.changes;
  PixelShaderManager& psm = m.pixel;

  if (IsPassiveRegister(address))
    return true;

  if (address >= BPMEM_IND_MTXA && address < BPMEM_IND_MTXA + 9)
  {
    if (changes)
      psm.SetIndMatrixChanged((address - BPMEM_IND_MTXA) / 3);
    return true;
  }
  if (address >= BPMEM_FOGRANGE && address < BPMEM_FOGRANGE + 6)
  {
    if (changes)
      psm.SetFogRangeAdjustChanged();
    return true;
  }
  if (address >= BPMEM_TEV_KSEL && address < BPMEM_TEV_KSEL + 8)
  {
    psm.SetTevKSel(address - BPMEM_TEV_KSEL, static_cast<u32>(bp.newvalue));
    return true;
  }
  if (address >= BPMEM_TEV_COLOR_RA && address < BPMEM_TEV_COLOR_RA + 8)
  {
    UpdateTevColor(psm, address);
    return true;
  }

  switch (address)
  {
  // Rasterizer setup.
  case BPMEM_GENMODE:
    PRIM_LOG("genmode: texgen={}, col={}, multisampling={}, tev={}, cull_mode={}, ind={}, "
             "zfeeze={}",
             bpmem.genMode.numtexgens, bpmem.genMode.numcolchans, bpmem.genMode.multisampling,
             bpmem.genMode.numtevstages + 1, bpmem.genMode.cull_mode, bpmem.genMode.numindstages,
             bpmem.genMode.zfreeze);
    if (changes)
      psm.SetGenModeChanged();
    // Only the cull mode bits affect rasterization state.
    if (changes & 0xC000)
      BPFunctions::SetGenerationMode();
    return true;

  case BPMEM_SCISSORTL:
  case BPMEM_SCISSORBR:
  case BPMEM_SCISSOROFFSET:
    m.xf.SetViewportChanged();
    m.geometry.SetViewportChanged();
    return true;

  case BPMEM_LINEPTWIDTH:
    m.geometry.SetLinePtWidthChanged();
    return true;

  // Indirect texturing.
  case BPMEM_RAS1_SS0:
  case BPMEM_RAS1_SS1:
    if (changes)
      psm.SetIndTexScaleChanged(address == BPMEM_RAS1_SS1);
    return true;

  case BPMEM_IREF:
    if (changes)
      psm.SetTevIndirectChanged();
    return true;

  // Pixel engine state.
  case BPMEM_ZMODE:
    PRIM_LOG("zmode: test={}, func={}, upd={}", bpmem.zmode.test_enable, bpmem.zmode.func,
             bpmem.zmode.update_enable);
    BPFunctions::SetDepthMode();
    psm.SetZModeControl();
    return true;

  case BPMEM_BLENDMODE:
    if (changes & 0xFFFF)
    {
      PRIM_LOG("blendmode: en={}, open={}, colupd={}, alphaupd={}, dst={}, src={}, sub={}, "
               "mode={}",
               bpmem.blendmode.blend_enable, bpmem.blendmode.logic_op_enable,
               bpmem.blendmode.color_update, bpmem.blendmode.alpha_update,
               bpmem.blendmode.dst_factor, bpmem.blendmode.src_factor, bpmem.blendmode.subtract,
               bpmem.blendmode.logic_mode);
      BPFunctions::SetBlendMode();
      psm.SetBlendModeChanged();
    }
    return true;

  case BPMEM_CONSTANTALPHA:
    PRIM_LOG("constalpha: alp={}, en={}", bpmem.dstalpha.alpha, bpmem.dstalpha.enable);
    if (changes)
    {
      psm.SetAlpha();
      psm.SetDestAlphaChanged();
    }
    // The enable bit affects blending.
    if (changes & 0x100)
      BPFunctions::SetBlendMode();
    return true;

  case BPMEM_ZCOMPARE:
    BPFunctions::OnPixelFormatChange(g_framebuffer_manager.get(), bpmem.zcontrol.pixel_format,
                                     bpmem.zcontrol.zformat);
    // The pixel format decides whether dual-source blending is possible.
    if (changes & 7)
      BPFunctions::SetBlendMode();
    psm.SetZModeControl();
    return true;

  case BPMEM_ALPHACOMPARE:
    PRIM_LOG("alphacmp: ref0={}, ref1={}, comp0={}, comp1={}, logic={}", bpmem.alpha_test.ref0,
             bpmem.alpha_test.ref1, bpmem.alpha_test.comp0, bpmem.alpha_test.comp1,
             bpmem.alpha_test.logic);
    if (changes & 0xFFFF)
      psm.SetAlpha();
    if (changes)
    {
      psm.SetAlphaTestChanged();
      BPFunctions::SetBlendMode();
    }
    return true;

  case BPMEM_BIAS:
    PRIM_LOG("ztex bias={:#x}", bpmem.ztex1.bias);
    if (changes)
      psm.SetZTextureBias();
    return true;

  case BPMEM_ZTEX2:
    if (changes & 3)
      psm.SetZTextureTypeChanged();
    if (changes & 12)
      psm.SetZTextureOpChanged();
    PRIM_LOG("ztex op={}, type={}", bpmem.ztex2.op, bpmem.ztex2.type);
    return true;

  // Fog.
  case BPMEM_FOGPARAM0:
  case BPMEM_FOGBMAGNITUDE:
  case BPMEM_FOGBEXPONENT:
  case BPMEM_FOGPARAM3:
    if (changes)
      psm.SetFogParamChanged();
    return true;

  case BPMEM_FOGCOLOR:
    if (changes)
      psm.SetFogColorChanged();
    return true;

  // Commands.
  case BPMEM_SETDRAWDONE:
    OnDrawDone(static_cast<u32>(bp.newvalue), cycles_into_future);
    return true;

  case BPMEM_PE_TOKEN_ID:
  case BPMEM_PE_TOKEN_INT_ID:
    OnToken(static_cast<u32>(bp.newvalue), address == BPMEM_PE_TOKEN_INT_ID, cycles_into_future);
    return true;

  case BPMEM_TRIGGER_EFB_COPY:
    ExecuteEFBCopy(psm);
    return true;

  case BPMEM_LOADTLUT1:
    LoadTLUT();
    return true;

  case BPMEM_PRELOAD_MODE:
    // Zero (written by GXTexModeSync) returns to automatic TMEM management.
    if (bp.newvalue != 0)
      PreloadTMEM();
    return true;

  case BPMEM_TEXINVALIDATE:
    TMEM::Invalidate(static_cast<u32>(bp.newvalue));
    return true;

  case BPMEM_CLEARBBOX1:
  case BPMEM_CLEARBBOX2:
  {
    // Each register sets two of the four bounds: left/right or top/bottom.
    const u32 first = static_cast<u32>(address & 2);
    g_bounding_box->Enable(psm);
    g_bounding_box->Set(first, static_cast<u16>(bp.newvalue & 0x3ff));
    g_bounding_box->Set(first + 1, static_cast<u16>(bp.newvalue >> 10));
    return true;
  }

  case BPMEM_CLEAR_PIXEL_PERF:
    // GXClearPixMetric writes 0xAAA; at least one game toggles between 0 and 0xAAA.
    if (PerfQueryBase::ShouldEmulate())
      g_perf_query->ResetQuery();
    return true;

  default:
    return false;
  }
}

// Registers addressed in blocks: TEV order and texture coordinate sizes (by groups of four),
// texture units, indirect stages and TEV combiners.
bool WriteBlockRegister(const Managers& m, const BPCmd& bp)
{
  const int address = bp.address;
  const u32 value = static_cast<u32>(bp.newvalue);

  if (address >= BPMEM_TREF && address < BPMEM_TREF + 8)
  {
    m.pixel.SetTevOrder(address - BPMEM_TREF, value);
    return true;
  }
  if (address >= BPMEM_SU_SSIZE && address < BPMEM_SU_SSIZE + 16)
  {
    // S and T size registers alternate; both map to the same texture coordinate.
    if (bp.changes)
    {
      const auto texcoord = static_cast<u8>((address - BPMEM_SU_SSIZE) >> 1);
      m.pixel.SetTexCoordChanged(texcoord);
      m.geometry.SetTexCoordChanged(texcoord);
    }
    return true;
  }
  if ((address & 0xc0) == 0x80)
  {
    const auto unit_address = TexUnitAddress::FromBPAddress(static_cast<u32>(address));
    // Every texture unit register except the unused slot reconfigures TMEM.
    if (unit_address.Reg != TexUnitAddress::Register::UNKNOWN)
    {
      TMEM::ConfigurationChanged(unit_address, value);
      return true;
    }
  }
  if (address >= BPMEM_IND_CMD && address < BPMEM_IND_CMD + 16)
  {
    m.pixel.SetTevIndirectChanged();
    return true;
  }
  if (address >= BPMEM_TEV_COLOR_ENV && address < BPMEM_TEV_COLOR_ENV + 32)
  {
    // Colour and alpha combiners alternate.
    const int offset = address - BPMEM_TEV_COLOR_ENV;
    m.pixel.SetTevCombiner(offset >> 1, offset & 1, value);
    return true;
  }
  return false;
}

void BPWritten(const Managers& managers, const BPCmd& bp, int cycles_into_future)
{
  // Rewriting the same value is free unless the register is a command.
  if (BPWords()[bp.address] == static_cast<u32>(bp.newvalue) && !IsCommandRegister(bp.address))
    return;

  // Anything queued was drawn with the old state.
  BPFunctions::FlushPipeline();
  BPWords()[bp.address] = static_cast<u32>(bp.newvalue);

  if (WriteFixedRegister(managers, bp, cycles_into_future) || WriteBlockRegister(managers, bp))
    return;

  DolphinAnalytics::Instance().ReportGameQuirk(GameQuirk::UsesUnknownBPCommand);
  WARN_LOG_FMT(VIDEO, "Unknown BP opcode: address = {:#010x} value = {:#010x}", bp.address,
               bp.newvalue);
}
}  // namespace

void LoadBPReg(u8 reg, u32 value, int cycles_into_future)
{
  auto& system = Core::System::GetInstance();

  // BPMEM_BP_MASK limits which bits the next write may change; it then reverts to all bits.
  const u32 previous = BPWords()[reg];
  const u32 mask = bpmem.bpMask;
  const u32 merged = (previous & ~mask) | (value & mask);
  const BPCmd bp = {reg, static_cast<int>((previous ^ merged) & 0xFFFFFF),
                    static_cast<int>(merged)};
  if (reg != BPMEM_BP_MASK)
    bpmem.bpMask = 0xFFFFFF;

  BPWritten({system.GetPixelShaderManager(), system.GetXFStateManager(),
             system.GetGeometryShaderManager()},
            bp, cycles_into_future);
}

// Deterministic dual core: the CPU-side pass raises the PE events itself, at the right time.
// (A pending BPMEM_BP_MASK is not applied here.)
void LoadBPRegPreprocess(u8 reg, u32 value, int cycles_into_future)
{
  auto& pixel_engine = Core::System::GetInstance().GetPixelEngine();
  const u32 data = value & 0xffffff;
  const u16 token = static_cast<u16>(data & 0xffff);

  if (reg == BPMEM_SETDRAWDONE)
  {
    if ((data & 0xff) == 0x02)
      pixel_engine.SetFinish(cycles_into_future);
  }
  else if (reg == BPMEM_PE_TOKEN_ID || reg == BPMEM_PE_TOKEN_INT_ID)
  {
    pixel_engine.SetToken(token, reg == BPMEM_PE_TOKEN_INT_ID, cycles_into_future);
  }
}

// ---------------------------------------------------------------------------------------------
// Descriptions

namespace
{
// Names of the registers that are listed under a fixed name; nullptr elsewhere.
constexpr std::array<const char*, 256> MakeFixedNames()
{
  std::array<const char*, 256> names{};
  const auto name = [&names](int reg, const char* text) { names[static_cast<size_t>(reg)] = text; };
  name(BPMEM_GENMODE, "BPMEM_GENMODE");
  for (int i = 0; i < 4; ++i)
    name(BPMEM_DISPLAYCOPYFILTER + i, "BPMEM_DISPLAYCOPYFILTER");
  name(BPMEM_IND_IMASK, "BPMEM_IND_IMASK");
  name(BPMEM_SCISSORTL, "BPMEM_SCISSORTL");
  name(BPMEM_SCISSORBR, "BPMEM_SCISSORBR");
  name(BPMEM_LINEPTWIDTH, "BPMEM_LINEPTWIDTH");
  name(BPMEM_PERF0_TRI, "BPMEM_PERF0_TRI");
  name(BPMEM_PERF0_QUAD, "BPMEM_PERF0_QUAD");
  name(BPMEM_RAS1_SS0, "BPMEM_RAS1_SS0");
  name(BPMEM_RAS1_SS1, "BPMEM_RAS1_SS1");
  name(BPMEM_IREF, "BPMEM_IREF");
  name(BPMEM_ZMODE, "BPMEM_ZMODE");
  name(BPMEM_BLENDMODE, "BPMEM_BLENDMODE");
  name(BPMEM_CONSTANTALPHA, "BPMEM_CONSTANTALPHA");
  name(BPMEM_ZCOMPARE, "BPMEM_ZCOMPARE");
  name(BPMEM_FIELDMASK, "BPMEM_FIELDMASK");
  name(BPMEM_SETDRAWDONE, "BPMEM_SETDRAWDONE");
  name(BPMEM_BUSCLOCK0, "BPMEM_BUSCLOCK0");
  name(BPMEM_PE_TOKEN_ID, "BPMEM_PE_TOKEN_ID");
  name(BPMEM_PE_TOKEN_INT_ID, "BPMEM_PE_TOKEN_INT_ID");
  name(BPMEM_EFB_TL, "BPMEM_EFB_TL");
  name(BPMEM_EFB_WH, "BPMEM_EFB_WH");
  name(BPMEM_EFB_ADDR, "BPMEM_EFB_ADDR");
  name(BPMEM_EFB_STRIDE, "BPMEM_EFB_STRIDE");
  name(BPMEM_COPYYSCALE, "BPMEM_COPYYSCALE");
  name(BPMEM_CLEAR_AR, "BPMEM_CLEAR_AR");
  name(BPMEM_CLEAR_GB, "BPMEM_CLEAR_GB");
  name(BPMEM_CLEAR_Z, "BPMEM_CLEAR_Z");
  name(BPMEM_TRIGGER_EFB_COPY, "BPMEM_TRIGGER_EFB_COPY");
  name(BPMEM_COPYFILTER0, "BPMEM_COPYFILTER0");
  name(BPMEM_COPYFILTER1, "BPMEM_COPYFILTER1");
  name(BPMEM_CLEARBBOX1, "BPMEM_CLEARBBOX1");
  name(BPMEM_CLEARBBOX2, "BPMEM_CLEARBBOX2");
  name(BPMEM_CLEAR_PIXEL_PERF, "BPMEM_CLEAR_PIXEL_PERF");
  name(BPMEM_REVBITS, "BPMEM_REVBITS");
  name(BPMEM_SCISSOROFFSET, "BPMEM_SCISSOROFFSET");
  name(BPMEM_PRELOAD_ADDR, "BPMEM_PRELOAD_ADDR");
  name(BPMEM_PRELOAD_TMEMEVEN, "BPMEM_PRELOAD_TMEMEVEN");
  name(BPMEM_PRELOAD_TMEMODD, "BPMEM_PRELOAD_TMEMODD");
  name(BPMEM_PRELOAD_MODE, "BPMEM_PRELOAD_MODE");
  name(BPMEM_LOADTLUT0, "BPMEM_LOADTLUT0");
  name(BPMEM_LOADTLUT1, "BPMEM_LOADTLUT1");
  name(BPMEM_TEXINVALIDATE, "BPMEM_TEXINVALIDATE");
  name(BPMEM_PERF1, "BPMEM_PERF1");
  name(BPMEM_FIELDMODE, "BPMEM_FIELDMODE");
  name(BPMEM_BUSCLOCK1, "BPMEM_BUSCLOCK1");
  name(BPMEM_FOGPARAM0, "BPMEM_FOGPARAM0");
  name(BPMEM_FOGBMAGNITUDE, "BPMEM_FOGBMAGNITUDE");
  name(BPMEM_FOGBEXPONENT, "BPMEM_FOGBEXPONENT");
  name(BPMEM_FOGPARAM3, "BPMEM_FOGPARAM3");
  name(BPMEM_FOGCOLOR, "BPMEM_FOGCOLOR");
  name(BPMEM_ALPHACOMPARE, "BPMEM_ALPHACOMPARE");
  name(BPMEM_BIAS, "BPMEM_BIAS");
  name(BPMEM_ZTEX2, "BPMEM_ZTEX2");
  name(BPMEM_BP_MASK, "BPMEM_BP_MASK");
  return names;
}
constexpr auto FIXED_NAMES = MakeFixedNames();

// Description of a fixed-name register; empty for those without one.
std::string DescribeFixed(u8 cmd, u32 data)
{
  const auto high_low = [data](const char* high, const char* low) {
    return fmt::format("Clear color {}: 0x{:02X}\nClear color {}: 0x{:02X}", high,
                       (data & 0xFF00) >> 8, low, data & 0xFF);
  };
  const auto bbox_pair = [data](int first) {
    return fmt::format("Bounding Box index {}: {}\nBounding Box index {}: {}", first,
                       data & 0x3ff, first + 1, (data >> 10) & 0x3ff);
  };
  const auto six_bit = [data](int field) { return (data >> (6 * field)) & 0x3f; };

  switch (cmd)
  {
  case BPMEM_GENMODE:
    return fmt::to_string(GenMode{.hex = data});
  case BPMEM_SCISSORTL:
  case BPMEM_SCISSORBR:
    return fmt::to_string(ScissorPos{.hex = data});
  case BPMEM_LINEPTWIDTH:
    return fmt::to_string(LPSize{.hex = data});
  case BPMEM_RAS1_SS0:
  case BPMEM_RAS1_SS1:
    return fmt::to_string(std::make_pair(cmd, TEXSCALE{.hex = data}));
  case BPMEM_IREF:
    return fmt::to_string(RAS1_IREF{.hex = data});
  case BPMEM_ZMODE:
    return fmt::format("Z mode: {}", ZMode{.hex = data});
  case BPMEM_BLENDMODE:
    return fmt::to_string(BlendMode{.hex = data});
  case BPMEM_CONSTANTALPHA:
    return fmt::to_string(ConstantAlpha{.hex = data});
  case BPMEM_ZCOMPARE:
    return fmt::to_string(PEControl{.hex = data});
  case BPMEM_FIELDMASK:
    return fmt::to_string(FieldMask{.hex = data});
  case BPMEM_EFB_TL:
  {
    const X10Y10 corner{.hex = data};
    return fmt::format("EFB Left: {}\nEFB Top: {}", corner.x, corner.y);
  }
  case BPMEM_EFB_WH:
  {
    const X10Y10 size{.hex = data};
    return fmt::format("EFB Width: {}\nEFB Height: {}", size.x + 1, size.y + 1);
  }
  case BPMEM_EFB_ADDR:
    return fmt::format("EFB Target address (32 byte aligned): 0x{:06X}", data << 5);
  case BPMEM_EFB_STRIDE:
    return fmt::format("EFB destination stride (32 byte aligned): 0x{:06X}", data << 5);
  case BPMEM_COPYYSCALE:
  {
    const float scale = static_cast<float>(data);
    return fmt::format("Y scaling factor (XFB copy only): 0x{:X} ({}, reciprocal {})", data,
                       scale / 256.f, 256.f / scale);
  }
  case BPMEM_CLEAR_AR:
    return high_low("alpha", "red");
  case BPMEM_CLEAR_GB:
    return high_low("green", "blue");
  case BPMEM_CLEAR_Z:
    return fmt::format("Clear Z value: 0x{:06X}", data);
  case BPMEM_TRIGGER_EFB_COPY:
    return fmt::to_string(UPE_Copy{.Hex = data});
  case BPMEM_COPYFILTER0:
    return fmt::format("w0: {}\nw1: {}\nw2: {}\nw3: {}", six_bit(0), six_bit(1), six_bit(2),
                       six_bit(3));
  case BPMEM_COPYFILTER1:
    // Seven coefficients in total; this register holds the last three.
    return fmt::format("w4: {}\nw5: {}\nw6: {}", six_bit(0), six_bit(1), six_bit(2));
  case BPMEM_CLEARBBOX1:
    return bbox_pair(0);
  case BPMEM_CLEARBBOX2:
    return bbox_pair(2);
  case BPMEM_SCISSOROFFSET:
    return fmt::to_string(ScissorOffset{.hex = data});
  case BPMEM_PRELOAD_ADDR:
    return fmt::format("Tmem preload address (32 byte aligned, in main memory): 0x{:06x}",
                       data << 5);
  case BPMEM_PRELOAD_TMEMEVEN:
    return fmt::format("Tmem preload even line: 0x{:04x} (byte 0x{:05x})", data,
                       data * TMEM_LINE_SIZE);
  case BPMEM_PRELOAD_TMEMODD:
    return fmt::format("Tmem preload odd line: 0x{:04x} (byte 0x{:05x})", data,
                       data * TMEM_LINE_SIZE);
  case BPMEM_PRELOAD_MODE:
    return fmt::to_string(BPU_PreloadTileInfo{.hex = data});
  case BPMEM_LOADTLUT0:
    return fmt::format("TLUT load address (32 byte aligned, in main memory): 0x{:06x}",
                       data << 5);
  case BPMEM_LOADTLUT1:
    return fmt::to_string(BPU_LoadTlutInfo{.hex = data});
  case BPMEM_FIELDMODE:
    return fmt::to_string(FieldMode{.hex = data});
  case BPMEM_FOGPARAM0:
    return fmt::to_string(FogParam0{.hex = data});
  case BPMEM_FOGBMAGNITUDE:
    return fmt::format("B magnitude: {}", data);
  case BPMEM_FOGBEXPONENT:
    return fmt::format("B shift: 1>>{} (1/{})", data, 1 << data);
  case BPMEM_FOGPARAM3:
    return fmt::to_string(FogParam3{.hex = data});
  case BPMEM_FOGCOLOR:
    return fmt::to_string(FogParams::FogColor{.hex = data});
  case BPMEM_ALPHACOMPARE:
    return fmt::to_string(AlphaTest{.hex = data});
  case BPMEM_BIAS:
    return fmt::to_string(ZTex1{.hex = data});
  case BPMEM_ZTEX2:
    return fmt::to_string(ZTex2{.hex = data});
  case BPMEM_BP_MASK:
    return fmt::format("The next BP command will only update these bits; others will retain "
                       "their prior values: {:06x}",
                       data);
  default:
    // Performance, clock, draw done, token and other command registers.
    return "";
  }
}

// Texture unit registers: seven kinds, four units in each of two banks.
std::optional<std::pair<std::string, std::string>> DescribeTextureUnit(u8 cmd, u32 data)
{
  if (cmd < BPMEM_TX_SETMODE0 || cmd >= BPMEM_TEV_COLOR_ENV)
    return std::nullopt;
  const bool upper_bank = cmd >= BPMEM_TX_SETMODE0_4;
  const int kind = ((cmd & 0x1f) >> 2);
  if (kind == 7)
    return std::nullopt;
  static constexpr std::array<const char*, 7> kinds = {
      "SETMODE0", "SETMODE1", "SETIMAGE0", "SETIMAGE1", "SETIMAGE2", "SETIMAGE3", "SETTLUT"};
  const int unit = (cmd & 3) + (upper_bank ? 4 : 0);
  std::string name = fmt::format("BPMEM_TX_{}{} Texture Unit {}", kinds[static_cast<size_t>(kind)],
                                 upper_bank ? "_4" : "", unit);
  std::string desc;
  switch (kind)
  {
  case 0:
    desc = fmt::to_string(TexMode0{.hex = data});
    break;
  case 1:
    desc = fmt::to_string(TexMode1{.hex = data});
    break;
  case 2:
    desc = fmt::to_string(TexImage0{.hex = data});
    break;
  case 3:
    desc = fmt::to_string(TexImage1{.hex = data});
    break;
  case 4:
    desc = fmt::to_string(TexImage2{.hex = data});
    break;
  case 5:
    desc = fmt::to_string(TexImage3{.hex = data});
    break;
  default:
    desc = fmt::to_string(TexTLUT{.hex = data});
    break;
  }
  return std::make_pair(std::move(name), std::move(desc));
}

// Everything addressed by block other than the texture units.
std::optional<std::pair<std::string, std::string>> DescribeBlock(u8 cmd, u32 data)
{
  const auto in = [cmd](int first, int count) { return cmd >= first && cmd < first + count; };

  if (in(BPMEM_IND_MTXA, 9))
  {
    const int matrix = (cmd - BPMEM_IND_MTXA) / 3;
    switch ((cmd - BPMEM_IND_MTXA) % 3)
    {
    case 0:
      return std::make_pair(fmt::format("BPMEM_IND_MTXA Matrix {}", matrix),
                            fmt::format("Matrix {} column A\n{}", matrix, IND_MTXA{.hex = data}));
    case 1:
      return std::make_pair(fmt::format("BPMEM_IND_MTXB Matrix {}", matrix),
                            fmt::format("Matrix {} column B\n{}", matrix, IND_MTXB{.hex = data}));
    default:
      return std::make_pair(fmt::format("BPMEM_IND_MTXC Matrix {}", matrix),
                            fmt::format("Matrix {} column C\n{}", matrix, IND_MTXC{.hex = data}));
    }
  }
  if (in(BPMEM_IND_CMD, 16))
  {
    return std::make_pair(fmt::format("BPMEM_IND_CMD number {}", cmd - BPMEM_IND_CMD),
                          fmt::to_string(TevStageIndirect{.fullhex = data}));
  }
  if (in(BPMEM_TREF, 8))
  {
    return std::make_pair(fmt::format("BPMEM_TREF number {}", cmd - BPMEM_TREF),
                          fmt::to_string(std::make_pair(cmd, TwoTevStageOrders{.hex = data})));
  }
  if (in(BPMEM_SU_SSIZE, 16))
  {
    // S and T sizes alternate.
    const bool is_s = ((cmd - BPMEM_SU_SSIZE) & 1) == 0;
    const int texcoord = (cmd - BPMEM_SU_SSIZE) / 2;
    return std::make_pair(
        fmt::format("BPMEM_SU_{}SIZE number {}", is_s ? 'S' : 'T', texcoord),
        fmt::to_string(std::make_pair(is_s, TCInfo{.hex = data})));
  }
  if (in(BPMEM_TEV_COLOR_ENV, 32))
  {
    const int stage = (cmd - BPMEM_TEV_COLOR_ENV) / 2;
    if (((cmd - BPMEM_TEV_COLOR_ENV) & 1) == 0)
    {
      return std::make_pair(fmt::format("BPMEM_TEV_COLOR_ENV Tev stage {}", stage),
                            fmt::to_string(TevStageCombiner::ColorCombiner{.hex = data}));
    }
    return std::make_pair(fmt::format("BPMEM_TEV_ALPHA_ENV Tev stage {}", stage),
                          fmt::to_string(TevStageCombiner::AlphaCombiner{.hex = data}));
  }
  if (in(BPMEM_TEV_COLOR_RA, 8))
  {
    const int reg = (cmd - BPMEM_TEV_COLOR_RA) / 2;
    if (((cmd - BPMEM_TEV_COLOR_RA) & 1) == 0)
    {
      return std::make_pair(fmt::format("BPMEM_TEV_COLOR_RA Tev register {}", reg),
                            fmt::to_string(TevReg::RA{.hex = data}));
    }
    return std::make_pair(fmt::format("BPMEM_TEV_COLOR_BG Tev register {}", reg),
                          fmt::to_string(TevReg::BG{.hex = data}));
  }
  if (cmd == BPMEM_FOGRANGE)
  {
    return std::make_pair(std::string("BPMEM_FOGRANGE Base"),
                          fmt::to_string(FogRangeParams::RangeBase{.hex = data}));
  }
  if (in(BPMEM_FOGRANGE + 1, 5))
  {
    return std::make_pair(fmt::format("BPMEM_FOGRANGE K element {}", cmd - BPMEM_FOGRANGE),
                          fmt::to_string(FogRangeKElement{.HEX = data}));
  }
  if (in(BPMEM_TEV_KSEL, 8))
  {
    return std::make_pair(fmt::format("BPMEM_TEV_KSEL number {}", cmd - BPMEM_TEV_KSEL),
                          fmt::to_string(std::make_pair(cmd, TevKSel{.hex = data})));
  }
  return DescribeTextureUnit(cmd, data);
}
}  // namespace

std::pair<std::string, std::string> GetBPRegInfo(u8 cmd, u32 cmddata)
{
  if (const char* const name = FIXED_NAMES[cmd])
    return {name, DescribeFixed(cmd, cmddata)};
  if (auto block = DescribeBlock(cmd, cmddata))
    return std::move(*block);
  return {fmt::format("Unknown BP Reg: {:02x}={:06x}", cmd, cmddata), ""};
}

// After a savestate load: push the BP state that lives in the backend (rather than in shader
// constants, which have their own savestate) back out. Nothing is replayed through BPWritten.
void BPReload()
{
  BPFunctions::SetGenerationMode();
  BPFunctions::SetScissorAndViewport(g_framebuffer_manager.get(), bpmem.scissorTL,
                                     bpmem.scissorBR, bpmem.scissorOffset, xfmem.viewport);
  BPFunctions::SetDepthMode();
  BPFunctions::SetBlendMode();
  BPFunctions::OnPixelFormatChange(g_framebuffer_manager.get(), bpmem.zcontrol.pixel_format,
                                   bpmem.zcontrol.zformat);
}
