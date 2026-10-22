// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"

namespace Core
{
class CPUThreadGuard;
}

// A table of guest code/data symbols keyed by address. The PowerPC symbol database derives
// from it and adds analysis.

namespace Common
{
// A call edge: `function` is the callee or caller, `call_address` the branch instruction.
struct SCall
{
  SCall(u32 a, u32 b) : function(a), call_address(b) {}
  u32 function;
  u32 call_address;
};

// A user note covering an address range.
struct Note
{
  std::string name;
  u32 address = 0;
  u32 size = 0;
  int layer = 0;

  Note() = default;
};

struct Symbol
{
  enum class Type
  {
    Function,
    Data,
  };

  Symbol() = default;
  explicit Symbol(const std::string& symbol_name) { Rename(symbol_name); }

  // Sets name, function_name (the name without parameters) and demangled_name.
  void Rename(const std::string& symbol_name);

  bool IsDemangled() const { return !demangled_name.empty(); }

  const std::string& GetDisplayName(bool use_demangled_name) const
  {
    return use_demangled_name && IsDemangled() ? demangled_name : name;
  }

  std::string name;
  std::string demangled_name;  // empty when the name is not a CodeWarrior mangled name
  std::string function_name;   // name up to the first '(' or ' '
  std::string object_name;     // object/source file the symbol came from
  std::vector<SCall> callers;  // functions calling this one
  std::vector<SCall> calls;    // functions this one calls
  u32 hash = 0;                // code hash, used to recognise known (HLE) functions
  u32 address = 0;
  u32 flags = 0;
  u32 size = 0;
  int num_calls = 0;
  Type type = Type::Function;
  int index = 0;  // position in the table, for the disassembly view's colours
  bool analyzed = false;
};

enum
{
  FFLAG_TIMERINSTRUCTIONS = (1 << 0),
  FFLAG_LEAF = (1 << 1),
  FFLAG_ONLYCALLSNICELEAFS = (1 << 2),
  FFLAG_EVIL = (1 << 3),
  FFLAG_RFI = (1 << 4),
  FFLAG_STRAIGHT = (1 << 5)
};

class SymbolDB
{
public:
  using XFuncMap = std::map<u32, Symbol>;
  using XNoteMap = std::map<u32, Note>;
  using XFuncPtrMap = std::map<u32, std::set<Symbol*>>;

  SymbolDB();
  virtual ~SymbolDB();

  virtual const Symbol* GetSymbolFromAddr(u32 addr) const { return nullptr; }
  virtual const Symbol* AddFunction(const Core::CPUThreadGuard& guard, u32 start_addr)
  {
    return nullptr;
  }
  void AddCompleteSymbol(const Symbol& symbol);
  bool RenameSymbol(const Symbol& symbol, const std::string& symbol_name);
  bool RenameSymbol(const Symbol& symbol, const std::string& symbol_name,
                    const std::string& object_name);

  // Lookups by function_name (first match in address order) and by hash.
  const Symbol* GetSymbolFromName(std::string_view name) const;
  std::vector<const Symbol*> GetSymbolsFromName(std::string_view name) const;
  const Symbol* GetSymbolFromHash(u32 hash) const;
  std::vector<const Symbol*> GetSymbolsFromHash(u32 hash) const;

  template <typename F>
  void ForEachSymbol(F f) const
  {
    std::lock_guard lock(m_mutex);
    for (const auto& entry : m_functions)
      f(entry.second);
  }

  // f may change a symbol but not its address.
  template <typename F>
  void ForEachSymbolWithMutation(F f)
  {
    std::lock_guard lock(m_mutex);
    for (auto& [address, symbol] : m_functions)
    {
      f(symbol);
      ASSERT_MSG(COMMON, address == symbol.address, "Symbol address was unexpectedly changed");
    }
  }

  template <typename F>
  void ForEachNote(F f) const
  {
    std::lock_guard lock(m_mutex);
    for (const auto& entry : m_notes)
      f(entry.second);
  }

  // f may change a note but not its address.
  template <typename F>
  void ForEachNoteWithMutation(F f)
  {
    std::lock_guard lock(m_mutex);
    for (auto& [address, note] : m_notes)
    {
      f(note);
      ASSERT_MSG(COMMON, address == note.address, "Note address was unexpectedly changed");
    }
  }

  bool IsEmpty() const;
  // Drops every symbol and note. Returns false when there was nothing to drop. The prefix is
  // currently ignored.
  bool Clear(const char* prefix = "");
  // Logs the symbol table.
  void List();
  // Renumbers Symbol::index in address order.
  void Index();

protected:
  static void Index(XFuncMap* functions);

  XFuncMap m_functions;
  XNoteMap m_notes;
  XFuncPtrMap m_checksum_to_function;
  std::string m_map_name;
  mutable std::recursive_mutex m_mutex;
};
}  // namespace Common
