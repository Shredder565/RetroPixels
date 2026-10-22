// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/SymbolDB.h"

#include <mutex>
#include <string>

#include "Common/CWDemangler.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"

namespace Common
{
namespace
{
// "foo(int) const" -> "foo"; "foo bar" -> "foo".
std::string BareName(const std::string& symbol_name)
{
  const std::size_t end = symbol_name.find_first_of("( ");
  return symbol_name.substr(0, end);
}
}  // namespace

void Symbol::Rename(const std::string& symbol_name)
{
  name = symbol_name;
  function_name = BareName(symbol_name);
  demangled_name = CWDemangler::demangle(symbol_name, CWDemangler::DemangleOptions()).value_or("");
}

SymbolDB::SymbolDB() = default;
SymbolDB::~SymbolDB() = default;

void SymbolDB::List()
{
  std::lock_guard lock(m_mutex);
  for (const auto& [address, symbol] : m_functions)
  {
    DEBUG_LOG_FMT(OSHLE, "{} @ {:08x}: {} bytes (hash {:08x}) : {} calls", symbol.name,
                  symbol.address, symbol.size, symbol.hash, symbol.num_calls);
  }
  INFO_LOG_FMT(OSHLE, "{} functions known in this program above.", m_functions.size());
}

bool SymbolDB::IsEmpty() const
{
  std::lock_guard lock(m_mutex);
  return m_functions.empty() && m_notes.empty();
}

bool SymbolDB::Clear(const char* prefix)
{
  std::lock_guard lock(m_mutex);
  m_map_name.clear();
  if (m_functions.empty() && m_notes.empty())
    return false;

  m_checksum_to_function.clear();
  m_notes.clear();
  m_functions.clear();
  return true;
}

// Numbers the functions 0, 1, 2, ... in address order.
void SymbolDB::Index(XFuncMap* functions)
{
  int index = 0;
  for (auto& [address, symbol] : *functions)
    symbol.index = index++;
}

void SymbolDB::Index()
{
  std::lock_guard lock(m_mutex);
  Index(&m_functions);
}

const Symbol* SymbolDB::GetSymbolFromName(std::string_view name) const
{
  std::lock_guard lock(m_mutex);
  for (const auto& entry : m_functions)
  {
    if (entry.second.function_name == name)
      return &entry.second;
  }
  return nullptr;
}

std::vector<const Symbol*> SymbolDB::GetSymbolsFromName(std::string_view name) const
{
  std::lock_guard lock(m_mutex);
  std::vector<const Symbol*> found;
  for (const auto& entry : m_functions)
  {
    if (entry.second.function_name == name)
      found.push_back(&entry.second);
  }
  return found;
}

const Symbol* SymbolDB::GetSymbolFromHash(u32 hash) const
{
  std::lock_guard lock(m_mutex);
  const auto it = m_checksum_to_function.find(hash);
  if (it == m_checksum_to_function.end() || it->second.empty())
    return nullptr;
  return *it->second.begin();
}

std::vector<const Symbol*> SymbolDB::GetSymbolsFromHash(u32 hash) const
{
  std::lock_guard lock(m_mutex);
  const auto it = m_checksum_to_function.find(hash);
  if (it == m_checksum_to_function.end())
    return {};
  return std::vector<const Symbol*>(it->second.begin(), it->second.end());
}

void SymbolDB::AddCompleteSymbol(const Symbol& symbol)
{
  std::lock_guard lock(m_mutex);
  m_functions[symbol.address] = symbol;
}

bool SymbolDB::RenameSymbol(const Symbol& symbol, const std::string& symbol_name)
{
  std::lock_guard lock(m_mutex);
  const auto it = m_functions.find(symbol.address);
  if (it == m_functions.end())
    return false;
  it->second.Rename(symbol_name);
  return true;
}

bool SymbolDB::RenameSymbol(const Symbol& symbol, const std::string& symbol_name,
                            const std::string& object_name)
{
  std::lock_guard lock(m_mutex);
  const auto it = m_functions.find(symbol.address);
  if (it == m_functions.end())
    return false;
  it->second.Rename(symbol_name);
  it->second.object_name = object_name;
  return true;
}
}  // namespace Common
