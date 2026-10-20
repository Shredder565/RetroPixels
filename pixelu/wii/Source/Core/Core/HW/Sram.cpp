// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/Sram.h"

#include <cstddef>
#include <cstring>

#include "Common/CommonTypes.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/Swap.h"

#include "Core/HW/EXI/EXI.h"

namespace
{
// Contents used when no SRAM file can be read: English, stereo, setup done.
// The IPL device fills in the clock, language and checksums at start-up.
Sram DefaultSram()
{
  static constexpr char SLOT_IDS[2][13] = {"DOLPHINSLOTA", "DOLPHINSLOTB"};

  Sram sram{};
  sram.settings.checksum = 0x2c;
  sram.settings.checksum_inv = 0xffd0;
  sram.settings.flags.value = 0x20 | SramFlags::kOobeDone | SramFlags::kStereo;
  for (int slot = 0; slot < 2; ++slot)
    std::memcpy(sram.settings_ex.flash_id[slot].data(), SLOT_IDS[slot], 12);
  sram.settings_ex.flash_id_checksum[0] = 0x6E;
  sram.settings_ex.flash_id_checksum[1] = 0x6D;
  return sram;
}

// One step of the generator the IPL uses to scramble flash IDs.
u64 ScrambleStep(u64 x)
{
  return (x * 0x41C64E6DULL + 0x3039ULL) >> 16;
}
}  // namespace

void InitSRAM(Sram* sram, const std::string& filename)
{
  File::IOFile file(filename, "rb");
  if (!file)
  {
    *sram = DefaultSram();
    return;
  }
  if (!file.ReadArray(sram, 1))
  {
    ERROR_LOG_FMT(EXPANSIONINTERFACE, "EXI IPL-DEV: Could not read all of SRAM");
    *sram = DefaultSram();
  }
}

void SetCardFlashID(Sram* sram, const u8* buffer, ExpansionInterface::Slot card_slot)
{
  int index;
  if (card_slot == ExpansionInterface::Slot::A)
    index = 0;
  else if (card_slot == ExpansionInterface::Slot::B)
    index = 1;
  else
  {
    PanicAlertFmt("Invalid memcard slot {}", card_slot);
    return;
  }

  // Bytes 0-11 of the card header are the flash ID scrambled with a stream
  // seeded from bytes 12-19 (the format time).
  CardFlashId& id = sram->settings_ex.flash_id[index];
  u64 state = Common::swap64(buffer + 12);
  u8 sum = 0;
  for (size_t i = 0; i < id.size(); ++i)
  {
    state = ScrambleStep(state);
    id[i] = static_cast<u8>(buffer[i] - static_cast<u8>(state));
    sum += id[i];
    state = ScrambleStep(state) & 0x7FFF;
  }
  sram->settings_ex.flash_id_checksum[index] = static_cast<u8>(~sum);
}

void FixSRAMChecksums(Sram* sram)
{
  // Big-endian 16-bit words from rtc_bias up to the extended settings.
  constexpr size_t first = offsetof(Sram, settings) + offsetof(SramSettings, rtc_bias);
  constexpr size_t end = offsetof(Sram, settings_ex);

  u16 sum = 0;
  u16 inverted_sum = 0;
  for (size_t offset = first; offset < end; offset += 2)
  {
    const u16 word = static_cast<u16>(((*sram)[offset] << 8) | (*sram)[offset + 1]);
    sum += word;
    inverted_sum += static_cast<u16>(~word);
  }
  sram->settings.checksum = sum;
  sram->settings.checksum_inv = inverted_sum;
}
