// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// The instruction templates follow the GameCube DSP instruction set as described
// in the public DSP documentation (duddie's GC DSP manual and its successors).
// Entries are matched first to last, so where encodings overlap the earlier
// entry wins; the order below is part of the behaviour.

#include "Core/DSP/DSPTables.h"

#include <array>
#include <cstddef>
#include <initializer_list>
#include <string>
#include <string_view>

#include <fmt/format.h>

#include "Common/CommonTypes.h"

#include "Core/DSP/DSPCore.h"

namespace DSP
{
namespace
{
// ---------------------------------------------------------------------------
// Building blocks for the templates
// ---------------------------------------------------------------------------

enum Trait : u8
{
  NONE = 0,
  EXT = 1 << 0,       // takes an extension opcode in its low bits
  BRANCH = 1 << 1,    // may change the flow of control
  UNCOND = 1 << 2,    // ... always
  READS_PC = 1 << 3,  // looks at pc (immediates, return addresses)
  SETS_SR = 1 << 4,   // writes the arithmetic flags
};

// An operand stored in the instruction word itself.
constexpr param2_t Field(partype_t type, s8 lshift, u16 mask)
{
  return {type, 1, 0, lshift, mask};
}

// An operand that is the whole second word of a two-word instruction.
constexpr param2_t Word2(partype_t type)
{
  return {type, 2, 1, 0, 0xffff};
}

template <size_t N>
class TemplateList
{
public:
  // The instruction is as long as its longest operand needs.
  constexpr void Add(const char* name, u16 opcode, u16 mask, std::initializer_list<param2_t> params,
                     u8 traits)
  {
    DSPOPCTemplate& op = m_ops[m_count++];
    op.name = name;
    op.opcode = opcode;
    op.opcode_mask = mask;
    op.size = 1;
    op.param_count = 0;
    for (const param2_t& param : params)
    {
      op.params[op.param_count++] = param;
      if (param.size > op.size)
        op.size = param.size;
    }
    op.extended = (traits & EXT) != 0;
    op.branch = (traits & BRANCH) != 0;
    op.uncond_branch = (traits & UNCOND) != 0;
    op.reads_pc = (traits & READS_PC) != 0;
    op.updates_sr = (traits & SETS_SR) != 0;
  }

  constexpr size_t Count() const { return m_count; }
  constexpr const std::array<DSPOPCTemplate, N>& Ops() const { return m_ops; }

private:
  std::array<DSPOPCTemplate, N> m_ops{};
  size_t m_count = 0;
};

// Short ASCII strings built at compile time, for the generated mnemonics and labels.
struct ShortText
{
  char text[12];
};

constexpr ShortText Join(std::string_view a, std::string_view b)
{
  ShortText out{};
  size_t n = 0;
  for (const char c : a)
    out.text[n++] = c;
  for (const char c : b)
    out.text[n++] = c;
  out.text[n] = '\0';
  return out;
}

// ---------------------------------------------------------------------------
// Condition codes
// ---------------------------------------------------------------------------

// Mnemonic suffixes for condition codes 0x0-0xe; 0xf is "always".
constexpr std::array<std::string_view, 15> CONDITION_SUFFIXES{
    "GE", "L", "G", "LE", "NZ", "Z", "NC", "C", "X8", "X9", "XA", "XB", "LNZ", "LZ", "O"};

struct ConditionalFamily
{
  std::string_view prefix;    // for the named conditions
  std::string_view x_prefix;  // for the X8-XB conditions
  std::string_view always;    // the unconditional form
  u16 base;
  u16 mask;
  u8 cond_traits;
  u8 always_traits;
};

enum FamilyIndex : size_t
{
  FAMILY_RET,
  FAMILY_RTI,
  FAMILY_CALL,
  FAMILY_IF,
  FAMILY_JMP,
  FAMILY_JMPR,
  FAMILY_CALLR,
  FAMILY_COUNT
};

constexpr std::array<ConditionalFamily, FAMILY_COUNT> FAMILIES{{
    {"RET", "RET", "RET", 0x02d0, 0xffff, BRANCH | READS_PC, BRANCH | UNCOND},
    {"RTI", "RTI", "RTI", 0x02f0, 0xffff, BRANCH | READS_PC, BRANCH | UNCOND},
    {"CALL", "CALL", "CALL", 0x02b0, 0xffff, BRANCH | READS_PC, BRANCH | UNCOND | READS_PC},
    {"IF", "IF", "IF", 0x0270, 0xffff, BRANCH | READS_PC, BRANCH | UNCOND | READS_PC},
    {"J", "JMP", "JMP", 0x0290, 0xffff, BRANCH | READS_PC, BRANCH | UNCOND | READS_PC},
    {"JR", "JMPR", "JMPR", 0x1700, 0xff1f, BRANCH, BRANCH | UNCOND},
    {"CALLR", "CALLR", "CALLR", 0x1710, 0xff1f, BRANCH | READS_PC, BRANCH | UNCOND | READS_PC},
}};

constexpr auto CONDITIONAL_NAMES = [] {
  std::array<std::array<ShortText, 16>, FAMILY_COUNT> names{};
  for (size_t f = 0; f < FAMILY_COUNT; ++f)
  {
    const ConditionalFamily& family = FAMILIES[f];
    for (size_t cond = 0; cond < 15; ++cond)
    {
      const bool x_condition = cond >= 8 && cond <= 11;
      names[f][cond] = Join(x_condition ? family.x_prefix : family.prefix, CONDITION_SUFFIXES[cond]);
    }
    names[f][15] = Join(family.always, "");
  }
  return names;
}();

template <size_t N>
constexpr void AddConditionalFamily(TemplateList<N>& list, size_t family_index,
                                    std::initializer_list<param2_t> params)
{
  const ConditionalFamily& family = FAMILIES[family_index];
  for (u16 cond = 0; cond < 16; ++cond)
  {
    const u8 traits = cond == 15 ? family.always_traits : family.cond_traits;
    list.Add(CONDITIONAL_NAMES[family_index][cond].text, static_cast<u16>(family.base + cond),
             family.mask, params, traits);
  }
}

// ---------------------------------------------------------------------------
// Main instructions
// ---------------------------------------------------------------------------

constexpr size_t MAIN_OP_COUNT = 230;

constexpr TemplateList<MAIN_OP_COUNT> BuildMainOps()
{
  TemplateList<MAIN_OP_COUNT> t;

  const param2_t ar_low2 = Field(P_REG, 0, 0x0003);
  const param2_t any_reg = Field(P_REG, 0, 0x001f);
  const param2_t jump_reg = Field(P_REG, 5, 0x00e0);
  const param2_t acc_bit8 = Field(P_ACC, 8, 0x0100);
  const param2_t accm_bit8 = Field(P_ACCM, 8, 0x0100);
  const param2_t other_accm_bit8 = Field(P_ACCM_D, 8, 0x0100);
  const param2_t acc_bit11 = Field(P_ACC, 11, 0x0800);
  const param2_t axh_bit9 = Field(P_REG1A, 9, 0x0200);
  const param2_t imm8 = Field(P_IMM, 0, 0x00ff);
  const param2_t shift6 = Field(P_IMM, 0, 0x003f);
  const param2_t short_mem = Field(P_MEM, 0, 0x00ff);
  const param2_t ar_bits5 = Field(P_PRG, 5, 0x0060);
  const param2_t ar_bits0 = Field(P_PRG, 0, 0x0003);

  // Address register arithmetic.
  t.Add("NOP", 0x0000, 0xfffc, {}, NONE);
  t.Add("DAR", 0x0004, 0xfffc, {ar_low2}, NONE);
  t.Add("IAR", 0x0008, 0xfffc, {ar_low2}, NONE);
  t.Add("SUBARN", 0x000c, 0xfffc, {ar_low2}, NONE);
  t.Add("ADDARN", 0x0010, 0xfff0, {ar_low2, Field(P_REG04, 2, 0x000c)}, NONE);

  t.Add("HALT", 0x0021, 0xffff, {}, BRANCH | UNCOND);

  // Conditional control flow, sixteen condition codes each.
  AddConditionalFamily(t, FAMILY_RET, {});
  AddConditionalFamily(t, FAMILY_RTI, {});
  AddConditionalFamily(t, FAMILY_CALL, {Word2(P_ADDR_I)});
  AddConditionalFamily(t, FAMILY_IF, {});
  AddConditionalFamily(t, FAMILY_JMP, {Word2(P_ADDR_I)});
  AddConditionalFamily(t, FAMILY_JMPR, {jump_reg});
  AddConditionalFamily(t, FAMILY_CALLR, {jump_reg});

  // Status register bits 6-13.
  t.Add("SBCLR", 0x1200, 0xff00, {Field(P_IMM, 0, 0x0007)}, NONE);
  t.Add("SBSET", 0x1300, 0xff00, {Field(P_IMM, 0, 0x0007)}, NONE);

  // Shifts by an immediate.
  t.Add("LSL", 0x1400, 0xfec0, {acc_bit8, shift6}, SETS_SR);
  t.Add("LSR", 0x1440, 0xfec0, {acc_bit8, shift6}, SETS_SR);
  t.Add("ASL", 0x1480, 0xfec0, {acc_bit8, shift6}, SETS_SR);
  t.Add("ASR", 0x14c0, 0xfec0, {acc_bit8, shift6}, SETS_SR);
  // Shifts of $ac0 by $ac1.m.
  t.Add("LSRN", 0x02ca, 0xffff, {}, SETS_SR);
  t.Add("ASRN", 0x02cb, 0xffff, {}, SETS_SR);

  // Moves with a long immediate or address.
  t.Add("LRI", 0x0080, 0xffe0, {any_reg, Word2(P_IMM)}, READS_PC);
  t.Add("LR", 0x00c0, 0xffe0, {any_reg, Word2(P_MEM)}, READS_PC);
  t.Add("SR", 0x00e0, 0xffe0, {Word2(P_MEM), any_reg}, READS_PC);

  t.Add("MRR", 0x1c00, 0xfc00, {Field(P_REG, 5, 0x03e0), any_reg}, NONE);

  t.Add("SI", 0x1600, 0xff00, {short_mem, Word2(P_IMM)}, READS_PC);

  // Short immediates.
  t.Add("ADDIS", 0x0400, 0xfe00, {accm_bit8, imm8}, SETS_SR);
  t.Add("CMPIS", 0x0600, 0xfe00, {accm_bit8, imm8}, SETS_SR);
  t.Add("LRIS", 0x0800, 0xf800, {Field(P_REG18, 8, 0x0700), imm8}, SETS_SR);

  // Long immediate arithmetic and logic on $acD.m.
  for (const auto& [name, opcode] : {std::pair<const char*, u16>{"ADDI", 0x0200},
                                     {"XORI", 0x0220},
                                     {"ANDI", 0x0240},
                                     {"ORI", 0x0260},
                                     {"CMPI", 0x0280},
                                     {"ANDF", 0x02a0},
                                     {"ANDCF", 0x02c0}})
  {
    t.Add(name, opcode, 0xfeff, {accm_bit8, Word2(P_IMM)}, READS_PC | SETS_SR);
  }

  // Loads from instruction memory.
  t.Add("ILRR", 0x0210, 0xfefc, {accm_bit8, ar_bits0}, NONE);
  t.Add("ILRRD", 0x0214, 0xfefc, {accm_bit8, ar_bits0}, NONE);
  t.Add("ILRRI", 0x0218, 0xfefc, {accm_bit8, ar_bits0}, NONE);
  t.Add("ILRRN", 0x021c, 0xfefc, {accm_bit8, ar_bits0}, NONE);

  // Hardware loops.
  t.Add("LOOP", 0x0040, 0xffe0, {any_reg}, BRANCH | UNCOND | READS_PC);
  t.Add("BLOOP", 0x0060, 0xffe0, {any_reg, Word2(P_ADDR_I)}, BRANCH | UNCOND | READS_PC);
  t.Add("LOOPI", 0x1000, 0xff00, {imm8}, BRANCH | UNCOND | READS_PC);
  t.Add("BLOOPI", 0x1100, 0xff00, {imm8, Word2(P_ADDR_I)}, BRANCH | UNCOND | READS_PC);

  // Indirect loads and stores through $arS, with the four post-modify modes.
  t.Add("LRR", 0x1800, 0xff80, {any_reg, ar_bits5}, NONE);
  t.Add("LRRD", 0x1880, 0xff80, {any_reg, ar_bits5}, NONE);
  t.Add("LRRI", 0x1900, 0xff80, {any_reg, ar_bits5}, NONE);
  t.Add("LRRN", 0x1980, 0xff80, {any_reg, ar_bits5}, NONE);
  t.Add("SRR", 0x1a00, 0xff80, {ar_bits5, any_reg}, NONE);
  t.Add("SRRD", 0x1a80, 0xff80, {ar_bits5, any_reg}, NONE);
  t.Add("SRRI", 0x1b00, 0xff80, {ar_bits5, any_reg}, NONE);
  t.Add("SRRN", 0x1b80, 0xff80, {ar_bits5, any_reg}, NONE);

  // Loads and stores within the page selected by $cr.
  t.Add("LRS", 0x2000, 0xf800, {Field(P_REG18, 8, 0x0700), short_mem}, NONE);
  t.Add("SRSH", 0x2800, 0xfe00, {short_mem, Field(P_ACCH, 8, 0x0100)}, NONE);
  t.Add("SRS", 0x2c00, 0xfc00, {short_mem, Field(P_REG1C, 8, 0x0300)}, NONE);

  // From here on every instruction can carry an extension opcode: seven bits of
  // it in the 0x3xxx group, eight bits above that.
  constexpr u8 ALU = EXT | SETS_SR;

  t.Add("XORR", 0x3000, 0xfc80, {accm_bit8, axh_bit9}, ALU);
  t.Add("ANDR", 0x3400, 0xfc80, {accm_bit8, axh_bit9}, ALU);
  t.Add("ORR", 0x3800, 0xfc80, {accm_bit8, axh_bit9}, ALU);
  t.Add("ANDC", 0x3c00, 0xfe80, {accm_bit8, other_accm_bit8}, ALU);
  t.Add("ORC", 0x3e00, 0xfe80, {accm_bit8, other_accm_bit8}, ALU);
  t.Add("XORC", 0x3080, 0xfe80, {accm_bit8, other_accm_bit8}, ALU);
  t.Add("NOT", 0x3280, 0xfe80, {accm_bit8}, ALU);
  t.Add("LSRNRX", 0x3480, 0xfc80, {acc_bit8, axh_bit9}, ALU);
  t.Add("ASRNRX", 0x3880, 0xfc80, {acc_bit8, axh_bit9}, ALU);
  t.Add("LSRNR", 0x3c80, 0xfe80, {acc_bit8, other_accm_bit8}, ALU);
  t.Add("ASRNR", 0x3e80, 0xfe80, {acc_bit8, other_accm_bit8}, ALU);

  // 0x4xxx-0x6xxx: add, subtract and move share one operand layout.
  struct AluGroup
  {
    const char* with_reg;
    const char* with_ax;
    const char* with_acc;
    const char* with_prod;
    u16 base;
  };
  for (const AluGroup& g : {AluGroup{"ADDR", "ADDAX", "ADD", "ADDP", 0x4000},
                            AluGroup{"SUBR", "SUBAX", "SUB", "SUBP", 0x5000},
                            AluGroup{"MOVR", "MOVAX", "MOV", "MOVP", 0x6000}})
  {
    t.Add(g.with_reg, g.base, 0xf800, {acc_bit8, Field(P_REG18, 9, 0x0600)}, ALU);
    t.Add(g.with_ax, static_cast<u16>(g.base | 0x0800), 0xfc00,
          {acc_bit8, Field(P_AX, 9, 0x0200)}, ALU);
    t.Add(g.with_acc, static_cast<u16>(g.base | 0x0c00), 0xfe00,
          {acc_bit8, Field(P_ACC_D, 8, 0x0100)}, ALU);
    t.Add(g.with_prod, static_cast<u16>(g.base | 0x0e00), 0xfe00, {acc_bit8}, ALU);
  }

  t.Add("ADDAXL", 0x7000, 0xfc00, {acc_bit8, Field(P_REG18, 9, 0x0200)}, ALU);
  t.Add("INCM", 0x7400, 0xfe00, {accm_bit8}, ALU);
  t.Add("INC", 0x7600, 0xfe00, {acc_bit8}, ALU);
  t.Add("DECM", 0x7800, 0xfe00, {accm_bit8}, ALU);
  t.Add("DEC", 0x7a00, 0xfe00, {acc_bit8}, ALU);
  t.Add("NEG", 0x7c00, 0xfe00, {acc_bit8}, ALU);
  t.Add("MOVNP", 0x7e00, 0xfe00, {acc_bit8}, ALU);

  t.Add("NX", 0x8000, 0xf700, {}, EXT);
  t.Add("CLR", 0x8100, 0xf700, {acc_bit11}, ALU);
  t.Add("CMP", 0x8200, 0xff00, {}, ALU);
  t.Add("MULAXH", 0x8300, 0xff00, {}, ALU);
  t.Add("CLRP", 0x8400, 0xff00, {}, ALU);
  t.Add("TSTPROD", 0x8500, 0xff00, {}, ALU);
  t.Add("TSTAXH", 0x8600, 0xfe00, {Field(P_REG1A, 8, 0x0100)}, ALU);

  // Mode switches.
  t.Add("M2", 0x8a00, 0xff00, {}, EXT);
  t.Add("M0", 0x8b00, 0xff00, {}, EXT);
  t.Add("CLR15", 0x8c00, 0xff00, {}, EXT);
  t.Add("SET15", 0x8d00, 0xff00, {}, EXT);
  t.Add("SET16", 0x8e00, 0xff00, {}, EXT);
  t.Add("SET40", 0x8f00, 0xff00, {}, EXT);

  // Multiplies of $axS.l by $axS.h.
  const param2_t axl_bit11 = Field(P_REG18, 11, 0x0800);
  const param2_t axh_bit11 = Field(P_REG1A, 11, 0x0800);
  t.Add("MUL", 0x9000, 0xf700, {axl_bit11, axh_bit11}, ALU);
  t.Add("ASR16", 0x9100, 0xf700, {acc_bit11}, ALU);
  t.Add("MULMVZ", 0x9200, 0xf600, {axl_bit11, axh_bit11, acc_bit8}, ALU);
  t.Add("MULAC", 0x9400, 0xf600, {axl_bit11, axh_bit11, acc_bit8}, ALU);
  t.Add("MULMV", 0x9600, 0xf600, {axl_bit11, axh_bit11, acc_bit8}, ALU);

  // Multiplies of a half of $ax0 by a half of $ax1.
  const param2_t ax0_half = Field(P_REGM18, 11, 0x1000);
  const param2_t ax1_half = Field(P_REGM19, 10, 0x0800);
  t.Add("MULX", 0xa000, 0xe700, {ax0_half, ax1_half}, ALU);
  t.Add("ABS", 0xa100, 0xf700, {acc_bit11}, ALU);
  t.Add("MULXMVZ", 0xa200, 0xe600, {ax0_half, ax1_half, acc_bit8}, ALU);
  t.Add("MULXAC", 0xa400, 0xe600, {ax0_half, ax1_half, acc_bit8}, ALU);
  t.Add("MULXMV", 0xa600, 0xe600, {ax0_half, ax1_half, acc_bit8}, ALU);
  t.Add("TST", 0xb100, 0xf700, {acc_bit11}, ALU);

  // Multiplies of $acS.m by $axT.h.
  const param2_t accm_bit12 = Field(P_ACCM, 12, 0x1000);
  t.Add("MULC", 0xc000, 0xe700, {accm_bit12, axh_bit11}, ALU);
  t.Add("CMPAXH", 0xc100, 0xe700, {acc_bit11, Field(P_REG1A, 12, 0x1000)}, ALU);
  t.Add("MULCMVZ", 0xc200, 0xe600, {accm_bit12, axh_bit11, acc_bit8}, ALU);
  t.Add("MULCAC", 0xc400, 0xe600, {accm_bit12, axh_bit11, acc_bit8}, ALU);
  t.Add("MULCMV", 0xc600, 0xe600, {accm_bit12, axh_bit11, acc_bit8}, ALU);

  // Multiply-accumulate into $prod.
  t.Add("MADDX", 0xe000, 0xfc00, {Field(P_REGM18, 8, 0x0200), Field(P_REGM19, 7, 0x0100)}, ALU);
  t.Add("MSUBX", 0xe400, 0xfc00, {Field(P_REGM18, 8, 0x0200), Field(P_REGM19, 7, 0x0100)}, ALU);
  t.Add("MADDC", 0xe800, 0xfc00, {Field(P_ACCM, 9, 0x0200), Field(P_REG19, 7, 0x0100)}, ALU);
  t.Add("MSUBC", 0xec00, 0xfc00, {Field(P_ACCM, 9, 0x0200), Field(P_REG19, 7, 0x0100)}, ALU);

  const param2_t axl_bit8 = Field(P_REG18, 8, 0x0100);
  const param2_t axh_bit8 = Field(P_REG1A, 8, 0x0100);
  t.Add("LSL16", 0xf000, 0xfe00, {acc_bit8}, ALU);
  t.Add("MADD", 0xf200, 0xfe00, {axl_bit8, axh_bit8}, ALU);
  t.Add("LSR16", 0xf400, 0xfe00, {acc_bit8}, ALU);
  t.Add("MSUB", 0xf600, 0xfe00, {axl_bit8, axh_bit8}, ALU);
  t.Add("ADDPAXZ", 0xf800, 0xfc00, {Field(P_ACC, 9, 0x0200), Field(P_AX, 8, 0x0100)}, ALU);
  t.Add("CLRL", 0xfc00, 0xfe00, {Field(P_ACCL, 11, 0x0800)}, ALU);
  t.Add("MOVPZ", 0xfe00, 0xfe00, {acc_bit8}, ALU);

  return t;
}

constexpr TemplateList<MAIN_OP_COUNT> MAIN_OP_LIST = BuildMainOps();
static_assert(MAIN_OP_LIST.Count() == MAIN_OP_COUNT);
constexpr const std::array<DSPOPCTemplate, MAIN_OP_COUNT>& s_main_ops = MAIN_OP_LIST.Ops();

// ---------------------------------------------------------------------------
// Extension instructions (the low 7 or 8 bits of an extendable instruction)
// ---------------------------------------------------------------------------

constexpr size_t EXT_OP_COUNT = 25;

constexpr TemplateList<EXT_OP_COUNT> BuildExtOps()
{
  TemplateList<EXT_OP_COUNT> t;

  const param2_t ar_low2 = Field(P_PRG, 0, 0x0003);
  const param2_t acc_part = Field(P_REG1C, 3, 0x0018);
  const param2_t load_target = Field(P_REG18, 3, 0x0038);
  const param2_t ls_target = Field(P_REG18, 4, 0x0030);
  const param2_t ls_source = Field(P_ACCM, 0, 0x0001);

  t.Add("XXX", 0x0000, 0x00fc, {Field(P_VAL, 0, 0x00ff)}, NONE);

  t.Add("DR", 0x0004, 0x00fc, {Field(P_REG, 0, 0x0003)}, NONE);
  t.Add("IR", 0x0008, 0x00fc, {Field(P_REG, 0, 0x0003)}, NONE);
  t.Add("NR", 0x000c, 0x00fc, {Field(P_REG, 0, 0x0003)}, NONE);
  t.Add("MV", 0x0010, 0x00f0, {Field(P_REG18, 2, 0x000c), Field(P_REG1C, 0, 0x0003)}, NONE);

  t.Add("S", 0x0020, 0x00e4, {ar_low2, acc_part}, NONE);
  t.Add("SN", 0x0024, 0x00e4, {ar_low2, acc_part}, NONE);

  t.Add("L", 0x0040, 0x00c4, {load_target, ar_low2}, NONE);
  t.Add("LN", 0x0044, 0x00c4, {load_target, ar_low2}, NONE);

  // Load through $ar0 and store through $ar3 in one go; N and M select the
  // index register update of $ar0 and $ar3.
  constexpr std::array<const char*, 4> modes{"", "N", "M", "NM"};
  constexpr std::array<std::array<const char*, 4>, 2> ls_names{{
      {"LS", "LSN", "LSM", "LSNM"},
      {"SL", "SLN", "SLM", "SLNM"},
  }};
  for (u16 mode = 0; mode < modes.size(); ++mode)
  {
    const u16 opcode = static_cast<u16>(0x0080 | (mode << 2));
    t.Add(ls_names[0][mode], opcode, 0x00ce, {ls_target, ls_source}, NONE);
    t.Add(ls_names[1][mode], static_cast<u16>(opcode | 2), 0x00ce, {ls_source, ls_target}, NONE);
  }

  // Dual loads into $ax0/$ax1 halves, or both halves of one $axR.
  constexpr std::array<const char*, 4> ldax_names{"LDAX", "LDAXN", "LDAXM", "LDAXNM"};
  for (u16 mode = 0; mode < 4; ++mode)
  {
    t.Add(ldax_names[mode], static_cast<u16>(0x00c3 | (mode << 2)), 0x00cf,
          {Field(P_AX, 4, 0x0010), Field(P_PRG, 5, 0x0020)}, NONE);
  }
  constexpr std::array<const char*, 4> ld_names{"LD", "LDN", "LDM", "LDNM"};
  for (u16 mode = 0; mode < 4; ++mode)
  {
    t.Add(ld_names[mode], static_cast<u16>(0x00c0 | (mode << 2)), 0x00cc,
          {Field(P_REGM18, 4, 0x0020), Field(P_REGM19, 3, 0x0010), ar_low2}, NONE);
  }

  return t;
}

constexpr TemplateList<EXT_OP_COUNT> EXT_OP_LIST = BuildExtOps();
static_assert(EXT_OP_LIST.Count() == EXT_OP_COUNT);
constexpr const std::array<DSPOPCTemplate, EXT_OP_COUNT>& s_ext_ops = EXT_OP_LIST.Ops();

// ---------------------------------------------------------------------------
// Hardware register labels
// ---------------------------------------------------------------------------

constexpr u16 HW_LABEL_BASE = 0xffa0;

constexpr char HexDigit(unsigned value)
{
  return "0123456789abcdef"[value & 0xf];
}

// "0xffNN" for every register in the labelled range.
constexpr auto HEX_LABEL_TEXT = [] {
  std::array<ShortText, 96> out{};
  for (unsigned i = 0; i < out.size(); ++i)
  {
    const unsigned addr = HW_LABEL_BASE + i;
    const char digits[] = {'0', 'x', 'f', 'f', HexDigit(addr >> 4), HexDigit(addr), '\0'};
    for (size_t c = 0; c < sizeof(digits); ++c)
      out[i].text[c] = digits[c];
  }
  return out;
}();

// "COEF_A1_n" / "COEF_A2_n" for the ADPCM coefficient pairs at 0xffa0-0xffaf.
constexpr auto COEF_LABEL_TEXT = [] {
  std::array<ShortText, 16> out{};
  for (unsigned i = 0; i < out.size(); ++i)
  {
    const char digits[] = {'C', 'O', 'E', 'F', '_', 'A', static_cast<char>('1' + (i & 1)), '_',
                           static_cast<char>('0' + i / 2), '\0'};
    for (size_t c = 0; c < sizeof(digits); ++c)
      out[i].text[c] = digits[c];
  }
  return out;
}();

struct NamedHwRegister
{
  u16 addr;
  const char* name;
  const char* description;
};

constexpr std::array<NamedHwRegister, 25> NAMED_HW_REGISTERS{{
    {0xffc9, "DSCR", "DSP DMA Control Reg"},
    {0xffcb, "DSBL", "DSP DMA Block Length"},
    {0xffcd, "DSPA", "DSP DMA DMEM Address"},
    {0xffce, "DSMAH", "DSP DMA Mem Address H"},
    {0xffcf, "DSMAL", "DSP DMA Mem Address L"},
    {0xffd1, "FORMAT", "Accelerator sample format"},
    {0xffd3, "ACDRAW", "Accelerator raw read/write from ARAM"},
    {0xffd4, "ACSAH", "Accelerator start address H"},
    {0xffd5, "ACSAL", "Accelerator start address L"},
    {0xffd6, "ACEAH", "Accelerator end address H"},
    {0xffd7, "ACEAL", "Accelerator end address L"},
    {0xffd8, "ACCAH", "Accelerator current address H"},
    {0xffd9, "ACCAL", "Accelerator current address L"},
    {0xffda, "PRED_SCALE", "ADPCM predictor and scale"},
    {0xffdb, "YN1", "ADPCM output history Y[N - 1]"},
    {0xffdc, "YN2", "ADPCM output history Y[N - 2]"},
    {0xffdd, "ACDSAMP", "Accelerator processed sample read from ARAM or ACIN"},
    {0xffde, "GAIN", "Gain"},
    {0xffdf, "ACIN", "Accelerator MMIO PCM input value"},
    {0xffef, "AMDM", "ARAM DMA Request Mask"},
    {0xfffb, "DIRQ", "DSP IRQ Request"},
    {0xfffc, "DMBH", "DSP Mailbox H"},
    {0xfffd, "DMBL", "DSP Mailbox L"},
    {0xfffe, "CMBH", "CPU Mailbox H"},
    {0xffff, "CMBL", "CPU Mailbox L"},
}};

constexpr std::array<pdlabel_t, 96> BuildHwLabels()
{
  std::array<pdlabel_t, 96> labels{};
  for (size_t i = 0; i < labels.size(); ++i)
  {
    const u16 addr = static_cast<u16>(HW_LABEL_BASE + i);
    if (i < COEF_LABEL_TEXT.size())
      labels[i] = {addr, COEF_LABEL_TEXT[i].text, COEF_LABEL_TEXT[i].text};
    else
      labels[i] = {addr, HEX_LABEL_TEXT[i].text, nullptr};
  }
  for (const NamedHwRegister& reg : NAMED_HW_REGISTERS)
    labels[reg.addr - HW_LABEL_BASE] = {reg.addr, reg.name, reg.description};
  return labels;
}

// ---------------------------------------------------------------------------
// Lookup tables
// ---------------------------------------------------------------------------

std::array<const DSPOPCTemplate*, 0x10000> s_main_lookup{};
std::array<const DSPOPCTemplate*, 0x100> s_ext_lookup{};

template <size_t N, size_t M>
void FillLookup(std::array<const DSPOPCTemplate*, M>& lookup,
                const std::array<DSPOPCTemplate, N>& ops)
{
  lookup.fill(&cw);

  // Go backwards so that, where encodings overlap, the earliest entry ends up in
  // the table. Visit every encoding of each template by counting through the
  // bits its mask leaves free.
  constexpr u32 all_bits = static_cast<u32>(M - 1);
  for (size_t i = N; i-- > 0;)
  {
    const DSPOPCTemplate& op = ops[i];
    const u32 fixed = op.opcode_mask & all_bits;
    if ((op.opcode & ~fixed & all_bits) != 0)
      continue;  // can never match

    const u32 free_bits = ~fixed & all_bits;
    u32 variant = 0;
    do
    {
      lookup[op.opcode | variant] = &op;
      variant = (variant - free_bits) & free_bits;
    } while (variant != 0);
  }
}

template <size_t N>
const DSPOPCTemplate* FirstByOpcode(UDSPInstruction opcode, const std::array<DSPOPCTemplate, N>& ops)
{
  const auto iter = FindByOpcode(opcode, ops);
  return iter == ops.end() ? nullptr : &*iter;
}

template <size_t N>
const DSPOPCTemplate* FirstByName(std::string_view name, const std::array<DSPOPCTemplate, N>& ops)
{
  for (const DSPOPCTemplate& op : ops)
  {
    if (name == op.name)
      return &op;
  }
  return nullptr;
}
}  // namespace

const DSPOPCTemplate cw = {"CW", 0x0000, 0x0000, 1, 1, {{P_VAL, 2, 0, 0, 0xffff}},
                           false, false, false, false, false};

const std::array<pdlabel_t, 96> pdlabels = BuildHwLabels();

const std::array<pdlabel_t, 36> regnames{{
    {DSP_REG_AR0, "AR0", "Addr Reg 00"},
    {DSP_REG_AR1, "AR1", "Addr Reg 01"},
    {DSP_REG_AR2, "AR2", "Addr Reg 02"},
    {DSP_REG_AR3, "AR3", "Addr Reg 03"},
    {DSP_REG_IX0, "IX0", "Index Reg 0"},
    {DSP_REG_IX1, "IX1", "Index Reg 1"},
    {DSP_REG_IX2, "IX2", "Index Reg 2"},
    {DSP_REG_IX3, "IX3", "Index Reg 3"},
    {DSP_REG_WR0, "WR0", "Wrapping Register 0"},
    {DSP_REG_WR1, "WR1", "Wrapping Register 1"},
    {DSP_REG_WR2, "WR2", "Wrapping Register 2"},
    {DSP_REG_WR3, "WR3", "Wrapping Register 3"},
    {DSP_REG_ST0, "ST0", "Call stack"},
    {DSP_REG_ST1, "ST1", "Data stack"},
    {DSP_REG_ST2, "ST2", "Loop addr stack"},
    {DSP_REG_ST3, "ST3", "Loop counter stack"},
    {DSP_REG_ACH0, "AC0.H", "Accu High 0"},
    {DSP_REG_ACH1, "AC1.H", "Accu High 1"},
    {DSP_REG_CR, "CR", "Config Register"},
    {DSP_REG_SR, "SR", "Special Register"},
    {DSP_REG_PRODL, "PROD.L", "Prod L"},
    {DSP_REG_PRODM, "PROD.M1", "Prod M1"},
    {DSP_REG_PRODH, "PROD.H", "Prod H"},
    {DSP_REG_PRODM2, "PROD.M2", "Prod M2"},
    {DSP_REG_AXL0, "AX0.L", "Extra Accu L 0"},
    {DSP_REG_AXL1, "AX1.L", "Extra Accu L 1"},
    {DSP_REG_AXH0, "AX0.H", "Extra Accu H 0"},
    {DSP_REG_AXH1, "AX1.H", "Extra Accu H 1"},
    {DSP_REG_ACL0, "AC0.L", "Accu Low 0"},
    {DSP_REG_ACL1, "AC1.L", "Accu Low 1"},
    {DSP_REG_ACM0, "AC0.M", "Accu Mid 0"},
    {DSP_REG_ACM1, "AC1.M", "Accu Mid 1"},
    {DSP_REG_ACC0_FULL, "ACC0", "Accu Full 0"},
    {DSP_REG_ACC1_FULL, "ACC1", "Accu Full 1"},
    {DSP_REG_AX0_FULL, "AX0", "Extra Accu 0"},
    {DSP_REG_AX1_FULL, "AX1", "Extra Accu 1"},
}};

std::string pdname(u16 val)
{
  if (val >= HW_LABEL_BASE)
    return pdlabels[val - HW_LABEL_BASE].name;
  return fmt::format("0x{:04x}", val);
}

std::string pdregname(int val)
{
  return regnames[static_cast<size_t>(val)].name;
}

std::string pdregnamelong(int val)
{
  return regnames[static_cast<size_t>(val)].description;
}

const DSPOPCTemplate* FindOpInfoByOpcode(UDSPInstruction opcode)
{
  return FirstByOpcode(opcode, s_main_ops);
}

const DSPOPCTemplate* FindOpInfoByName(std::string_view name)
{
  return FirstByName(name, s_main_ops);
}

const DSPOPCTemplate* FindExtOpInfoByOpcode(UDSPInstruction opcode)
{
  return FirstByOpcode(opcode, s_ext_ops);
}

const DSPOPCTemplate* FindExtOpInfoByName(std::string_view name)
{
  return FirstByName(name, s_ext_ops);
}

const DSPOPCTemplate* GetOpTemplate(UDSPInstruction inst)
{
  return s_main_lookup[inst];
}

const DSPOPCTemplate* GetExtOpTemplate(UDSPInstruction inst)
{
  // In the 0x3xxx group only the low seven bits are the extension.
  const u16 ext_bits = (inst >> 12) == 0x3 ? (inst & 0x7f) : (inst & 0xff);
  return s_ext_lookup[ext_bits];
}

void InitInstructionTable()
{
  FillLookup(s_ext_lookup, s_ext_ops);
  FillLookup(s_main_lookup, s_main_ops);
}
}  // namespace DSP
