// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <string>

#include "Common/CommonTypes.h"
#include "Common/Swap.h"

namespace ExpansionInterface
{
enum class Slot : int;
}

// The 12-byte flash ID a memory card is formatted with.
using CardFlashId = std::array<u8, 12>;

// The battery-backed SRAM behind the IPL device (0x44 bytes). The layout is
// fixed by the hardware and the IPL; SRAM files on disk hold these bytes as
// they are.
#pragma pack(push, 1)

// Byte 0x17: boot and output preferences.
struct SramFlags
{
  enum : u8
  {
    kStereo = 0x04,            // set: stereo output, clear: mono
    kOobeDone = 0x08,          // clear: the IPL runs its first-time setup
    kBootToMenu = 0x40,        // show the IPL menu even with a disc inserted
    kProgressiveScan = 0x80,   // offer progressive scan to games that support it
  };

  bool stereo() const { return Has(kStereo); }
  bool oobe_done() const { return Has(kOobeDone); }
  bool boot_to_menu() const { return Has(kBootToMenu); }
  bool progressive_scan() const { return Has(kProgressiveScan); }

  void stereo(bool enable) { set_flag(enable, kStereo); }
  void oobe_done(bool enable) { set_flag(enable, kOobeDone); }
  void boot_to_menu(bool enable) { set_flag(enable, kBootToMenu); }
  void progressive_scan(bool enable) { set_flag(enable, kProgressiveScan); }

  void set_flag(bool enable, u8 flag) { value = enable ? (value | flag) : (value & ~flag); }

  u8 value;

private:
  bool Has(u8 flag) const { return (value & flag) != 0; }
};

// Bytes 0x04-0x17. The IPL checks the two checksums, which cover the bytes
// from rtc_bias up to and including flags (see FixSRAMChecksums).
struct SramSettings
{
  Common::BigEndianValue<u16> checksum;
  Common::BigEndianValue<u16> checksum_inv;
  u32 ead0;  // meaning unknown
  u32 ead1;  // meaning unknown
  u32 rtc_bias;
  s8 vi_horizontal_offset;  // in pixels
  u8 ntd;                   // meaning unknown
  u8 language;
  SramFlags flags;
};

// Bytes 0x18-0x43.
struct SramSettingsEx
{
  // Per memory card slot (A, B): the ID that unlocks the card.
  CardFlashId flash_id[2];
  // Last wireless keyboard and controllers seen.
  u32 wireless_kbd_id;
  u16 wireless_pad_id[4];
  // Last fatal drive error.
  u8 di_error_code;
  u8 field_25;
  // Per slot: the byte sum of flash_id, inverted.
  u8 flash_id_checksum[2];
  u16 gbs_mode;
  u8 field_3e[2];
};

struct Sram
{
  Common::BigEndianValue<u32> rtc{};
  SramSettings settings{};
  SramSettingsEx settings_ex{};

  // Byte access to the whole block, as the EXI device reads and writes it.
  u8& operator[](size_t offset) { return reinterpret_cast<u8*>(this)[offset]; }
};
static_assert(sizeof(Sram) == 0x44);

#pragma pack(pop)

// Loads the SRAM file, or default contents when it is missing or short.
void InitSRAM(Sram* sram, const std::string& filename);
// Derives the flash ID of the card in `card_slot` from the first 20 bytes of
// its header, and stores it with its checksum.
void SetCardFlashID(Sram* sram, const u8* buffer, ExpansionInterface::Slot card_slot);
// Recomputes settings.checksum and settings.checksum_inv.
void FixSRAMChecksums(Sram* sram);
