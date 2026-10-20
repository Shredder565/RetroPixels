// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>

#include "Common/CommonTypes.h"
#include "Common/Crypto/SHA1.h"
#include "DiscIO/Volume.h"

namespace DiscIO
{
// What GameCube and Wii discs have in common: the disc header at the start of the (game)
// partition.
class VolumeDisc : public Volume
{
public:
  // The boot.id file of Triforce arcade discs, which takes the place of the disc header for
  // identification. It is read as raw bytes: multi-byte fields are not byte-swapped. The
  // offsets are in the comments.
  struct BootID
  {
    u32 magic;  // 0x00: "BTID"
    std::array<uint32_t, 3> unknown0;
    std::array<uint32_t, 4> unknown1;
    std::array<char, 4> mediaboard_type;  // 0x20: "GCAM" on Triforce
    uint32_t unknown2;
    uint16_t year;       // 0x28
    uint8_t month;       // 0x2a
    uint8_t day;         // 0x2b
    uint8_t video_mode;  // 0x2c: supported resolutions and orientations (bit mask)
    uint8_t unknown3;
    uint8_t type_3_compatible;  // 0x2e: 1 for titles that run on Type-3 boards
    uint8_t unknown4;
    std::array<char, 8> game_id;  // 0x30
    uint32_t region_flags;        // 0x38
    std::array<uint32_t, 9> unknown6;
    std::array<char, 0x20> manufacturer;                 // 0x60
    std::array<char, 0x20> game_name;                    // 0x80
    std::array<char, 0x20> game_executable;              // 0xa0
    std::array<char, 0x20> test_executable;              // 0xc0
    std::array<std::array<char, 0x20>, 8> credit_types;  // 0xe0
  };

  std::string GetGameID(const Partition& partition = PARTITION_NONE) const override;
  Country GetCountry(const Partition& partition = PARTITION_NONE) const override;
  std::string GetMakerID(const Partition& partition = PARTITION_NONE) const override;
  std::optional<u16> GetRevision(const Partition& partition = PARTITION_NONE) const override;
  std::string GetInternalName(const Partition& partition = PARTITION_NONE) const override;
  std::string GetApploaderDate(const Partition& partition) const override;
  std::optional<u8> GetDiscNumber(const Partition& partition = PARTITION_NONE) const override;
  u32 GetSimulatedMemorySize() const override;
  bool IsNKit() const override;

protected:
  // Maps the region field of a disc header (or of the Wii region data) to a Region.
  Region RegionCodeToRegion(std::optional<u32> region_code) const;
  // Hashes what the game partition boots from: headers, apploader, DOL, FST and banner.
  void AddGamePartitionToSyncHash(Common::SHA1::Context* context) const;
};

static_assert(sizeof(VolumeDisc::BootID) == 480);

}  // namespace DiscIO
