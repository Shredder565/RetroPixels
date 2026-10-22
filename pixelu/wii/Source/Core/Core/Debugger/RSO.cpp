// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/Debugger/RSO.h"

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <list>
#include <string>
#include <utility>
#include <vector>

#include "Common/Logging/Log.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PPCSymbolDB.h"

// RSO files are the relocatable modules of the Wii SDK. Once linked by the guest's loader they
// form a doubly linked chain in memory, each starting with the header described in RSO.h; all
// "offsets" read here are absolute addresses at that point.

namespace
{
// Reads a guest structure field by field, front to back.
class FieldReader
{
public:
  FieldReader(const Core::CPUThreadGuard& guard, u32 address) : m_guard(guard), m_next(address) {}

  u32 Word() { return Take<u32>(); }
  u8 Byte() { return Take<u8>(); }

private:
  template <typename T> T Take()
  {
    const T value = PowerPC::MMU::HostRead<T>(m_guard, m_next);
    m_next += static_cast<u32>(sizeof(T));
    return value;
  }

  const Core::CPUThreadGuard& m_guard;
  u32 m_next;
};

// Appends `count` consecutive records of a table to `table`; `read_one` fills one record.
template <typename Record, typename ReadOne>
void AppendTable(const Core::CPUThreadGuard& guard, u32 address, std::size_t count,
                 std::vector<Record>& table, ReadOne read_one)
{
  for (std::size_t n = 0; n < count; ++n)
  {
    FieldReader reader(guard, address);
    Record record{};
    read_one(reader, record);
    table.push_back(record);
    address += static_cast<u32>(sizeof(Record));
  }
}

template <typename Entry> void ReadRelocation(FieldReader& reader, Entry& entry)
{
  entry.r_offset = reader.Word();
  entry.r_info = reader.Word();
  entry.r_addend = reader.Word();
}

// A table size that isn't a whole number of records means a corrupt header; the remainder is
// ignored.
template <typename Record> std::size_t RecordCount(u32 table_size, const char* what)
{
  if (table_size % sizeof(Record) != 0)
    WARN_LOG_FMT(SYMBOLS, "RSO {} has an incoherent size ({:08x})", what, table_size);
  return table_size / sizeof(Record);
}

// Names longer than this are assumed to come from a corrupt header.
constexpr u32 MAX_MODULE_NAME_LENGTH = 0x100;
}  // namespace

// RSOHeaderView ------------------------------------------------------------------------------

void RSOHeaderView::Load(const Core::CPUThreadGuard& guard, u32 address)
{
  m_address = address;
  FieldReader reader(guard, address);

  RSOEntry& entry = m_header.entry;
  entry.next_entry = reader.Word();
  entry.prev_entry = reader.Word();
  entry.section_count = reader.Word();
  entry.section_table_offset = reader.Word();
  entry.name_offset = reader.Word();
  entry.name_size = reader.Word();
  entry.version = reader.Word();
  entry.bss_size = reader.Word();

  RSOSectionInfo& sections = m_header.section_info;
  sections.prolog_section_index = reader.Byte();
  sections.epilog_section_index = reader.Byte();
  sections.unresolved_section_index = reader.Byte();
  sections.bss_section_index = reader.Byte();
  sections.prolog_offset = reader.Word();
  sections.epilog_offset = reader.Word();
  sections.unresolved_offset = reader.Word();

  RSORelocationTables& relocations = m_header.relocation_tables;
  relocations.internals_offset = reader.Word();
  relocations.internals_size = reader.Word();
  relocations.externals_offset = reader.Word();
  relocations.externals_size = reader.Word();

  RSOSymbolTables& symbols = m_header.symbol_tables;
  symbols.exports_offset = reader.Word();
  symbols.exports_size = reader.Word();
  symbols.exports_name_table = reader.Word();
  symbols.imports_offset = reader.Word();
  symbols.imports_size = reader.Word();
  symbols.imports_name_table = reader.Word();

  m_name = PowerPC::MMU::HostGetString(guard, entry.name_offset,
                                       std::min(entry.name_size, MAX_MODULE_NAME_LENGTH));
}

const std::string& RSOHeaderView::GetName() const
{
  return m_name;
}

u32 RSOHeaderView::GetImportsNameTable() const
{
  return m_header.symbol_tables.imports_name_table;
}

u32 RSOHeaderView::GetVersion() const
{
  return m_header.entry.version;
}

u32 RSOHeaderView::GetExportsOffset() const
{
  return m_header.symbol_tables.exports_offset;
}

u32 RSOHeaderView::GetPrologSectionIndex() const
{
  return m_header.section_info.prolog_section_index;
}

u32 RSOHeaderView::GetNextEntry() const
{
  return m_header.entry.next_entry;
}

u32 RSOHeaderView::GetInternalsSize() const
{
  return m_header.relocation_tables.internals_size;
}

u32 RSOHeaderView::GetEpilogSectionOffset() const
{
  return m_header.section_info.epilog_offset;
}

u32 RSOHeaderView::GetImportsSize() const
{
  return m_header.symbol_tables.imports_size;
}

u32 RSOHeaderView::GetSectionCount() const
{
  return m_header.entry.section_count;
}

u32 RSOHeaderView::GetUnresolvedSectionIndex() const
{
  return m_header.section_info.unresolved_section_index;
}

u32 RSOHeaderView::GetExternalsOffset() const
{
  return m_header.relocation_tables.externals_offset;
}

u32 RSOHeaderView::GetExportsNameTable() const
{
  return m_header.symbol_tables.exports_name_table;
}

u32 RSOHeaderView::GetPrevEntry() const
{
  return m_header.entry.prev_entry;
}

u32 RSOHeaderView::GetBssSectionIndex() const
{
  return m_header.section_info.bss_section_index;
}

u32 RSOHeaderView::GetImportsOffset() const
{
  return m_header.symbol_tables.imports_offset;
}

u32 RSOHeaderView::GetPrologSectionOffset() const
{
  return m_header.section_info.prolog_offset;
}

u32 RSOHeaderView::GetSectionTableOffset() const
{
  return m_header.entry.section_table_offset;
}

u32 RSOHeaderView::GetExternalsSize() const
{
  return m_header.relocation_tables.externals_size;
}

u32 RSOHeaderView::GetEpilogSectionIndex() const
{
  return m_header.section_info.epilog_section_index;
}

u32 RSOHeaderView::GetBssSectionSize() const
{
  return m_header.entry.bss_size;
}

u32 RSOHeaderView::GetExportsSize() const
{
  return m_header.symbol_tables.exports_size;
}

u32 RSOHeaderView::GetInternalsOffset() const
{
  return m_header.relocation_tables.internals_offset;
}

u32 RSOHeaderView::GetUnresolvedSectionOffset() const
{
  return m_header.section_info.unresolved_offset;
}

// Table views ----------------------------------------------------------------------------------

void RSOSectionsView::Load(const Core::CPUThreadGuard& guard, u32 address, std::size_t count)
{
  m_address = address;
  AppendTable(guard, address, count, m_sections, [](FieldReader& reader, RSOSection& section) {
    section.offset = reader.Word();
    section.size = reader.Word();
  });
}

void RSOImportsView::Load(const Core::CPUThreadGuard& guard, u32 address, std::size_t count)
{
  m_address = address;
  AppendTable(guard, address, count, m_imports, [](FieldReader& reader, RSOImport& entry) {
    entry.name_offset = reader.Word();
    entry.code_offset = reader.Word();
    entry.entry_offset = reader.Word();
  });
}

void RSOExportsView::Load(const Core::CPUThreadGuard& guard, u32 address, std::size_t count)
{
  m_address = address;
  AppendTable(guard, address, count, m_exports, [](FieldReader& reader, RSOExport& entry) {
    entry.name_offset = reader.Word();
    entry.code_offset = reader.Word();
    entry.section_index = reader.Word();
    entry.hash = reader.Word();
  });
}

void RSOInternalsView::Load(const Core::CPUThreadGuard& guard, u32 address, std::size_t count)
{
  m_address = address;
  AppendTable(guard, address, count, m_entries, ReadRelocation<RSOInternalsEntry>);
}

void RSOExternalsView::Load(const Core::CPUThreadGuard& guard, u32 address, std::size_t count)
{
  m_address = address;
  AppendTable(guard, address, count, m_entries, ReadRelocation<RSOExternalsEntry>);
}

const std::vector<RSOSection>& RSOSectionsView::GetSections() const
{
  return m_sections;
}

std::size_t RSOSectionsView::Count() const
{
  return m_sections.size();
}

const RSOSection& RSOSectionsView::GetSection(std::size_t index) const
{
  return m_sections.at(index);
}

const std::vector<RSOImport>& RSOImportsView::GetImports() const
{
  return m_imports;
}

const RSOImport& RSOImportsView::GetImport(std::size_t index) const
{
  return m_imports.at(index);
}

std::size_t RSOImportsView::Count() const
{
  return m_imports.size();
}

std::size_t RSOExportsView::Count() const
{
  return m_exports.size();
}

const std::vector<RSOExport>& RSOExportsView::GetExports() const
{
  return m_exports;
}

const RSOExport& RSOExportsView::GetExport(std::size_t index) const
{
  return m_exports.at(index);
}

const RSOInternalsEntry& RSOInternalsView::GetEntry(std::size_t index) const
{
  return m_entries.at(index);
}

std::size_t RSOInternalsView::Count() const
{
  return m_entries.size();
}

const std::vector<RSOInternalsEntry>& RSOInternalsView::GetEntries() const
{
  return m_entries;
}

const std::vector<RSOExternalsEntry>& RSOExternalsView::GetEntries() const
{
  return m_entries;
}

std::size_t RSOExternalsView::Count() const
{
  return m_entries.size();
}

const RSOExternalsEntry& RSOExternalsView::GetEntry(std::size_t index) const
{
  return m_entries.at(index);
}

// RSOView --------------------------------------------------------------------------------------

void RSOView::LoadHeader(const Core::CPUThreadGuard& guard, u32 address)
{
  m_address = address;
  m_header.Load(guard, address);
}

void RSOView::LoadAll(const Core::CPUThreadGuard& guard, u32 address)
{
  // The header first, since it locates everything else; then the tables in header order.
  LoadHeader(guard, address);
  using TableLoader = void (RSOView::*)(const Core::CPUThreadGuard&);
  for (const TableLoader load_table :
       {&RSOView::LoadSections, &RSOView::LoadImports, &RSOView::LoadExports,
        &RSOView::LoadInternals, &RSOView::LoadExternals})
  {
    (this->*load_table)(guard);
  }
}

void RSOView::LoadSections(const Core::CPUThreadGuard& guard)
{
  m_sections.Load(guard, m_header.GetSectionTableOffset(), m_header.GetSectionCount());
}

void RSOView::LoadImports(const Core::CPUThreadGuard& guard)
{
  const std::size_t count = RecordCount<RSOImport>(m_header.GetImportsSize(), "Imports Table");
  m_imports.Load(guard, m_header.GetImportsOffset(), count);
}

void RSOView::LoadExports(const Core::CPUThreadGuard& guard)
{
  const std::size_t count = RecordCount<RSOExport>(m_header.GetExportsSize(), "Exports Table");
  m_exports.Load(guard, m_header.GetExportsOffset(), count);
}

void RSOView::LoadInternals(const Core::CPUThreadGuard& guard)
{
  const std::size_t count =
      RecordCount<RSOInternalsEntry>(m_header.GetInternalsSize(), "Internals Relocation Table");
  m_internals.Load(guard, m_header.GetInternalsOffset(), count);
}

void RSOView::LoadExternals(const Core::CPUThreadGuard& guard)
{
  const std::size_t count =
      RecordCount<RSOExternalsEntry>(m_header.GetExternalsSize(), "Externals Relocation Table");
  m_externals.Load(guard, m_header.GetExternalsOffset(), count);
}

void RSOView::Apply(const Core::CPUThreadGuard& guard, PPCSymbolDB* symbol_db) const
{
  const std::string module_name = GetName();
  for (const RSOExport& exported : GetExports())
  {
    const u32 address = GetExportAddress(exported);
    if (address == 0)
      continue;

    // Code exports become (or rename) functions; anything the function scanner rejects is
    // recorded as a data symbol.
    const Common::Symbol* function = symbol_db->AddFunction(guard, address);
    if (function == nullptr)
      function = symbol_db->GetSymbolFromAddr(address);

    const std::string symbol_name = GetExportName(guard, exported);
    if (function != nullptr)
    {
      symbol_db->RenameSymbol(*function, symbol_name, module_name);
      continue;
    }
    symbol_db->AddKnownSymbol(guard, address, 0, symbol_name, module_name,
                              Common::Symbol::Type::Data);
  }
  DEBUG_LOG_FMT(SYMBOLS, "RSO({}): {} symbols applied", module_name, GetExportsCount());
}

u32 RSOView::GetAddress() const
{
  return m_address;
}

u32 RSOView::GetVersion() const
{
  return m_header.GetVersion();
}

u32 RSOView::GetPrevEntry() const
{
  return m_header.GetPrevEntry();
}

u32 RSOView::GetNextEntry() const
{
  return m_header.GetNextEntry();
}

const std::string& RSOView::GetName() const
{
  return m_header.GetName();
}

std::size_t RSOView::GetSectionCount() const
{
  return m_sections.Count();
}

const std::vector<RSOSection>& RSOView::GetSections() const
{
  return m_sections.GetSections();
}

const RSOSection& RSOView::GetSection(std::size_t index) const
{
  return m_sections.GetSection(index);
}

const std::vector<RSOImport>& RSOView::GetImports() const
{
  return m_imports.GetImports();
}

std::size_t RSOView::GetImportsCount() const
{
  return m_imports.Count();
}

const RSOImport& RSOView::GetImport(std::size_t index) const
{
  return m_imports.GetImport(index);
}

std::string RSOView::GetImportName(const Core::CPUThreadGuard& guard,
                                   const RSOImport& rso_import) const
{
  const u32 name_address = m_header.GetImportsNameTable() + rso_import.name_offset;
  return PowerPC::MMU::HostGetString(guard, name_address);
}

std::string RSOView::GetName(const Core::CPUThreadGuard& guard, const RSOImport& rso_import) const
{
  return GetImportName(guard, rso_import);
}

const std::vector<RSOExport>& RSOView::GetExports() const
{
  return m_exports.GetExports();
}

std::size_t RSOView::GetExportsCount() const
{
  return m_exports.Count();
}

const RSOExport& RSOView::GetExport(std::size_t index) const
{
  return m_exports.GetExport(index);
}

std::string RSOView::GetExportName(const Core::CPUThreadGuard& guard,
                                   const RSOExport& rso_export) const
{
  const u32 name_address = m_header.GetExportsNameTable() + rso_export.name_offset;
  return PowerPC::MMU::HostGetString(guard, name_address);
}

std::string RSOView::GetName(const Core::CPUThreadGuard& guard, const RSOExport& rso_export) const
{
  return GetExportName(guard, rso_export);
}

u32 RSOView::GetExportAddress(const RSOExport& rso_export) const
{
  // Exports are relative to their section; an export with a bad section index has no address.
  if (rso_export.section_index >= GetSectionCount())
    return 0;
  return GetSection(rso_export.section_index).offset + rso_export.code_offset;
}

const std::vector<RSOInternalsEntry>& RSOView::GetInternals() const
{
  return m_internals.GetEntries();
}

std::size_t RSOView::GetInternalsCount() const
{
  return m_internals.Count();
}

const RSOInternalsEntry& RSOView::GetInternalsEntry(std::size_t index) const
{
  return m_internals.GetEntry(index);
}

const std::vector<RSOExternalsEntry>& RSOView::GetExternals() const
{
  return m_externals.GetEntries();
}

std::size_t RSOView::GetExternalsCount() const
{
  return m_externals.Count();
}

const RSOExternalsEntry& RSOView::GetExternalsEntry(std::size_t index) const
{
  return m_externals.GetEntry(index);
}

namespace
{
// The prolog, epilog and unresolved-symbol handler are each given as a section index plus an
// offset into that section. Index 0 means the module has no such function.
u32 SpecialFunctionAddress(const RSOView& view, u32 section_index, u32 offset,
                           const char* missing_message, const char* bad_index_message)
{
  if (section_index == 0)
  {
    WARN_LOG_FMT(SYMBOLS, "{}", missing_message);
    return 0;
  }
  if (section_index >= view.GetSectionCount())
  {
    WARN_LOG_FMT(SYMBOLS, "{}", bad_index_message);
    return 0;
  }
  return view.GetSection(section_index).offset + offset;
}
}  // namespace

u32 RSOView::GetProlog() const
{
  return SpecialFunctionAddress(
      *this, m_header.GetPrologSectionIndex(), m_header.GetPrologSectionOffset(),
      "RSO doesn't have a prolog function", "RSO prolog section index out of bound");
}

u32 RSOView::GetEpilog() const
{
  return SpecialFunctionAddress(
      *this, m_header.GetEpilogSectionIndex(), m_header.GetEpilogSectionOffset(),
      "RSO doesn't have an epilog function", "RSO epilog section index out of bound");
}

u32 RSOView::GetUnresolved() const
{
  return SpecialFunctionAddress(
      *this, m_header.GetUnresolvedSectionIndex(), m_header.GetUnresolvedSectionOffset(),
      "RSO doesn't have a unresolved function", "RSO unresolved section index out of bound");
}

// RSOChainView ---------------------------------------------------------------------------------

bool RSOChainView::Load(const Core::CPUThreadGuard& guard, u32 address)
{
  RSOView start;
  start.LoadHeader(guard, address);
  DEBUG_LOG_FMT(SYMBOLS, "RSOChain node name: {}", start.GetName());
  m_chain.emplace_front(std::move(start));

  // Collect the whole chain (both directions, checking the back links) before loading tables.
  const RSOView& first_loaded = m_chain.front();
  if (!LoadNextChain(guard, first_loaded))
    return false;
  if (!LoadPrevChain(guard, first_loaded))
    return false;

  for (RSOView& module : m_chain)
  {
    module.LoadSections(guard);
    module.LoadExports(guard);
    module.LoadImports(guard);
    module.LoadExternals(guard);
    module.LoadInternals(guard);
  }
  return true;
}

bool RSOChainView::LoadNextChain(const Core::CPUThreadGuard& guard, const RSOView& view)
{
  u32 expected_prev = view.GetAddress();
  for (u32 address = view.GetNextEntry(); address != 0;)
  {
    RSOView module;
    module.LoadHeader(guard, address);
    if (module.GetPrevEntry() != expected_prev)
    {
      ERROR_LOG_FMT(SYMBOLS, "RSOChain has an incoherent previous link {:08x} != {:08x} in {}",
                    expected_prev, module.GetPrevEntry(), module.GetName());
      return false;
    }
    expected_prev = address;
    address = module.GetNextEntry();
    m_chain.push_back(std::move(module));
  }
  return true;
}

bool RSOChainView::LoadPrevChain(const Core::CPUThreadGuard& guard, const RSOView& view)
{
  u32 expected_next = view.GetAddress();
  for (u32 address = view.GetPrevEntry(); address != 0;)
  {
    RSOView module;
    module.LoadHeader(guard, address);
    if (module.GetNextEntry() != expected_next)
    {
      ERROR_LOG_FMT(SYMBOLS, "RSOChain has an incoherent next link {:08x} != {:08x} in {}",
                    expected_next, module.GetNextEntry(), module.GetName());
      return false;
    }
    expected_next = address;
    address = module.GetPrevEntry();
    m_chain.push_front(std::move(module));
  }
  return true;
}

void RSOChainView::Apply(const Core::CPUThreadGuard& guard, PPCSymbolDB* symbol_db) const
{
  for (const RSOView& module : m_chain)
    module.Apply(guard, symbol_db);
}

const std::list<RSOView>& RSOChainView::GetChain() const
{
  return m_chain;
}

void RSOChainView::Clear()
{
  m_chain.clear();
}
