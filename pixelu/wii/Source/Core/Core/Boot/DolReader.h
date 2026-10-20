// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <optional>
#include <span>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/Boot/Boot.h"

namespace File
{
class IOFile;
}

// Reader for the DOL executable format: a fixed header of up to 7 text and
// 11 data sections, plus bss and the entry point.
class DolReader final : public BootExecutableReader
{
public:
  explicit DolReader(const std::string& filename);
  explicit DolReader(File::IOFile file);
  explicit DolReader(std::vector<u8> buffer);
  ~DolReader() override;

  bool IsValid() const override { return m_is_valid; }
  bool IsWii() const override { return m_is_wii; }
  bool IsAncast() const { return m_ancast_index.has_value(); }
  u32 GetEntryPoint() const override { return m_dolheader.m_entrypoint; }
  bool LoadIntoMemory(Core::System& system, bool only_in_mem1 = false) const override;
  // DOL files carry no symbols.
  bool LoadSymbols(const Core::CPUThreadGuard&, PPCSymbolDB&, const std::string&) const override
  {
    return false;
  }

private:
  enum
  {
    DOL_NUM_TEXT = 7,
    DOL_NUM_DATA = 11
  };

  struct DolHeader
  {
    u32 m_text_offset[DOL_NUM_TEXT];
    u32 m_data_offset[DOL_NUM_DATA];

    u32 m_text_address[DOL_NUM_TEXT];
    u32 m_data_address[DOL_NUM_DATA];

    u32 m_text_size[DOL_NUM_TEXT];
    u32 m_data_size[DOL_NUM_DATA];

    u32 m_bss_address;
    u32 m_bss_size;
    u32 m_entrypoint;
  };
  DolHeader m_dolheader;

  struct LoadableSection
  {
    u32 m_address;
    u32 m_header_section_size;
    std::span<const u8> m_data;
  };

  std::vector<LoadableSection> m_sections;

  bool m_is_valid = false;
  bool m_is_wii = false;
  std::optional<std::size_t> m_ancast_index;

  // Parses the header and records where each section's bytes are.
  bool Initialize(std::span<const u8> buffer);

  bool LoadAncastIntoMemory(Core::System& system) const;
};
