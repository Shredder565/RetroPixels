// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/DSP/DSPCodeUtil.h"

#include <algorithm>
#include <span>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/MsgHandler.h"
#include "Common/Swap.h"

#include "Core/DSP/DSPAssembler.h"
#include "Core/DSP/DSPDisassembler.h"
#include "Core/DSP/DSPTables.h"

namespace DSP
{
namespace
{
std::string DisassembleAt(DSPDisassembler& disassembler, std::span<const u16> code, size_t index)
{
  std::string line;
  u16 pc = static_cast<u16>(index);
  disassembler.DisassembleOpcode(code, &pc, line);
  return line;
}
}  // namespace

bool Assemble(const std::string& text, std::vector<u16>& code, bool force)
{
  AssemblerSettings settings;
  settings.force = force;
  DSPAssembler assembler(settings);
  return assembler.Assemble(text, code);
}

bool Disassemble(std::span<const u16> code, bool line_numbers, std::string& text)
{
  if (code.empty())
    return false;

  // With the words shown the listing cannot be assembled again; it is for reading.
  AssemblerSettings settings;
  settings.show_hex = true;
  settings.show_pc = line_numbers;
  settings.ext_separator = '\'';
  settings.decode_names = true;
  settings.decode_registers = true;

  DSPDisassembler disassembler(settings);
  return disassembler.Disassemble(code, text);
}

// Used by tools and tests that have no log output, hence fmt::print.
bool Compare(std::span<const u16> code1, std::span<const u16> code2)
{
  if (code1.size() != code2.size())
    fmt::print("Size difference! 1={} 2={}\n", code1.size(), code2.size());

  DSPDisassembler disassembler{AssemblerSettings{}};
  const u16 common = static_cast<u16>(std::min(code1.size(), code2.size()));
  u32 equal_words = 0;

  for (u16 i = 0; i < common; ++i)
  {
    if (code1[i] == code2[i])
    {
      ++equal_words;
      continue;
    }

    fmt::print("!! {:04x} : {:04x} vs {:04x} - {}  vs  {}\n", i, code1[i], code2[i],
               DisassembleAt(disassembler, code1, i), DisassembleAt(disassembler, code2, i));

    // Words are compared one at a time, so a differing immediate shows up on its
    // own. When the word before is an identical two-word instruction, show the pair.
    if (i == 0 || code1[i - 1] != code2[i - 1])
      continue;
    const DSPOPCTemplate* const previous = FindOpInfoByOpcode(code1[i - 1]);
    if (previous == nullptr || previous->size != 2)
      continue;
    const int start = i - 1;
    fmt::print("   (or {:04x} : {:04x} {:04x} vs {:04x} {:04x} - {}  vs  {})\n", start,
               code1[i - 1], code1[i], code2[i - 1], code2[i],
               DisassembleAt(disassembler, code1, i - 1u), DisassembleAt(disassembler, code2, i - 1u));
  }

  if (code1.size() != code2.size())
  {
    fmt::print("Extra code words:\n");
    const std::span<const u16> longer = code1.size() > code2.size() ? code1 : code2;
    for (size_t i = common; i < longer.size(); ++i)
    {
      fmt::print("!! {:04x} : {:04x} - {}\n", static_cast<u16>(i), longer[i],
                 DisassembleAt(disassembler, longer, i));
    }
  }

  fmt::print("Equal instruction words: {} / {}\n", equal_words, common);
  return code1.size() == code2.size() && code1.size() == equal_words;
}

std::string CodeToBinaryStringBE(std::span<const u16> code)
{
  std::string bytes;
  bytes.reserve(code.size() * 2);
  for (const u16 word : code)
  {
    bytes.push_back(static_cast<char>(word >> 8));
    bytes.push_back(static_cast<char>(word & 0xff));
  }
  return bytes;
}

std::vector<u16> BinaryStringBEToCode(const std::string& str)
{
  // A trailing odd byte is dropped.
  std::vector<u16> code;
  code.reserve(str.size() / 2);
  for (size_t i = 0; i + 1 < str.size(); i += 2)
  {
    const auto high = static_cast<u8>(str[i]);
    const auto low = static_cast<u8>(str[i + 1]);
    code.push_back(static_cast<u16>((high << 8) | low));
  }
  return code;
}

std::optional<std::vector<u16>> LoadBinary(const std::string& filename)
{
  std::string bytes;
  if (!File::ReadFileToString(filename, bytes))
    return std::nullopt;
  return BinaryStringBEToCode(bytes);
}

bool SaveBinary(std::span<const u16> code, const std::string& filename)
{
  return File::WriteStringToFile(filename, CodeToBinaryStringBE(code));
}

bool DumpDSPCode(const u8* code_be, size_t size_in_bytes, u32 crc)
{
  const std::string base_path =
      File::GetUserPath(D_DUMPDSP_IDX) + fmt::format("DSP_UC_{:08X}", crc);
  const std::string binary_path = base_path + ".bin";

  if (!File::IOFile(binary_path, "wb").WriteBytes(code_be, size_in_bytes))
  {
    PanicAlertFmt("Can't dump UCode to file '{}'!!", binary_path);
    return false;
  }

  // The listing is made from host-order words.
  std::vector<u16> code(size_in_bytes / 2);
  for (size_t i = 0; i < code.size(); ++i)
    code[i] = Common::swap16(code_be + i * 2);

  std::string listing;
  if (!Disassemble(code, true, listing))
    return false;
  return File::WriteStringToFile(base_path + ".txt", listing);
}
}  // namespace DSP
