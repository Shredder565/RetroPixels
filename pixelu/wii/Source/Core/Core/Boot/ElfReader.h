// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Common/CommonTypes.h"
#include "Core/Boot/Boot.h"
#include "Core/Boot/ElfTypes.h"

namespace File
{
class IOFile;
}

enum KnownElfTypes
{
  KNOWNELF_PSP = 0,
  KNOWNELF_DS = 1,
  KNOWNELF_GBA = 2,
  KNOWNELF_GC = 3,
};

typedef int SectionID;

// Reader for 32-bit big-endian ELF executables. The header and tables are
// converted to host byte order in the reader's own copy of the file.
class ElfReader final : public BootExecutableReader
{
public:
  explicit ElfReader(const std::string& filename);
  explicit ElfReader(File::IOFile file);
  explicit ElfReader(std::vector<u8> buffer);
  ~ElfReader() override;
  u32 Read32(int off) const { return base32[off >> 2]; }
  ElfType GetType() const { return (ElfType)(header->e_type); }
  ElfMachine GetMachine() const { return (ElfMachine)(header->e_machine); }
  u32 GetEntryPoint() const override { return entryPoint; }
  u32 GetFlags() const { return (u32)(header->e_flags); }
  bool LoadIntoMemory(Core::System& system, bool only_in_mem1 = false) const override;
  bool LoadSymbols(const Core::CPUThreadGuard& guard, PPCSymbolDB& ppc_symbol_db,
                   const std::string& filename) const override;
  bool IsValid() const override { return m_is_valid; }
  bool IsWii() const override;

  int GetNumSegments() const { return (int)(header->e_phnum); }
  int GetNumSections() const { return (int)(header->e_shnum); }
  const u8* GetPtr(int offset) const { return (u8*)base + offset; }
  const char* GetSectionName(int section) const;
  // Null for out-of-range indices and for sections without file data (.bss).
  const u8* GetSectionDataPtr(int section) const
  {
    const bool in_range = section >= 0 && section < header->e_shnum;
    if (!in_range || sections[section].sh_type == SHT_NOBITS)
      return nullptr;
    return GetPtr(static_cast<int>(sections[section].sh_offset));
  }
  bool IsCodeSegment(int segment) const { return (segments[segment].p_flags & PF_X) != 0; }
  const u8* GetSegmentPtr(int segment) const
  {
    return GetPtr(static_cast<int>(segments[segment].p_offset));
  }
  int GetSegmentSize(int segment) const { return static_cast<int>(segments[segment].p_filesz); }
  u32 GetSectionAddr(SectionID section) const { return sectionAddrs[section]; }
  int GetSectionSize(SectionID section) const
  {
    return static_cast<int>(sections[section].sh_size);
  }
  // Returns -1 when no section of that name exists.
  SectionID GetSectionByName(const char* name, int firstSection = 0) const;

  bool DidRelocate() const { return bRelocate; }

private:
  bool Initialize();

  char* base = nullptr;
  u32* base32 = nullptr;

  Elf32_Ehdr* header = nullptr;
  Elf32_Phdr* segments = nullptr;
  Elf32_Shdr* sections = nullptr;

  u32* sectionAddrs = nullptr;
  bool bRelocate = false;
  u32 entryPoint = 0;
  bool m_is_valid = false;
};
