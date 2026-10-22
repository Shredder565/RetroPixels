// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/HW/EXI/EXI_Device.h"

class PointerWrap;

namespace ExpansionInterface
{
enum class Slot : int;

// The Triforce's GBA cartridge reader in a memory card slot. A GBA ROM and its
// EEPROM save are served through a small command protocol in immediate EXI
// transfers; every answer carries a running CRC-8.
class CEXIAgp : public IEXIDevice
{
public:
  CEXIAgp(Core::System& system, const Slot slot);
  ~CEXIAgp() override;
  bool IsPresent() const override { return true; }
  void ImmWrite(u32 _uData, u32 _uSize) override;
  u32 ImmRead(u32 _uSize) override;
  void DoState(PointerWrap& p) override;

private:
  void LoadRom(const std::string& path);
  void LoadSave(const std::string& path);
  void WriteSave(const std::string& path) const;

  void Hash(u8 byte);
  u8 RomByte();
  void WriteEEPROMBit(u32 data);
  bool ReadEEPROMWord(u32 index, u64* word) const;

  Slot m_slot;
  std::string m_base_path;

  u32 m_address = 0;
  u32 m_current_cmd = 0;
  std::vector<u8> m_eeprom;
  u32 m_eeprom_cmd = 0;
  u64 m_eeprom_data = 0;
  u32 m_eeprom_mask = 0;
  u16 m_eeprom_pos = 0;
  u32 m_eeprom_size = 0;
  u16 m_eeprom_add_end = 0;
  u16 m_eeprom_add_mask = 0;
  u16 m_eeprom_read_mask = 0;
  u32 m_eeprom_status_mask = 0;
  bool m_eeprom_write_status = false;
  u8 m_hash = 0;
  u32 m_position = 0;
  u32 m_return_pos = 0;
  std::vector<u8> m_rom;
  u32 m_rom_mask = 0;
  u32 m_rom_size = 0;
  u32 m_rw_offset = 0;
};
}  // namespace ExpansionInterface
