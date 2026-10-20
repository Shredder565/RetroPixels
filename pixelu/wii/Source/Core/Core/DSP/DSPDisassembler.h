// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// Turns DSP machine code back into assembler text (the syntax DSPAssembler reads).

#pragma once

#include <span>
#include <string>

#include "Common/CommonTypes.h"

namespace DSP
{
struct DSPOPCTemplate;

// Output options; the assembler reads the same struct for its own options.
struct AssemblerSettings
{
  bool print_tabs = false;        // tab after the mnemonic instead of padding it
  bool show_hex = false;          // prefix each line with the instruction words
  bool show_pc = false;           // prefix each line with its address
  bool force = false;             // assembler: keep going after errors
  bool decode_names = true;       // print hardware register names for addresses
  bool decode_registers = true;   // print register names rather than numbers
  char ext_separator = '\'';      // between the main and the extension mnemonic
  bool lower_case_ops = true;

  u16 pc = 0;
};

class DSPDisassembler
{
public:
  explicit DSPDisassembler(const AssemblerSettings& settings);

  // One line per instruction, each ending in a newline. Stops (returning false)
  // at the first word it cannot disassemble.
  bool Disassemble(std::span<const u16> code, std::string& text);

  // Appends the instruction at *pc and advances *pc past it. Only the low 15 bits
  // of *pc index the buffer, so 0x0000 and 0x8000 both name its first word.
  bool DisassembleOpcode(std::span<const u16> code, u16* pc, std::string& dest);
  bool DisassembleOpcode(const u16* binbuf, size_t binbuf_size, u16* pc, std::string& dest);

private:
  std::string FormatOperands(const DSPOPCTemplate& op, u16 word1, u16 word2) const;

  const AssemblerSettings settings_;
};
}  // namespace DSP
