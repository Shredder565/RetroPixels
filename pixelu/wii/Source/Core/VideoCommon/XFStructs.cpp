// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// XF (transform unit) loads: plain register-block writes, indexed loads from CP arrays, and the
// text descriptions the FIFO tools show for them.

#include "VideoCommon/XFStructs.h"

#include <array>
#include <bit>
#include <iterator>
#include <string>
#include <utility>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/Swap.h"

#include "Core/DolphinAnalytics.h"
#include "Core/HW/Memmap.h"
#include "Core/System.h"

#include "VideoCommon/CPMemory.h"
#include "VideoCommon/Fifo.h"
#include "VideoCommon/GeometryShaderManager.h"
#include "VideoCommon/PixelShaderManager.h"
#include "VideoCommon/VertexLoaderManager.h"
#include "VideoCommon/VertexManagerBase.h"
#include "VideoCommon/XFMemory.h"
#include "VideoCommon/XFStateManager.h"

namespace
{
constexpr u32 VIEWPORT_WORDS = 6;
constexpr u32 PROJECTION_WORDS = 7;
constexpr u32 TEXGEN_SLOTS = 8;

// XF memory viewed as its 32-bit address space.
u32* XFWords()
{
  return reinterpret_cast<u32*>(&xfmem);
}

bool InBlock(u32 address, u32 first, u32 count)
{
  return address >= first && address < first + count;
}

bool IsMatrixMemory(u32 address)
{
  return InBlock(address, XFMEM_POSMATRICES, XFMEM_POSMATRICES_END - XFMEM_POSMATRICES) ||
         InBlock(address, XFMEM_NORMALMATRICES, XFMEM_NORMALMATRICES_END - XFMEM_NORMALMATRICES) ||
         InBlock(address, XFMEM_POSTMATRICES, XFMEM_POSTMATRICES_END - XFMEM_POSTMATRICES);
}

void ReportUnknownXFRegister()
{
  DolphinAnalytics::Instance().ReportGameQuirk(GameQuirk::UsesUnknownXFCommand);
}

// Any write into the matrix/light memory: draw what is queued, then mark the words dirty.
void NoteMemoryWrite(XFStateManager& tracker, u32 first, u32 count)
{
  g_vertex_manager->Flush();
  tracker.InvalidateXFRange(static_cast<int>(first), static_cast<int>(first + count));
}

// Reacts to a register write before the new value is stored, so handlers can still compare
// against the old value.
void BeforeRegisterWrite(Core::System& system, XFStateManager& tracker, u32 address, u32 value)
{
  if (InBlock(address, XFMEM_SETVIEWPORT, VIEWPORT_WORDS))
  {
    g_vertex_manager->Flush();
    tracker.SetViewportChanged();
    system.GetPixelShaderManager().SetViewportChanged();
    system.GetGeometryShaderManager().SetViewportChanged();
    return;
  }
  if (InBlock(address, XFMEM_SETPROJECTION, PROJECTION_WORDS))
  {
    g_vertex_manager->Flush();
    tracker.SetProjectionChanged();
    system.GetGeometryShaderManager().SetProjectionChanged();
    return;
  }
  if (InBlock(address, XFMEM_SETTEXMTXINFO, TEXGEN_SLOTS))
  {
    g_vertex_manager->Flush();
    tracker.SetTexMatrixInfoChanged(static_cast<int>(address - XFMEM_SETTEXMTXINFO));
    return;
  }
  if (InBlock(address, XFMEM_SETPOSTMTXINFO, TEXGEN_SLOTS))
  {
    g_vertex_manager->Flush();
    tracker.SetTexMatrixInfoChanged(static_cast<int>(address - XFMEM_SETPOSTMTXINFO));
    return;
  }
  if (InBlock(address, XFMEM_UNKNOWN_GROUP_3_START, 8))
  {
    // Possibly per-texgen normal matrix info; never seen used.
    ReportUnknownXFRegister();
    DEBUG_LOG_FMT(VIDEO, "Possible Normal Mtx XF reg?: {:x}={:x}", address, value);
    return;
  }

  switch (address)
  {
  // Status, diagnostics and performance registers: nothing to emulate.
  case XFMEM_ERROR:
  case XFMEM_DIAG:
  case XFMEM_STATE0:
  case XFMEM_STATE1:
  case XFMEM_CLOCK:
  case XFMEM_SETGPMETRIC:
    return;

  case XFMEM_CLIPDISABLE:
  {
    // Clipping is not configurable here; just note which games touch the bits.
    const ClipDisable bits{.hex = value};
    auto& analytics = DolphinAnalytics::Instance();
    if (bits.disable_clipping_detection)
      analytics.ReportGameQuirk(GameQuirk::SetsXFClipDisableBit0);
    if (bits.disable_trivial_rejection)
      analytics.ReportGameQuirk(GameQuirk::SetsXFClipDisableBit1);
    if (bits.disable_cpoly_clipping_acceleration)
      analytics.ReportGameQuirk(GameQuirk::SetsXFClipDisableBit2);
    return;
  }

  case XFMEM_VTXSPECS:
    VertexLoaderManager::g_needs_cp_xf_consistency_check = true;
    return;

  case XFMEM_SETNUMCHAN:
    if (xfmem.numChan.numColorChans != (value & 3))
      g_vertex_manager->Flush();
    tracker.SetLightingConfigChanged();
    return;

  case XFMEM_SETCHAN0_AMBCOLOR:
  case XFMEM_SETCHAN1_AMBCOLOR:
  case XFMEM_SETCHAN0_MATCOLOR:
  case XFMEM_SETCHAN1_MATCOLOR:
  {
    // Material change bits: 0-1 ambient, 2-3 material colour, indexed by channel.
    const u32 slot = address - XFMEM_SETCHAN0_AMBCOLOR;
    if (XFWords()[address] != value)
    {
      g_vertex_manager->Flush();
      tracker.SetMaterialColorChanged(static_cast<int>(slot));
    }
    return;
  }

  case XFMEM_SETCHAN0_COLOR:
  case XFMEM_SETCHAN1_COLOR:
  case XFMEM_SETCHAN0_ALPHA:
  case XFMEM_SETCHAN1_ALPHA:
    if (XFWords()[address] != (value & 0x7fff))
      g_vertex_manager->Flush();
    tracker.SetLightingConfigChanged();
    return;

  case XFMEM_DUALTEX:
    if (xfmem.dualTexTrans.enabled != ((value & 1) != 0))
      g_vertex_manager->Flush();
    tracker.SetTexMatrixInfoChanged(-1);
    return;

  case XFMEM_SETMATRIXINDA:
    tracker.SetTexMatrixChangedA(value);
    VertexLoaderManager::g_needs_cp_xf_consistency_check = true;
    return;

  case XFMEM_SETMATRIXINDB:
    tracker.SetTexMatrixChangedB(value);
    VertexLoaderManager::g_needs_cp_xf_consistency_check = true;
    return;

  case XFMEM_SETNUMTEXGENS:
    if (xfmem.numTexGen.numTexGens != (value & 15))
      g_vertex_manager->Flush();
    return;

  default:
    ReportUnknownXFRegister();
    WARN_LOG_FMT(VIDEO, "Unknown XF Reg: {:x}={:x}", address, value);
    return;
  }
}

// Where a block write lands: the part below the register file goes to matrix/light memory,
// the rest to registers. `regs_end` is already clamped to the end of the register file.
struct TransferSplit
{
  u32 memory_words;  // words written to memory, starting at the base address
  u32 regs_first;    // first register written (== regs_end when none)
  u32 regs_end;
};

TransferSplit SplitTransfer(u32 base, u32 count, u32 clamped_end)
{
  if (base >= XFMEM_REGISTERS_START)
    return {0, base, clamped_end};
  if (clamped_end > XFMEM_REGISTERS_START)
    return {XFMEM_REGISTERS_START - base, XFMEM_REGISTERS_START, clamped_end};
  return {count, clamped_end, clamped_end};
}

// True if any of the `count` big-endian words at `src` differs from XF memory at `address`.
bool DiffersFromXF(u32 address, const u8* src, u32 count)
{
  const u32* const current = XFWords() + address;
  for (u32 i = 0; i < count; ++i)
  {
    if (current[i] != Common::swap32(src + i * 4))
      return true;
  }
  return false;
}

// Indexed loads read from main memory at array base + stride * index.
u32 IndexedSource(const CPState& state, CPArray array, u32 index)
{
  return state.array_bases[array] + state.array_strides[array] * index;
}
}  // namespace

void LoadXFReg(u16 base_address, u8 transfer_size, const u8* data)
{
  if (base_address > XFMEM_REGISTERS_END)
  {
    WARN_LOG_FMT(VIDEO, "XF load base address past end of address space: {:x} {} bytes",
                 base_address, transfer_size);
    return;
  }

  u32 end = u32{base_address} + transfer_size;
  if (end > XFMEM_REGISTERS_END)
  {
    WARN_LOG_FMT(VIDEO, "XF load ends past end of address space: {:x} {} bytes", base_address,
                 transfer_size);
    end = XFMEM_REGISTERS_END;
  }

  auto& system = Core::System::GetInstance();
  auto& tracker = system.GetXFStateManager();
  const TransferSplit split = SplitTransfer(base_address, transfer_size, end);

  const u8* src = data;
  if (base_address < XFMEM_REGISTERS_START)
  {
    NoteMemoryWrite(tracker, base_address, split.memory_words);
    u32* const dest = XFWords() + base_address;
    for (u32 i = 0; i < split.memory_words; ++i, src += 4)
      dest[i] = Common::swap32(src);
  }

  for (u32 address = split.regs_first; address < split.regs_end; ++address, src += 4)
  {
    const u32 value = Common::swap32(src);
    BeforeRegisterWrite(system, tracker, address, value);
    XFWords()[address] = value;
  }
}

void LoadIndexedXF(CPArray array, u32 index, u16 address, u8 size)
{
  const u32 byte_count = u32{size} * sizeof(u32);
  auto& system = Core::System::GetInstance();
  auto& fifo = system.GetFifo();

  // In deterministic mode the CPU thread already captured the source words.
  const u8* const src =
      fifo.UseDeterministicGPUThread() ?
          static_cast<const u8*>(fifo.PopFifoAuxBuffer(byte_count)) :
          system.GetMemory().GetPointerForRange(IndexedSource(g_main_cp_state, array, index),
                                                byte_count);
  // An unmapped source would otherwise be dereferenced; ignore the load instead.
  if (src == nullptr)
    return;

  if (!DiffersFromXF(address, src, size))
    return;

  NoteMemoryWrite(system.GetXFStateManager(), address, size);
  u32* const dest = XFWords() + address;
  for (u32 i = 0; i < size; ++i)
    dest[i] = Common::swap32(src + i * 4);
}

void PreprocessIndexedXF(CPArray array, u32 index, [[maybe_unused]] u16 address, u8 size)
{
  const size_t byte_count = size_t{size} * sizeof(u32);
  auto& system = Core::System::GetInstance();
  const u8* const src = system.GetMemory().GetPointerForRange(
      IndexedSource(g_preprocess_cp_state, array, index), byte_count);
  system.GetFifo().PushFifoAuxBuffer(src, byte_count);
}

namespace
{
// Fixed register names (registers 0x1000-0x103f); nullptr where the register has none.
constexpr std::array<const char*, 0x40> MakeRegisterNames()
{
  std::array<const char*, 0x40> names{};
  const auto set = [&names](u32 address, const char* name) {
    names[address - XFMEM_REGISTERS_START] = name;
  };
  set(XFMEM_ERROR, "XFMEM_ERROR");
  set(XFMEM_DIAG, "XFMEM_DIAG");
  set(XFMEM_STATE0, "XFMEM_STATE0");
  set(XFMEM_STATE1, "XFMEM_STATE1");
  set(XFMEM_CLOCK, "XFMEM_CLOCK");
  set(XFMEM_CLIPDISABLE, "XFMEM_CLIPDISABLE");
  set(XFMEM_SETGPMETRIC, "XFMEM_SETGPMETRIC");
  set(XFMEM_VTXSPECS, "XFMEM_VTXSPECS");
  set(XFMEM_SETNUMCHAN, "XFMEM_SETNUMCHAN");
  set(XFMEM_SETCHAN0_AMBCOLOR, "XFMEM_SETCHAN0_AMBCOLOR");
  set(XFMEM_SETCHAN1_AMBCOLOR, "XFMEM_SETCHAN1_AMBCOLOR");
  set(XFMEM_SETCHAN0_MATCOLOR, "XFMEM_SETCHAN0_MATCOLOR");
  set(XFMEM_SETCHAN1_MATCOLOR, "XFMEM_SETCHAN1_MATCOLOR");
  set(XFMEM_SETCHAN0_COLOR, "XFMEM_SETCHAN0_COLOR");
  set(XFMEM_SETCHAN1_COLOR, "XFMEM_SETCHAN1_COLOR");
  set(XFMEM_SETCHAN0_ALPHA, "XFMEM_SETCHAN0_ALPHA");
  set(XFMEM_SETCHAN1_ALPHA, "XFMEM_SETCHAN1_ALPHA");
  set(XFMEM_DUALTEX, "XFMEM_DUALTEX");
  set(XFMEM_SETMATRIXINDA, "XFMEM_SETMATRIXINDA");
  set(XFMEM_SETMATRIXINDB, "XFMEM_SETMATRIXINDB");
  constexpr std::array<const char*, VIEWPORT_WORDS> viewport = {
      "XFMEM_SETVIEWPORT + 0", "XFMEM_SETVIEWPORT + 1", "XFMEM_SETVIEWPORT + 2",
      "XFMEM_SETVIEWPORT + 3", "XFMEM_SETVIEWPORT + 4", "XFMEM_SETVIEWPORT + 5"};
  for (u32 i = 0; i < VIEWPORT_WORDS; ++i)
    set(XFMEM_SETVIEWPORT + i, viewport[i]);
  constexpr std::array<const char*, PROJECTION_WORDS> projection = {
      "XFMEM_SETPROJECTION + 0", "XFMEM_SETPROJECTION + 1", "XFMEM_SETPROJECTION + 2",
      "XFMEM_SETPROJECTION + 3", "XFMEM_SETPROJECTION + 4", "XFMEM_SETPROJECTION + 5",
      "XFMEM_SETPROJECTION + 6"};
  for (u32 i = 0; i < PROJECTION_WORDS; ++i)
    set(XFMEM_SETPROJECTION + i, projection[i]);
  set(XFMEM_SETNUMTEXGENS, "XFMEM_SETNUMTEXGENS");
  return names;
}
constexpr auto REGISTER_NAMES = MakeRegisterNames();

// Description for a register that has a fixed name.
std::string DescribeNamedRegister(u32 address, u32 value)
{
  const float as_float = std::bit_cast<float>(value);

  if (InBlock(address, XFMEM_SETVIEWPORT, VIEWPORT_WORDS))
  {
    static constexpr std::array<const char*, VIEWPORT_WORDS> fields = {
        "width", "height", "z range", "x origin", "y origin", "far z"};
    return fmt::format("Viewport {}: {}", fields[address - XFMEM_SETVIEWPORT], as_float);
  }
  if (address == XFMEM_SETPROJECTION + PROJECTION_WORDS - 1)
    return fmt::to_string(static_cast<ProjectionType>(value));
  if (InBlock(address, XFMEM_SETPROJECTION, PROJECTION_WORDS))
    return fmt::format("Projection[{}]: {}", address - XFMEM_SETPROJECTION, as_float);

  switch (address)
  {
  case XFMEM_STATE0:
    return "internal state 0";
  case XFMEM_STATE1:
    return "internal state 1";
  case XFMEM_CLIPDISABLE:
    return fmt::to_string(ClipDisable{.hex = value});
  case XFMEM_VTXSPECS:
    return fmt::to_string(INVTXSPEC{.hex = value});
  case XFMEM_SETNUMCHAN:
    return fmt::format("Number of color channels: {}", value & 3);
  case XFMEM_SETCHAN0_AMBCOLOR:
  case XFMEM_SETCHAN1_AMBCOLOR:
    return fmt::format("Channel {} Ambient Color: {:08x}", address - XFMEM_SETCHAN0_AMBCOLOR,
                       value);
  case XFMEM_SETCHAN0_MATCOLOR:
  case XFMEM_SETCHAN1_MATCOLOR:
    return fmt::format("Channel {} Material Color: {:08x}", address - XFMEM_SETCHAN0_MATCOLOR,
                       value);
  case XFMEM_SETCHAN0_COLOR:
  case XFMEM_SETCHAN1_COLOR:
    return fmt::format("Channel {} Color config:\n{}", address - XFMEM_SETCHAN0_COLOR,
                       LitChannel{.hex = value});
  case XFMEM_SETCHAN0_ALPHA:
  case XFMEM_SETCHAN1_ALPHA:
    return fmt::format("Channel {} Alpha config:\n{}", address - XFMEM_SETCHAN0_ALPHA,
                       LitChannel{.hex = value});
  case XFMEM_DUALTEX:
    return fmt::format("Dual Tex Trans {}", (value & 1) != 0 ? "enabled" : "disabled");
  case XFMEM_SETMATRIXINDA:
    return fmt::format("Matrix index A:\n{}", TMatrixIndexA{.Hex = value});
  case XFMEM_SETMATRIXINDB:
    return fmt::format("Matrix index B:\n{}", TMatrixIndexB{.Hex = value});
  case XFMEM_SETNUMTEXGENS:
    return fmt::format("Number of tex gens: {}", value & 15);
  default:
    // Error, diag, clock and metric registers.
    return "";
  }
}
}  // namespace

std::pair<std::string, std::string> GetXFRegInfo(u32 address, u32 value)
{
  if (InBlock(address, XFMEM_SETTEXMTXINFO, TEXGEN_SLOTS))
  {
    return {fmt::format("XFMEM_SETTEXMTXINFO Matrix {}", address - XFMEM_SETTEXMTXINFO),
            fmt::to_string(TexMtxInfo{.hex = value})};
  }
  if (InBlock(address, XFMEM_SETPOSTMTXINFO, TEXGEN_SLOTS))
  {
    return {fmt::format("XFMEM_SETPOSTMTXINFO Matrix {}", address - XFMEM_SETPOSTMTXINFO),
            fmt::to_string(PostMtxInfo{.hex = value})};
  }
  if (InBlock(address, XFMEM_UNKNOWN_GROUP_3_START, 8))
  {
    return {fmt::format("Possible Normal Mtx XF reg?: {:x}={:x}", address, value),
            "Maybe these are for Normals? xfmem.texcoords[0].nrmmtxinfo.hex = data; break; ??"};
  }
  if (InBlock(address, XFMEM_REGISTERS_START, static_cast<u32>(REGISTER_NAMES.size())))
  {
    const char* const name = REGISTER_NAMES[address - XFMEM_REGISTERS_START];
    if (name != nullptr)
      return {name, DescribeNamedRegister(address, value)};
  }
  return {fmt::format("Unknown XF Reg: {:x}={:x}", address, value), ""};
}

std::string GetXFMemName(u32 address)
{
  // Matrices are named by row and column (four columns, three for normal matrices).
  const auto matrix = [address](const char* kind, u32 base, u32 columns) {
    const u32 offset = address - base;
    return fmt::format("{} matrix row {:2d} col {:2d}", kind, offset / columns, offset % columns);
  };

  if (InBlock(address, XFMEM_POSMATRICES, XFMEM_POSMATRICES_END - XFMEM_POSMATRICES))
    return matrix("Position", XFMEM_POSMATRICES, 4);
  if (InBlock(address, XFMEM_NORMALMATRICES, XFMEM_NORMALMATRICES_END - XFMEM_NORMALMATRICES))
    return matrix("Normal", XFMEM_NORMALMATRICES, 3);
  if (InBlock(address, XFMEM_POSTMATRICES, XFMEM_POSTMATRICES_END - XFMEM_POSTMATRICES))
    return matrix("Post", XFMEM_POSTMATRICES, 4);
  if (!InBlock(address, XFMEM_LIGHTS, XFMEM_LIGHTS_END - XFMEM_LIGHTS))
    return fmt::format("Unknown memory {:04x}", address);

  // Lights are 16 words each: 3 unused, colour, 3 cosine and 3 distance attenuation factors,
  // then two xyz vectors whose meaning depends on the light type.
  const u32 light = (address - XFMEM_LIGHTS) / 16;
  const u32 word = (address - XFMEM_LIGHTS) % 16;
  if (word < 3)
    return fmt::format("Light {} unused param {}", light, word);
  if (word == 3)
    return fmt::format("Light {} color", light);
  if (word < 7)
    return fmt::format("Light {} cosine attenuation {}", light, word - 4);
  if (word < 10)
    return fmt::format("Light {} distance attenuation {}", light, word - 7);
  if (word < 13)
    return fmt::format("Light {0} {1} position or inf ldir {1}", light, "xyz"[word - 10]);
  return fmt::format("Light {0} {1} direction or half angle {1}", light, "xyz"[word - 13]);
}

std::string GetXFMemDescription(u32 address, u32 value)
{
  // Matrix words and light parameters from the attenuation factors on are floats; light
  // colours, the unused light words and anything unknown are shown as hex.
  bool is_float = IsMatrixMemory(address);
  if (InBlock(address, XFMEM_LIGHTS, XFMEM_LIGHTS_END - XFMEM_LIGHTS))
    is_float = (address - XFMEM_LIGHTS) % 16 > 3;

  if (is_float)
    return fmt::format("{} = {}", GetXFMemName(address), std::bit_cast<float>(value));
  return fmt::format("{} = {:08x}", GetXFMemName(address), value);
}

std::pair<std::string, std::string> GetXFTransferInfo(u16 base_address, u8 transfer_size,
                                                      const u8* data)
{
  if (base_address > XFMEM_REGISTERS_END)
    return {"Invalid XF Transfer", "Base address past end of address space"};

  // A single register write is described just like the register itself.
  if (transfer_size == 1 && base_address >= XFMEM_REGISTERS_START)
    return GetXFRegInfo(base_address, Common::swap32(data));

  std::string name;
  std::string desc;
  auto name_out = std::back_inserter(name);
  auto desc_out = std::back_inserter(desc);

  u32 end = u32{base_address} + transfer_size;
  if (end > XFMEM_REGISTERS_END)
  {
    name += "Invalid XF Transfer ";
    desc += "Transfer ends past end of address space\n\n";
    end = XFMEM_REGISTERS_END;
  }

  const TransferSplit split = SplitTransfer(base_address, transfer_size, end);
  const u8* src = data;

  if (base_address < XFMEM_REGISTERS_START)
  {
    fmt::format_to(name_out, "Write {} XF mem words at {:04x}", split.memory_words, base_address);
    for (u32 i = 0; i < split.memory_words; ++i, src += 4)
    {
      if (i != 0)
        desc += '\n';
      desc += GetXFMemDescription(base_address + i, Common::swap32(src));
    }
    if (end > XFMEM_REGISTERS_START)
      name += "; ";
  }

  // Registers are listed (even zero of them) unless the whole transfer stayed in memory.
  if (base_address >= XFMEM_REGISTERS_START || end > XFMEM_REGISTERS_START)
  {
    fmt::format_to(name_out, "Write {} XF regs at {:04x}", split.regs_end - split.regs_first,
                   split.regs_first);
    for (u32 address = split.regs_first; address < split.regs_end; ++address, src += 4)
    {
      const auto [reg_name, reg_desc] = GetXFRegInfo(address, Common::swap32(src));
      fmt::format_to(desc_out, "{}\n{}\n", reg_name, reg_desc);
    }
  }

  return {std::move(name), std::move(desc)};
}

std::pair<std::string, std::string> GetXFIndexedLoadInfo(CPArray array, u32 index, u16 address,
                                                         u8 size)
{
  std::string targets;
  for (u32 i = 0; i < size; ++i)
  {
    targets += GetXFMemName(address + i);
    targets += '\n';
  }
  return {fmt::format("Load {} words to XF address {:03x} from CP array {} row {}", size, address,
                      array, index),
          std::move(targets)};
}
