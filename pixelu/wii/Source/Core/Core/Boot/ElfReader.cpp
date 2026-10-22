// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/Boot/ElfReader.h"

#include <cstring>
#include <string>
#include <utility>

#include "Common/CommonTypes.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/Swap.h"

#include "Core/HW/Memmap.h"
#include "Core/PowerPC/PPCSymbolDB.h"
#include "Core/System.h"

namespace
{
// The tables are big-endian in the file and are converted in place, so the
// rest of the reader can use the structures directly.
template <typename... Fields> void ToHostOrder(Fields&... fields)
{
  ((fields = Common::FromBigEndian(fields)), ...);
}

void ToHostOrder(Elf32_Ehdr& h)
{
  ToHostOrder(h.e_type, h.e_machine, h.e_version, h.e_entry, h.e_phoff, h.e_shoff, h.e_flags,
              h.e_ehsize, h.e_phentsize, h.e_phnum, h.e_shentsize, h.e_shnum, h.e_shstrndx);
}

void ToHostOrder(Elf32_Phdr& p)
{
  ToHostOrder(p.p_type, p.p_offset, p.p_vaddr, p.p_paddr, p.p_filesz, p.p_memsz, p.p_flags,
              p.p_align);
}

void ToHostOrder(Elf32_Shdr& s)
{
  ToHostOrder(s.sh_name, s.sh_type, s.sh_flags, s.sh_addr, s.sh_offset, s.sh_size, s.sh_link,
              s.sh_info, s.sh_addralign, s.sh_entsize);
}

// Same heuristic as the DOL reader: Wii code is recognised by an
// `mfspr rX, HID4`, an SPR the GameCube CPU doesn't have.
constexpr u32 MFSPR_HID4 = 0x7c13fba6;
constexpr u32 MFSPR_HID4_MASK = 0xfc1fffff;

// True when `text` (of `length` bytes) holds a NUL within its first
// `length` bytes, i.e. the string is terminated inside its table.
bool IsTerminated(const char* text, size_t length)
{
  return std::memchr(text, '\0', length) != nullptr;
}
}  // namespace

ElfReader::~ElfReader() = default;

// All three constructors only differ in how the base class obtains the bytes.
ElfReader::ElfReader(const std::string& filename) : BootExecutableReader(filename)
{
  m_is_valid = Initialize();
}

ElfReader::ElfReader(std::vector<u8> buffer) : BootExecutableReader(std::move(buffer))
{
  m_is_valid = Initialize();
}

ElfReader::ElfReader(File::IOFile file) : BootExecutableReader(std::move(file))
{
  m_is_valid = Initialize();
}

bool ElfReader::Initialize()
{
  if (m_bytes.size() < sizeof(Elf32_Ehdr))
  {
    ERROR_LOG_FMT(BOOT, "ELF file is too small.");
    return false;
  }

  u8* const file = m_bytes.data();
  base = reinterpret_cast<char*>(file);
  base32 = reinterpret_cast<u32*>(file);
  header = reinterpret_cast<Elf32_Ehdr*>(file);

  // Only 32-bit big-endian images are understood.
  static constexpr u8 expected_ident[] = {ELFMAG0, ELFMAG1,    ELFMAG2,
                                          ELFMAG3, ELFCLASS32, ELFDATA2MSB};
  if (std::memcmp(header->e_ident, expected_ident, sizeof(expected_ident)) != 0)
  {
    ERROR_LOG_FMT(BOOT, "Invalid ELF header.");
    return false;
  }

  ToHostOrder(*header);

  const size_t file_size = m_bytes.size();
  const auto fits = [file_size](size_t offset, size_t length) {
    return offset <= file_size && file_size - offset >= length;
  };

  bool tables_ok = header->e_ehsize == sizeof(Elf32_Ehdr);
  if (header->e_phnum != 0)
    tables_ok = tables_ok && header->e_phentsize == sizeof(Elf32_Phdr);
  if (header->e_shnum != 0)
    tables_ok = tables_ok && header->e_shentsize == sizeof(Elf32_Shdr);
  tables_ok = tables_ok && fits(header->e_phoff, sizeof(Elf32_Phdr) * header->e_phnum) &&
              fits(header->e_shoff, sizeof(Elf32_Shdr) * header->e_shnum);
  if (header->e_shstrndx != SHN_UNDEF)
    tables_ok = tables_ok && header->e_shstrndx < header->e_shnum;
  if (!tables_ok)
  {
    ERROR_LOG_FMT(BOOT, "Invalid ELF header table.");
    return false;
  }

  segments = reinterpret_cast<Elf32_Phdr*>(base + header->e_phoff);
  sections = reinterpret_cast<Elf32_Shdr*>(base + header->e_shoff);

  // The counts are re-read on every iteration on purpose: a table that
  // overlaps the file header changes them while it is being converted.
  for (int i = 0; i < GetNumSegments(); ++i)
  {
    Elf32_Phdr& segment = segments[i];
    ToHostOrder(segment);
    if (!fits(segment.p_offset, segment.p_filesz) || segment.p_memsz < segment.p_filesz)
    {
      ERROR_LOG_FMT(BOOT, "Invalid ELF program header {}.", i);
      return false;
    }
  }

  for (int i = 0; i < GetNumSections(); ++i)
  {
    Elf32_Shdr& section = sections[i];
    ToHostOrder(section);
    if (section.sh_type == SHT_NOBITS)
      continue;
    if (!fits(section.sh_offset, section.sh_size))
    {
      ERROR_LOG_FMT(BOOT, "Invalid ELF section header {}.", i);
      return false;
    }
  }

  entryPoint = header->e_entry;
  // Anything other than a plain executable would need relocating.
  bRelocate = header->e_type != ET_EXEC;
  return true;
}

const char* ElfReader::GetSectionName(int section) const
{
  if (!m_is_valid || section < 0 || section >= header->e_shnum)
    return nullptr;
  if (sections[section].sh_type == SHT_NULL)
    return nullptr;

  const int names_index = header->e_shstrndx;
  const char* const names = reinterpret_cast<const char*>(GetSectionDataPtr(names_index));
  if (names == nullptr)
    return nullptr;

  const size_t names_size = sections[names_index].sh_size;
  const size_t offset = sections[section].sh_name;
  if (offset >= names_size || !IsTerminated(names + offset, names_size - offset))
    return nullptr;

  return names + offset;
}

// A deliberately small loader: enough for the statically linked executables
// that devkitPPC produces, nothing more.
bool ElfReader::LoadIntoMemory(Core::System& system, bool only_in_mem1) const
{
  if (!m_is_valid)
    return false;
  Memory::MemoryManager& memory = system.GetMemory();

  INFO_LOG_FMT(BOOT, "String section: {}", header->e_shstrndx);

  if (bRelocate)
  {
    PanicAlertFmt("Error: Dolphin doesn't know how to load a relocatable elf.");
    return false;
  }

  INFO_LOG_FMT(BOOT, "{} segments:", header->e_phnum);

  for (int i = 0; i < header->e_phnum; ++i)
  {
    const Elf32_Phdr& segment = segments[i];
    INFO_LOG_FMT(BOOT, "Type: {} Vaddr: {:08x} Paddr: {:08x} Filesz: {} Memsz: {}", segment.p_type,
                 segment.p_vaddr, segment.p_paddr, segment.p_filesz, segment.p_memsz);

    if (segment.p_type != PT_LOAD)
      continue;

    // Prefer the load address, mapped into the cached segment. Some linkers
    // leave it as zero, in which case the virtual address is used as is.
    const u32 target = segment.p_paddr != 0 ? (segment.p_paddr | 0x80000000) : segment.p_vaddr;
    if (only_in_mem1 && target >= memory.GetRamSizeReal())
      continue;

    memory.CopyToEmu(target, GetSegmentPtr(i), segment.p_filesz);
    // Whatever the file doesn't provide (.bss) starts out zeroed.
    if (segment.p_memsz > segment.p_filesz)
      memory.Memset(target + segment.p_filesz, 0, segment.p_memsz - segment.p_filesz);

    INFO_LOG_FMT(BOOT, "Loadable Segment Copied to {:08x}, size {:08x}", target, segment.p_memsz);
  }

  INFO_LOG_FMT(BOOT, "Done loading.");
  return true;
}

SectionID ElfReader::GetSectionByName(const char* name, int firstSection) const
{
  for (int index = firstSection; index < header->e_shnum; ++index)
  {
    const char* const candidate = GetSectionName(index);
    if (candidate != nullptr && std::strcmp(name, candidate) == 0)
      return index;
  }
  return -1;
}

bool ElfReader::LoadSymbols(const Core::CPUThreadGuard& guard, PPCSymbolDB& ppc_symbol_db,
                            const std::string& filename) const
{
  if (!m_is_valid)
    return false;

  bool added_any = false;
  const SectionID table = GetSectionByName(".symtab");
  if (table != -1)
  {
    const u32 names_index = sections[table].sh_link;
    if (names_index >= header->e_shnum)
    {
      ERROR_LOG_FMT(BOOT, "Invalid ELF symbol string table.");
      return false;
    }

    const char* const names =
        reinterpret_cast<const char*>(GetSectionDataPtr(static_cast<int>(names_index)));
    if (names == nullptr)
    {
      ERROR_LOG_FMT(BOOT, "ELF symbol string table has no data.");
      return false;
    }

    const u8* const entries = GetSectionDataPtr(table);
    if (entries == nullptr)
    {
      ERROR_LOG_FMT(BOOT, "ELF symbol table has no data.");
      return false;
    }

    const size_t names_size = sections[names_index].sh_size;
    const int count = static_cast<int>(sections[table].sh_size / sizeof(Elf32_Sym));
    for (int index = 0; index < count; ++index)
    {
      Elf32_Sym entry;
      std::memcpy(&entry, entries + static_cast<size_t>(index) * sizeof(Elf32_Sym), sizeof(entry));

      // Symbol table entries are left in file byte order.
      const u32 size = Common::swap32(entry.st_size);
      if (size == 0)
        continue;

      const size_t name_offset = Common::swap32(entry.st_name);
      if (name_offset >= names_size || !IsTerminated(names + name_offset, names_size - name_offset))
      {
        ERROR_LOG_FMT(BOOT, "Invalid ELF symbol name {}.", index);
        return false;
      }

      u32 address = Common::swap32(entry.st_value);
      if (bRelocate)
        address += sectionAddrs[Common::swap16(entry.st_shndx)];

      Common::Symbol::Type kind;
      switch (ELF32_ST_TYPE(entry.st_info))
      {
      case STT_FUNC:
        kind = Common::Symbol::Type::Function;
        break;
      case STT_OBJECT:
        kind = Common::Symbol::Type::Data;
        break;
      default:
        // Sections, files and untyped symbols aren't worth tracking.
        continue;
      }

      ppc_symbol_db.AddKnownSymbol(guard, address, size, names + name_offset, filename, kind);
      added_any = true;
    }
  }

  ppc_symbol_db.Index();
  return added_any;
}

bool ElfReader::IsWii() const
{
  if (!m_is_valid)
    return false;

  // A rough heuristic, prone to the odd false result either way.
  for (int i = 0; i < GetNumSegments(); ++i)
  {
    if (!IsCodeSegment(i))
      continue;

    const u8* const code = GetSegmentPtr(i);
    const size_t words = static_cast<size_t>(GetSegmentSize(i)) / sizeof(u32);
    for (size_t w = 0; w < words; ++w)
    {
      if ((Common::swap32(code + w * sizeof(u32)) & MFSPR_HID4_MASK) == MFSPR_HID4)
        return true;
    }
  }
  return false;
}
