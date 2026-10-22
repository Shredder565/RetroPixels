// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/PowerPC/PPCSymbolDB.h"

#include <cstdio>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/Debugger/DebugInterface.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PPCAnalyst.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/PowerPC/SignatureDB/SignatureDB.h"
#include "Core/System.h"

namespace
{
constexpr u32 INSTR_BLR = 0x4E800020;

bool IsTextSection(const std::string& section)
{
  return section == ".text" || section == ".init";
}
}  // namespace

PPCSymbolDB::PPCSymbolDB() = default;

PPCSymbolDB::~PPCSymbolDB() = default;

const Common::Symbol* PPCSymbolDB::AddFunction(const Core::CPUThreadGuard& guard, u32 start_addr)
{
  std::lock_guard lock(m_mutex);
  if (m_functions.contains(start_addr))
    return nullptr;

  Common::Symbol analysed;
  if (!PPCAnalyst::AnalyzeFunction(guard, start_addr, analysed))
    return nullptr;

  Common::Symbol& symbol = m_functions.emplace(start_addr, std::move(analysed)).first->second;
  symbol.type = Common::Symbol::Type::Function;
  m_checksum_to_function[symbol.hash].insert(&symbol);
  return &symbol;
}

void PPCSymbolDB::AddKnownSymbol(const Core::CPUThreadGuard& guard, u32 startAddr, u32 size,
                                 const std::string& name, const std::string& object_name,
                                 Common::Symbol::Type type)
{
  std::lock_guard lock(m_mutex);
  InsertKnownSymbol(guard, startAddr, size, name, object_name, type, m_functions,
                    m_checksum_to_function);
}

void PPCSymbolDB::InsertKnownSymbol(const Core::CPUThreadGuard& guard, u32 start, u32 size,
                                    const std::string& name, const std::string& object_name,
                                    Common::Symbol::Type type, XFuncMap& functions,
                                    XFuncPtrMap& by_hash)
{
  const auto [it, is_new] = functions.try_emplace(start, name);
  Common::Symbol& symbol = it->second;
  symbol.object_name = object_name;
  symbol.type = type;

  if (!is_new)
  {
    // Known already: take the new name and size, and rehash the code.
    symbol.Rename(name);
    symbol.hash = HashSignatureDB::ComputeCodeChecksum(guard, start, start + size - 4);
    symbol.size = size;
    return;
  }

  symbol.address = start;
  if (type != Common::Symbol::Type::Function)
  {
    symbol.size = size;
    return;
  }

  PPCAnalyst::AnalyzeFunction(guard, start, symbol, size);
  // The given size wins over the analysed one.
  if (size != 0 && symbol.size != size)
  {
    WARN_LOG_FMT(SYMBOLS, "Analysed symbol ({}) size mismatch, {} expected but {} computed", name,
                 size, symbol.size);
    symbol.size = size;
  }
  by_hash[symbol.hash].insert(&symbol);
}

void PPCSymbolDB::AddKnownNote(u32 start_addr, u32 size, const std::string& name)
{
  std::lock_guard lock(m_mutex);
  InsertKnownNote(start_addr, size, name, m_notes);
}

void PPCSymbolDB::InsertKnownNote(u32 start, u32 size, const std::string& name, XNoteMap& notes)
{
  const auto [it, is_new] = notes.try_emplace(start);
  Common::Note& note = it->second;
  note.name = name;
  note.size = size;
  if (is_new)
    note.address = start;
}

void PPCSymbolDB::DetermineNoteLayers()
{
  std::lock_guard lock(m_mutex);
  AssignNoteLayers(m_notes);
}

// Each note gets one layer more for every note that starts at or before it
// (in address order) and still covers its start.
void PPCSymbolDB::AssignNoteLayers(XNoteMap& notes)
{
  for (auto& [address, note] : notes)
    note.layer = 0;

  for (auto outer = notes.begin(); outer != notes.end(); ++outer)
  {
    const u32 end = outer->second.address + outer->second.size;
    // Notes that begin strictly inside [address, end) are nested in this one.
    // An empty or wrapping range nests nothing.
    if (end <= outer->first)
      continue;
    const auto stop = notes.lower_bound(end);
    for (auto inner = std::next(outer); inner != stop; ++inner)
      inner->second.layer += 1;
  }
}

const Common::Symbol* PPCSymbolDB::GetSymbolFromAddr(u32 addr) const
{
  std::lock_guard lock(m_mutex);
  if (m_functions.empty())
    return nullptr;

  auto it = m_functions.lower_bound(addr);
  if (it != m_functions.end() && it->second.address == addr)
    return &it->second;
  if (it == m_functions.begin())
    return nullptr;

  const Common::Symbol& before = std::prev(it)->second;
  if (addr >= before.address && addr < before.address + before.size)
    return &before;
  return nullptr;
}

const Common::Note* PPCSymbolDB::GetNoteFromAddr(u32 addr) const
{
  std::lock_guard lock(m_mutex);
  if (m_notes.empty())
    return nullptr;

  auto it = m_notes.lower_bound(addr);
  if (it != m_notes.end() && it->second.address == addr)
    return &it->second;

  // Walk back through the notes starting before addr until one covers it.
  // A layer-0 note has nothing underneath, so the search stops there.
  while (it != m_notes.begin())
  {
    --it;
    if (addr < it->second.address + it->second.size)
      return &it->second;
    if (it->second.layer == 0)
      break;
  }
  return nullptr;
}

void PPCSymbolDB::DeleteFunction(u32 start_address)
{
  std::lock_guard lock(m_mutex);
  const auto it = m_functions.find(start_address);
  if (it == m_functions.end())
    return;

  // Drop the hash index entries too so they cannot dangle.
  Common::Symbol* const symbol = &it->second;
  for (auto entry = m_checksum_to_function.begin(); entry != m_checksum_to_function.end();)
  {
    entry->second.erase(symbol);
    if (entry->second.empty())
      entry = m_checksum_to_function.erase(entry);
    else
      ++entry;
  }
  m_functions.erase(it);
}

void PPCSymbolDB::DeleteNote(u32 start_address)
{
  std::lock_guard lock(m_mutex);
  m_notes.erase(start_address);
}

std::string PPCSymbolDB::GetDescription(u32 addr) const
{
  const Common::Symbol* symbol = GetSymbolFromAddr(addr);
  return symbol ? symbol->name : " --- ";
}

void PPCSymbolDB::FillInCallers()
{
  std::lock_guard lock(m_mutex);
  LinkCallers(m_functions);
}

void PPCSymbolDB::LinkCallers(XFuncMap& functions)
{
  for (auto& [address, symbol] : functions)
    symbol.callers.clear();

  // Calls into addresses that are not a known function start are dropped.
  for (const auto& [caller_address, caller] : functions)
  {
    for (const Common::SCall& call : caller.calls)
    {
      if (const auto callee = functions.find(call.function); callee != functions.end())
        callee->second.callers.emplace_back(caller_address, call.call_address);
    }
  }
}

void PPCSymbolDB::PrintCalls(u32 funcAddr) const
{
  std::lock_guard lock(m_mutex);
  const auto it = m_functions.find(funcAddr);
  if (it == m_functions.end())
  {
    WARN_LOG_FMT(SYMBOLS, "Symbol does not exist");
    return;
  }

  const Common::Symbol& func = it->second;
  DEBUG_LOG_FMT(SYMBOLS, "The function {} at {:08x} calls:", func.name, func.address);
  for (const Common::SCall& call : func.calls)
  {
    if (const auto callee = m_functions.find(call.function); callee != m_functions.end())
      DEBUG_LOG_FMT(SYMBOLS, "* {:08x} : {}", call.call_address, callee->second.name);
  }
}

void PPCSymbolDB::PrintCallers(u32 funcAddr) const
{
  std::lock_guard lock(m_mutex);
  const auto it = m_functions.find(funcAddr);
  if (it == m_functions.end())
    return;

  const Common::Symbol& func = it->second;
  DEBUG_LOG_FMT(SYMBOLS, "The function {} at {:08x} is called by:", func.name, func.address);
  for (const Common::SCall& caller : func.callers)
  {
    if (const auto from = m_functions.find(caller.function); from != m_functions.end())
      DEBUG_LOG_FMT(SYMBOLS, "* {:08x} : {}", caller.call_address, from->second.name);
  }
}

void PPCSymbolDB::LogFunctionCall(u32 addr)
{
  std::lock_guard lock(m_mutex);
  if (const auto it = m_functions.find(addr); it != m_functions.end())
    ++it->second.num_calls;
}

bool PPCSymbolDB::FindMapFile(std::string* existing_map_file, std::string* writable_map_file)
{
  std::string path =
      File::GetUserPath(D_MAPS_IDX) + SConfig::GetInstance().m_debugger_game_id + ".map";

  if (writable_map_file)
    *writable_map_file = path;

  if (!File::Exists(path))
    return false;

  if (existing_map_file)
    *existing_map_file = std::move(path);
  return true;
}

bool PPCSymbolDB::LoadMapOnBoot(const Core::CPUThreadGuard& guard)
{
  std::string map_file;
  if (!FindMapFile(&map_file, nullptr))
    return Clear();

  {
    // Rebooting the same title keeps the table that is already loaded.
    std::lock_guard lock(m_mutex);
    if (!IsEmpty() && map_file == m_map_name)
      return false;
  }

  if (!LoadMap(guard, std::move(map_file)))
    return Clear();
  return true;
}

// ---------------------------------------------------------------------------
// Map file parsing
//
// Accepted input: CodeWarrior linker maps (as found on some discs) and the
// maps SaveSymbolMap writes. A map consists of sections ("<name> section
// layout", or a bare ".text"/".init" line) holding one symbol per line in
// one of three layouts, detected from the first symbol line:
//   2 columns: address name
//   3 columns: address size vaddress [alignment] name
//   4 columns: address size vaddress fileoffset [alignment] name
// Names may carry " (entry of <container>)", which is folded into
// "container::name", and a tab followed by the object file name.
// ---------------------------------------------------------------------------
namespace
{
// Lines are read in chunks of at most this many bytes (including the NUL);
// a longer line is processed as several.
constexpr int MAP_LINE_CHUNK = 512;
constexpr size_t NAME_CAPACITY = 512;

bool IsCSpace(char c)
{
  return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

std::vector<std::string_view> SplitOnSpaces(std::string_view text)
{
  std::vector<std::string_view> words;
  size_t i = 0;
  while (i < text.size())
  {
    while (i < text.size() && IsCSpace(text[i]))
      ++i;
    const size_t begin = i;
    while (i < text.size() && !IsCSpace(text[i]))
      ++i;
    if (i > begin)
      words.push_back(text.substr(begin, i - begin));
  }
  return words;
}

bool IsHexWord(std::string_view word)
{
  return !word.empty() && word.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos;
}

// Lines that never hold a symbol: column headings and link-map entries
// ("  3] name ...").
bool IsHeadingWord(std::string_view word)
{
  return word == "Starting" || word == "address" || word == "-----------------------" ||
         word.ends_with(']');
}

// Number of columns implied by a symbol line, or 0 if the line does not
// start with an 8-digit hex address.
int DetectColumns(std::string_view line)
{
  const std::vector<std::string_view> words = SplitOnSpaces(line);
  if (words.empty() || words[0].size() != 8 || !IsHexWord(words[0]))
    return 0;
  if (words.size() < 3 || !IsHexWord(words[1]) || !IsHexWord(words[2]))
    return 2;
  if (words.size() >= 4 && words[3].size() == 8 && IsHexWord(words[3]))
    return 4;
  return 3;
}

struct MapEntry
{
  u32 address = 0;
  u32 size = 0;
  u32 vaddress = 0;
  char name[NAME_CAPACITY]{};
};

// A leading number in the name field is an alignment value; drop it.
void StripAlignment(char* name)
{
  if (name[0] != ' ' && !(name[0] >= '0' && name[0] <= '9'))
    return;
  const std::string text(StripWhitespace(name));
  u32 alignment = 0;
  std::sscanf(text.c_str(), "%i %511[^\r\n]", &alignment, name);
}

// "func (entry of container) rest" becomes "container::func rest", unless
// the container is a section (starts with '.').
void FoldEntryOf(char* name)
{
  static constexpr std::string_view marker = " (entry of ";
  char* const found = std::strstr(name, marker.data());
  if (!found)
    return;

  const char* const after = found + marker.size();
  char container[NAME_CAPACITY]{};
  std::sscanf(after, "%511s", container);
  char* const close = std::strchr(container, ')');
  if (!close || container[0] == '.')
    return;

  const std::string rest(after + std::strlen(container));
  *close = '\0';
  std::string folded = fmt::format("{}::{}{}", container, std::string_view(name, found - name), rest);
  if (folded.size() >= NAME_CAPACITY)
    folded.resize(NAME_CAPACITY - 1);
  std::memcpy(name, folded.c_str(), folded.size() + 1);
}

void ParseEntry(const char* line, int columns, MapEntry* entry)
{
  u32 file_offset = 0;
  switch (columns)
  {
  case 4:
    std::sscanf(line, "%08x %08x %08x %08x %511[^\r\n]", &entry->address, &entry->size,
                &entry->vaddress, &file_offset, entry->name);
    StripAlignment(entry->name);
    FoldEntryOf(entry->name);
    break;
  case 3:
    std::sscanf(line, "%08x %08x %08x %511[^\r\n]", &entry->address, &entry->size,
                &entry->vaddress, entry->name);
    StripAlignment(entry->name);
    FoldEntryOf(entry->name);
    break;
  default:
    std::sscanf(line, "%08x %511[^\r\n]", &entry->address, entry->name);
    entry->vaddress = entry->address;
    break;
  }
}

// In a "bad" map (possibly for another build) a function is only trusted if
// blr instructions sit right before it and at its last word.
bool BracketedByBlr(const Core::CPUThreadGuard& guard, u32 address, u32 size)
{
  auto& mmu = guard.GetSystem().GetMMU();
  const PowerPC::TryReadInstResult before = mmu.TryReadInstruction(address - 4);
  if (!before.valid || before.hex != INSTR_BLR)
    return false;
  const PowerPC::TryReadInstResult last = mmu.TryReadInstruction(address + size - 4);
  return last.valid && last.hex == INSTR_BLR;
}
}  // namespace

bool PPCSymbolDB::LoadMap(const Core::CPUThreadGuard& guard, std::string filename, bool bad)
{
  File::IOFile file(filename, "r");
  if (!file)
    return false;

  // Parsed into fresh tables, which replace the current ones at the end.
  XFuncMap functions;
  XNoteMap notes;
  XFuncPtrMap by_hash;

  std::string section;
  int columns = 0;
  int accepted = 0;
  int rejected = 0;

  char line[MAP_LINE_CHUNK];
  while (std::fgets(line, MAP_LINE_CHUNK, file.GetHandle()))
  {
    if (std::strlen(line) < 4)
      continue;

    const std::vector<std::string_view> words = SplitOnSpaces(line);
    const std::string first(words.empty() ? std::string_view{} : words[0].substr(0, 255));

    if (first == "UNUSED")
      continue;
    if (StripWhitespace(line).ends_with(" section layout") || first == ".text" || first == ".init")
    {
      section = first;
      continue;
    }
    if (IsHeadingWord(first) || section.empty())
      continue;

    if (columns == 0)
    {
      columns = DetectColumns(line);
      if (columns == 0)
        continue;
    }

    MapEntry entry;
    ParseEntry(line, columns, &entry);
    if (entry.name[0] == '\0')
      continue;

    // name<TAB>object file
    const std::vector<std::string> fields = SplitString(entry.name, '\t');
    const std::string symbol_name(StripWhitespace(fields.empty() ? entry.name : fields[0]));
    const std::string object_name = fields.size() > 1 ? std::string(StripWhitespace(fields[1])) : "";

    // Everything outside .text/.init (notes included) is data.
    const bool is_function = IsTextSection(section);
    const Common::Symbol::Type type =
        is_function ? Common::Symbol::Type::Function : Common::Symbol::Type::Data;

    const u32 address = entry.vaddress;
    const u32 size = entry.size;
    bool good;
    if (is_function)
    {
      good = !bad && PowerPC::MMU::HostIsInstructionRAMAddress(guard, address) &&
             PowerPC::MMU::HostIsInstructionRAMAddress(guard, address + size - 4);
      if (!good)
        good = BracketedByBlr(guard, address, size);
    }
    else
    {
      good = !bad && PowerPC::MMU::HostIsRAMAddress(guard, address) &&
             PowerPC::MMU::HostIsRAMAddress(guard, address + size - 1);
    }

    if (!good)
    {
      ++rejected;
      continue;
    }
    ++accepted;
    if (section == ".note")
      InsertKnownNote(address, size, entry.name, notes);
    else
      InsertKnownSymbol(guard, address, size, symbol_name, object_name, type, functions, by_hash);
  }

  Index(&functions);
  AssignNoteLayers(notes);
  LinkCallers(functions);

  std::lock_guard lock(m_mutex);
  std::swap(m_functions, functions);
  std::swap(m_notes, notes);
  std::swap(m_checksum_to_function, by_hash);
  std::swap(m_map_name, filename);

  NOTICE_LOG_FMT(SYMBOLS, "{} symbols loaded, {} symbols ignored.", accepted, rejected);
  return true;
}

// ---------------------------------------------------------------------------
// Map file writing
// ---------------------------------------------------------------------------

bool PPCSymbolDB::SaveSymbolMap(const std::string& filename) const
{
  File::IOFile file(filename, "w");
  if (!file)
    return false;

  std::lock_guard lock(m_mutex);

  // address size vaddress alignment name [TAB object]
  const auto write_symbols = [&](const char* header, Common::Symbol::Type type) {
    file.WriteString(header);
    for (const auto& [address, symbol] : m_functions)
    {
      if (symbol.type != type)
        continue;
      std::string line = fmt::format("{:08x} {:06x} {:08x} {} {}", symbol.address, symbol.size,
                                     symbol.address, 0, symbol.name);
      if (!symbol.object_name.empty())
        line += fmt::format(" \t{}", symbol.object_name);
      line += '\n';
      file.WriteString(line);
    }
  };

  write_symbols(".text section layout\n", Common::Symbol::Type::Function);
  write_symbols("\n.data section layout\n", Common::Symbol::Type::Data);

  file.WriteString("\n.note section layout\n");
  for (const auto& [address, note] : m_notes)
  {
    file.WriteString(fmt::format("{:08x} {:06x} {:08x} {} {}\n", note.address, note.size,
                                 note.address, 0, note.name));
  }
  return true;
}

// A disassembly listing of every function, for reading only (it is not a
// format LoadMap understands).
bool PPCSymbolDB::SaveCodeMap(const Core::CPUThreadGuard& guard, const std::string& filename) const
{
  constexpr int NAME_COLUMN_WIDTH = 30;

  File::IOFile file(filename, "w");
  if (!file)
    return false;

  file.WriteString(".text\n");

  std::lock_guard lock(m_mutex);
  const auto& debug_interface = guard.GetSystem().GetPowerPC().GetDebugInterface();

  u32 listed_until = 0;
  for (const auto& [start, symbol] : m_functions)
  {
    // Functions nested in one already listed only get a marker line.
    if (symbol.address + symbol.size <= listed_until)
    {
      file.WriteString(fmt::format("// {:08x} beginning of {}\n", symbol.address, symbol.name));
      continue;
    }

    file.WriteString(fmt::format("\n{}:\n", symbol.name));
    listed_until = symbol.address + symbol.size;
    for (u32 address = symbol.address; address < listed_until; address += 4)
    {
      file.WriteString(fmt::format("{:08x} {:<{}.{}} {}\n", address, symbol.name,
                                   NAME_COLUMN_WIDTH, NAME_COLUMN_WIDTH,
                                   debug_interface.Disassemble(&guard, address)));
    }
  }
  return true;
}
