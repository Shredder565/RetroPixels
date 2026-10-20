// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/PowerPC/SignatureDB/MEGASignatureDB.h"

#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <utility>

#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PPCSymbolDB.h"

namespace
{
constexpr size_t HEX_DIGITS_PER_WORD = 8;
constexpr std::string_view WILDCARD_WORD = "........";

// Reads the fields of one signature line in order.
class SignatureLineReader
{
public:
  explicit SignatureLineReader(const std::string& line) : m_stream(line) {}

  // The first token: concatenated 8-digit words.
  bool ReadCode(std::vector<u32>* code)
  {
    std::string digits;
    if (!(m_stream >> digits) || digits.size() % HEX_DIGITS_PER_WORD != 0)
      return false;

    for (size_t pos = 0; pos < digits.size(); pos += HEX_DIGITS_PER_WORD)
    {
      const std::string word_text = digits.substr(pos, HEX_DIGITS_PER_WORD);
      const u32 word = static_cast<u32>(std::strtoul(word_text.c_str(), nullptr, 16));
      // A zero word is reserved for wildcards; a literal zero is rejected.
      if (word == 0 && word_text != WILDCARD_WORD)
      {
        WARN_LOG_FMT(SYMBOLS, "MEGA database failed to parse code");
        return false;
      }
      code->push_back(word);
    }
    return true;
  }

  // ":offset name": the offset token is ignored.
  bool ReadName(std::string* name)
  {
    std::string offset_token;
    return (m_stream >> offset_token) && ReadTextUntilNextRef(name);
  }

  // Any number of "^offset name" pairs up to the end of the line.
  bool ReadRefs(std::vector<MEGASignatureReference>* refs)
  {
    u32 index = 1;
    std::string token;
    while (m_stream && (m_stream >> token) && !token.empty())
    {
      const std::string digits = token.substr(1);  // drop the '^'
      const char* const begin = digits.c_str();
      char* end = nullptr;
      const u64 offset = std::strtoul(begin, &end, 16);
      if (end == begin || offset > std::numeric_limits<u32>::max())
      {
        WARN_LOG_FMT(SYMBOLS, "MEGA database failed to parse reference {} offset", index);
        return false;
      }

      std::string ref_name;
      if (!ReadTextUntilNextRef(&ref_name))
      {
        WARN_LOG_FMT(SYMBOLS, "MEGA database failed to parse reference {} name", index);
        return false;
      }
      refs->emplace_back(static_cast<u32>(offset), std::move(ref_name));
      ++index;
      token.clear();
    }
    return true;
  }

private:
  // Takes the rest of the line up to the next " ^" as a (trimmed, non-empty)
  // name and leaves the remainder for the following reads.
  bool ReadTextUntilNextRef(std::string* text)
  {
    std::string rest;
    std::getline(m_stream, rest);
    size_t cut = rest.find(" ^");
    *text = StripWhitespace(rest.substr(0, cut));
    if (text->empty())
      return false;

    if (cut == std::string::npos)
      cut = rest.size();
    m_stream.str(rest.substr(cut));
    m_stream.clear();
    return true;
  }

  std::istringstream m_stream;
};

bool Matches(const Core::CPUThreadGuard& guard, u32 address, u32 size, const MEGASignature& sig)
{
  if (size != sig.code.size() * sizeof(u32))
    return false;

  for (size_t i = 0; i < sig.code.size(); ++i)
  {
    const u32 expected = sig.code[i];
    if (expected == 0)
      continue;
    const u32 actual =
        PowerPC::MMU::HostRead<u32>(guard, static_cast<u32>(address + i * sizeof(u32)));
    if (actual != expected)
      return false;
  }
  return true;
}
}  // namespace

MEGASignatureDB::MEGASignatureDB() = default;
MEGASignatureDB::~MEGASignatureDB() = default;

void MEGASignatureDB::Clear()
{
  m_signatures.clear();
}

bool MEGASignatureDB::Load(const std::string& file_path)
{
  std::ifstream file;
  File::OpenFStream(file, file_path, std::ios_base::in);
  if (!file)
    return false;

  std::string line;
  for (size_t line_number = 1; std::getline(file, line); ++line_number)
  {
    SignatureLineReader reader(line);
    MEGASignature sig;
    if (reader.ReadCode(&sig.code) && reader.ReadName(&sig.name) && reader.ReadRefs(&sig.refs))
      m_signatures.push_back(std::move(sig));
    else
      WARN_LOG_FMT(SYMBOLS, "MEGA database failed to parse line {}", line_number);
  }
  return true;
}

bool MEGASignatureDB::Save(const std::string& file_path) const
{
  ERROR_LOG_FMT(SYMBOLS, "MEGA database save unsupported yet.");
  return false;
}

void MEGASignatureDB::Apply(const Core::CPUThreadGuard& guard, PPCSymbolDB* symbol_db) const
{
  // The first matching signature names the symbol.
  symbol_db->ForEachSymbol([&](const Common::Symbol& symbol) {
    for (const MEGASignature& sig : m_signatures)
    {
      if (!Matches(guard, symbol.address, symbol.size, sig))
        continue;
      symbol_db->RenameSymbol(symbol, sig.name);
      INFO_LOG_FMT(SYMBOLS, "Found {} at {:08x} (size: {:08x})!", sig.name, symbol.address,
                   symbol.size);
      break;
    }
  });
  symbol_db->Index();
}

void MEGASignatureDB::Populate(const PPCSymbolDB* func_db, const std::string& filter)
{
  ERROR_LOG_FMT(SYMBOLS, "MEGA database can't be populated yet.");
}

bool MEGASignatureDB::Add(const Core::CPUThreadGuard& guard, u32 startAddr, u32 size,
                          const std::string& name)
{
  ERROR_LOG_FMT(SYMBOLS, "Can't add symbol to MEGA database yet.");
  return false;
}

void MEGASignatureDB::List() const
{
  for (const MEGASignature& sig : m_signatures)
    DEBUG_LOG_FMT(SYMBOLS, "{} : {} bytes", sig.name, sig.code.size() * sizeof(u32));
  INFO_LOG_FMT(SYMBOLS, "{} functions known in current MEGA database.", m_signatures.size());
}
