// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// The DSP instruction set as data: one template per instruction (encoding,
// operand fields, control-flow properties) for the assembler, disassembler,
// interpreter, analyzer and JIT, plus the names of registers and of the
// hardware registers.

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <string_view>

#include "Core/DSP/DSPCommon.h"
#include "Core/DSP/DSPCore.h"

namespace DSP
{
// Operand kinds. For register operands the base register number sits in bits
// 8-13; 0x80 marks the "other accumulator" (_D) forms and 0x10 the multiplier
// forms that may name either half of an AX register.
enum partype_t
{
  P_NONE = 0x0000,
  P_VAL = 0x0001,
  P_IMM = 0x0002,
  P_MEM = 0x0003,
  P_STR = 0x0004,
  P_ADDR_I = 0x0005,
  P_ADDR_D = 0x0006,
  P_REG = 0x8000,
  P_REG04 = P_REG | DSP_REG_IX0 << 8,
  P_REG08 = P_REG | DSP_REG_WR0 << 8,
  P_REG18 = P_REG | DSP_REG_AXL0 << 8,
  P_REGM18 = P_REG | DSP_REG_AXL0 << 8 | 0x10,
  P_REG19 = P_REG | DSP_REG_AXL1 << 8,
  P_REGM19 = P_REG | DSP_REG_AXL1 << 8 | 0x10,
  P_REG1A = P_REG | DSP_REG_AXH0 << 8 | 0x80,
  P_ACCL = P_REG | DSP_REG_ACL0 << 8,
  P_REG1C = P_REG | DSP_REG_ACL0 << 8 | 0x10,
  P_ACCM = P_REG | DSP_REG_ACM0 << 8,
  P_ACCM_D = P_REG | DSP_REG_ACM0 << 8 | 0x80,
  P_ACC = P_REG | DSP_REG_ACC0_FULL << 8,
  P_ACCH = P_REG | DSP_REG_ACH0 << 8,
  P_ACC_D = P_REG | DSP_REG_ACC0_FULL << 8 | 0x80,
  P_AX = P_REG | DSP_REG_AX0_FULL << 8,
  P_REGS_MASK = 0x03f80,
  P_REF = P_REG | 0x4000,
  P_PRG = P_REF | P_REG,
};

// One operand field: its kind, how many words the instruction has when it
// is present (size), which word holds it (loc), and where (mask, lshift).
struct param2_t
{
  partype_t type;
  u8 size;
  u8 loc;
  s8 lshift;
  u16 mask;
};

struct DSPOPCTemplate
{
  const char* name;
  u16 opcode;
  u16 opcode_mask;

  u8 size;
  u8 param_count;
  param2_t params[8];
  bool extended;
  bool branch;
  bool uncond_branch;
  bool reads_pc;
  bool updates_sr;
};

// The template used for words that are no instruction ("CW", a constant word).
extern const DSPOPCTemplate cw;

struct pdlabel_t
{
  u16 addr;
  const char* name;
  const char* description;
};

extern const std::array<pdlabel_t, 36> regnames;
extern const std::array<pdlabel_t, 96> pdlabels;

std::string pdname(u16 val);
std::string pdregname(int val);
std::string pdregnamelong(int val);

// Builds the lookup tables behind GetOpTemplate and GetExtOpTemplate.
void InitInstructionTable();

// Linear searches over the templates, for the assembler and disassembler.
const DSPOPCTemplate* FindOpInfoByOpcode(UDSPInstruction opcode);
const DSPOPCTemplate* FindOpInfoByName(std::string_view name);

const DSPOPCTemplate* FindExtOpInfoByOpcode(UDSPInstruction opcode);
const DSPOPCTemplate* FindExtOpInfoByName(std::string_view name);

// Table lookups for the emulators. Both return nullptr before InitInstructionTable.
const DSPOPCTemplate* GetOpTemplate(UDSPInstruction inst);
const DSPOPCTemplate* GetExtOpTemplate(UDSPInstruction inst);

// First entry of data whose (opcode, opcode_mask) pair matches the instruction.
template <typename T, size_t N>
auto FindByOpcode(UDSPInstruction opcode, const std::array<T, N>& data)
{
  return std::ranges::find_if(data, [opcode](const auto& entry) {
    return (opcode & entry.opcode_mask) == entry.opcode;
  });
}
}  // namespace DSP
