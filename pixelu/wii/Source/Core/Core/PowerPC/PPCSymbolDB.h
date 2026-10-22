// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// The PowerPC symbol table: functions found by analysis or loaded from map
// files, data symbols, and user notes. Also reads and writes CodeWarrior-
// style symbol maps and writes disassembly "code maps".

#pragma once

#include <string>

#include "Common/CommonTypes.h"
#include "Common/SymbolDB.h"

namespace Core
{
class CPUThreadGuard;
}  // namespace Core

class PPCSymbolDB : public Common::SymbolDB
{
public:
  PPCSymbolDB();
  ~PPCSymbolDB() override;

  // Analyses and adds the function at start_addr. Returns null if it was
  // already known or could not be analysed.
  const Common::Symbol* AddFunction(const Core::CPUThreadGuard& guard, u32 start_addr) override;
  // Adds a symbol whose name and size are known (or refreshes an existing one).
  void AddKnownSymbol(const Core::CPUThreadGuard& guard, u32 startAddr, u32 size,
                      const std::string& name, const std::string& object_name,
                      Common::Symbol::Type type = Common::Symbol::Type::Function);
  void AddKnownNote(u32 start_addr, u32 size, const std::string& name);

  // The symbol starting at, or containing, addr.
  const Common::Symbol* GetSymbolFromAddr(u32 addr) const override;
  bool NoteExists() const { return !m_notes.empty(); }
  // The innermost note containing addr.
  const Common::Note* GetNoteFromAddr(u32 addr) const;
  // Recomputes Note::layer (how many notes enclose each one).
  void DetermineNoteLayers();
  void DeleteFunction(u32 start_address);
  void DeleteNote(u32 start_address);

  // Symbol name for addr, or " --- ".
  std::string GetDescription(u32 addr) const;

  // Rebuilds every symbol's callers from the others' calls.
  void FillInCallers();

  // Loads the map for the running title, if there is one; otherwise clears
  // the table. Returns true if the table changed.
  bool LoadMapOnBoot(const Core::CPUThreadGuard& guard);
  // bad: the map may be for another build; only accept functions that are
  // bracketed by blr instructions in memory.
  bool LoadMap(const Core::CPUThreadGuard& guard, std::string filename, bool bad = false);
  bool SaveSymbolMap(const std::string& filename) const;
  bool SaveCodeMap(const Core::CPUThreadGuard& guard, const std::string& filename) const;

  void PrintCalls(u32 funcAddr) const;
  void PrintCallers(u32 funcAddr) const;
  void LogFunctionCall(u32 addr);

  // Map file locations for the running title. Returns whether one exists.
  static bool FindMapFile(std::string* existing_map_file, std::string* writable_map_file);

private:
  static void InsertKnownSymbol(const Core::CPUThreadGuard& guard, u32 start, u32 size,
                                const std::string& name, const std::string& object_name,
                                Common::Symbol::Type type, XFuncMap& functions,
                                XFuncPtrMap& by_hash);
  static void InsertKnownNote(u32 start, u32 size, const std::string& name, XNoteMap& notes);
  static void AssignNoteLayers(XNoteMap& notes);
  static void LinkCallers(XFuncMap& functions);
};
