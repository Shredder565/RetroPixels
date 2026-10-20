// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/CPMemory.h"

#include <cstring>
#include <type_traits>
#include <utility>

#include "Common/Logging/Log.h"
#include "Core/DolphinAnalytics.h"
#include "Core/System.h"
#include "VideoCommon/CommandProcessor.h"

CPState g_main_cp_state;
CPState g_preprocess_cp_state;

void CopyPreprocessCPStateFromMain()
{
  static_assert(std::is_trivially_copyable_v<CPState>);
  std::memcpy(static_cast<void*>(&g_preprocess_cp_state),
              static_cast<const void*>(&g_main_cp_state), sizeof(CPState));
}

namespace
{
void ReportQuirk(GameQuirk quirk)
{
  DolphinAnalytics::Instance().ReportGameQuirk(quirk);
}

// The three VAT words, by register group.
constexpr std::array<u8, 3> VAT_GROUP_REGS = {CP_VAT_REG_A, CP_VAT_REG_B, CP_VAT_REG_C};

// Index of a VAT register group (0 for A, 1 for B, 2 for C), or -1.
int VatGroupOf(u8 command)
{
  for (size_t g = 0; g < VAT_GROUP_REGS.size(); ++g)
  {
    if (command == VAT_GROUP_REGS[g])
      return static_cast<int>(g);
  }
  return -1;
}

u32& VatWord(VAT& vat, int group)
{
  return group == 0 ? vat.g0.Hex : group == 1 ? vat.g1.Hex : vat.g2.Hex;
}

// Calls fn(register number, field) for every register the state keeps; `State` may be const.
template <typename State, typename Fn>
void ForEachRegister(State& state, Fn&& fn)
{
  fn(MATINDEX_A, state.matrix_index_a.Hex);
  fn(MATINDEX_B, state.matrix_index_b.Hex);
  fn(VCD_LO, state.vtx_desc.low.Hex);
  fn(VCD_HI, state.vtx_desc.high.Hex);
  for (u32 n = 0; n < CP_NUM_VAT_REG; ++n)
  {
    fn(CP_VAT_REG_A + n, state.vtx_attr[n].g0.Hex);
    fn(CP_VAT_REG_B + n, state.vtx_attr[n].g1.Hex);
    fn(CP_VAT_REG_C + n, state.vtx_attr[n].g2.Hex);
  }
  for (u32 n = 0; n < CP_NUM_ARRAYS; ++n)
  {
    const auto array = static_cast<CPArray>(n);
    fn(ARRAY_BASE + n, state.array_bases[array]);
    fn(ARRAY_STRIDE + n, state.array_strides[array]);
  }
}

// For registers that are only defined at the exact base number (low nybble 0).
void CheckExactRegister(u8 sub_cmd, u8 expected, const char* name)
{
  if (sub_cmd == expected)
    return;
  ReportQuirk(GameQuirk::UsesMaybeInvalidCPCommand);
  WARN_LOG_FMT(VIDEO, "CP {}: an exact value of {:02x} was expected but instead a value of {:02x} "
                      "was seen",
               name, expected, sub_cmd);
}
}  // namespace

std::pair<std::string, std::string> GetCPRegInfo(u8 cmd, u32 value)
{
  const u8 reg = cmd & CP_COMMAND_MASK;

  const int vat_group = VatGroupOf(reg);
  if (vat_group >= 0)
  {
    const char letter = static_cast<char>('A' + vat_group);
    if (cmd - reg >= CP_NUM_VAT_REG)
      return {fmt::format("CP_VAT_REG_{} invalid", letter), ""};

    const std::string name = fmt::format("CP_VAT_REG_{} - Format {}", letter, cmd & CP_VAT_MASK);
    switch (vat_group)
    {
    case 0:
      return {name, fmt::to_string(UVAT_group0{.Hex = value})};
    case 1:
      return {name, fmt::to_string(UVAT_group1{.Hex = value})};
    default:
      return {name, fmt::to_string(UVAT_group2{.Hex = value})};
    }
  }

  const auto array = static_cast<CPArray>(cmd & CP_ARRAY_MASK);
  switch (reg)
  {
  case MATINDEX_A:
    return {"MATINDEX_A", fmt::to_string(TMatrixIndexA{.Hex = value})};
  case MATINDEX_B:
    return {"MATINDEX_B", fmt::to_string(TMatrixIndexB{.Hex = value})};
  case VCD_LO:
    return {"VCD_LO", fmt::to_string(TVtxDesc::Low{.Hex = value})};
  case VCD_HI:
    return {"VCD_HI", fmt::to_string(TVtxDesc::High{.Hex = value})};
  case ARRAY_BASE:
    return {fmt::format("ARRAY_BASE Array {}", array), fmt::format("Base address {:08x}", value)};
  case ARRAY_STRIDE:
    return {fmt::format("ARRAY_STRIDE Array {}", array), fmt::format("Stride {:02x}", value & 0xff)};
  default:
    // Includes the unemulated 0x00/0x10/0x20 registers.
    return {fmt::format("Invalid CP register {:02x} = {:08x}", cmd, value), ""};
  }
}

CPState::CPState(const u32* memory) : CPState()
{
  ForEachRegister(*this, [memory](u32 reg, u32& field) { field = memory[reg]; });
}

void CPState::FillCPMemoryArray(u32* memory) const
{
  ForEachRegister(*this, [memory](u32 reg, const u32& field) { memory[reg] = field; });
}

void CPState::LoadCPReg(u8 sub_cmd, u32 value)
{
  const u8 reg = sub_cmd & CP_COMMAND_MASK;

  const int vat_group = VatGroupOf(reg);
  if (vat_group >= 0)
  {
    // Only eight tables exist; 0x78-0x7f etc. alias tables 0-7 but are reported.
    const int table = sub_cmd - reg;
    if (table >= CP_NUM_VAT_REG)
    {
      ReportQuirk(GameQuirk::UsesMaybeInvalidCPCommand);
      WARN_LOG_FMT(VIDEO, "CP_VAT_REG_{}: Invalid VAT {}", static_cast<char>('A' + vat_group),
                   table);
    }
    VatWord(vtx_attr[sub_cmd & CP_VAT_MASK], vat_group) = value;
    return;
  }

  switch (reg)
  {
  case UNKNOWN_00:
  case UNKNOWN_10:
  case UNKNOWN_20:
    // Every SDK and libogc title writes 0x20 = 0 at boot; anything else is worth noting.
    if (sub_cmd != UNKNOWN_20 || value != 0)
    {
      ReportQuirk(GameQuirk::UsesCPPerfCommand);
      DEBUG_LOG_FMT(VIDEO, "Unknown CP command possibly relating to perf queries used: {:02x}",
                    sub_cmd);
    }
    return;

  case MATINDEX_A:
    CheckExactRegister(sub_cmd, MATINDEX_A, "MATINDEX_A");
    matrix_index_a.Hex = value;
    return;

  case MATINDEX_B:
    CheckExactRegister(sub_cmd, MATINDEX_B, "MATINDEX_B");
    matrix_index_b.Hex = value;
    return;

  case VCD_LO:
    CheckExactRegister(sub_cmd, VCD_LO, "VCD_LO");
    vtx_desc.low.Hex = value;
    return;

  case VCD_HI:
    CheckExactRegister(sub_cmd, VCD_HI, "VCD_HI");
    vtx_desc.high.Hex = value;
    return;

  case ARRAY_BASE:
  {
    // Array pointers are physical addresses; the CP ignores the upper bits.
    const u32 mask = CommandProcessor::GetPhysicalAddressMask(Core::System::GetInstance().IsWii());
    array_bases[static_cast<CPArray>(sub_cmd & CP_ARRAY_MASK)] = value & mask;
    return;
  }

  case ARRAY_STRIDE:
    array_strides[static_cast<CPArray>(sub_cmd & CP_ARRAY_MASK)] = value & 0xFF;
    return;

  default:
    ReportQuirk(GameQuirk::UsesUnknownCPCommand);
    WARN_LOG_FMT(VIDEO, "Unknown CP register {:02x} set to {:08x}", sub_cmd, value);
    return;
  }
}
