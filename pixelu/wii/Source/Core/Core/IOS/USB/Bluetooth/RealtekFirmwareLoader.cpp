// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.
//
// Many Realtek USB Bluetooth adapters start with a ROM firmware that needs a patch uploaded
// before they work. The patches are Realtek's own redistributable files from the
// linux-firmware repository (rtl_bt/rtlXXXX_fw.bin and the optional rtlXXXX_config.bin). The
// "epatch" container and the Realtek vendor commands used here are the ones documented by the
// public Linux btrtl driver.

#include "Core/IOS/USB/Bluetooth/RealtekFirmwareLoader.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Common/Buffer.h"
#include "Common/FileUtil.h"
#include "Common/HttpRequest.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"

#include "Core/IOS/USB/Bluetooth/LibUSBBluetoothAdapter.h"
#include "Core/IOS/USB/Bluetooth/hci.h"

namespace
{
// LMP subversions of the ROM firmware of each chip family.
constexpr u16 LMP_8723A = 0x1200;
constexpr u16 LMP_8723B = 0x8723;
constexpr u16 LMP_8821A = 0x8821;
constexpr u16 LMP_8761A = 0x8761;
constexpr u16 LMP_8822B = 0x8822;
constexpr u16 LMP_8851B = 0x8851;
constexpr u16 LMP_8852A = 0x8852;
constexpr u16 LMP_8922A = 0x8922;

// Realtek vendor commands.
constexpr u16 OPCODE_DOWNLOAD = 0xfc20;
constexpr u16 OPCODE_READ_ROM_VERSION = 0xfc6d;

// A download command carries a sequence byte and up to this many bytes of the image.
constexpr std::size_t DOWNLOAD_CHUNK = 252;

// The epatch container: an 8-byte signature, the firmware version (u32) and the number of
// patches (u16), then per patch a chip ID (u16), a length (u16) and an offset (u32), each as
// its own array. The file ends with an "extension" section, read backwards.
constexpr std::string_view EPATCH_MAGIC = "Realtech";
constexpr std::string_view EPATCH_MAGIC_V2 = "RTBTCore";
constexpr std::size_t EPATCH_HEADER_SIZE = 14;
constexpr std::array<u8, 4> EXTENSION_MAGIC{0x51, 0x04, 0xfd, 0x77};

struct VersionInfo
{
  u8 status;
  u8 hci_version;
  u16 hci_revision;
  u16 lmp_subversion;
};

u16 ReadLE16(std::span<const u8> bytes, std::size_t offset)
{
  return static_cast<u16>(bytes[offset] | (bytes[offset + 1] << 8));
}

u32 ReadLE32(std::span<const u8> bytes, std::size_t offset)
{
  return static_cast<u32>(ReadLE16(bytes, offset)) |
         (static_cast<u32>(ReadLE16(bytes, offset + 2)) << 16);
}

std::span<const u8> AsBytes(std::string_view text)
{
  return {reinterpret_cast<const u8*>(text.data()), text.size()};
}

// Adapters that use the Realtek driver without Realtek's own vendor ID, plus Realtek-ID
// adapters whose firmware needs loading. Grouped by vendor ID.
struct KnownVendor
{
  u16 vid;
  std::vector<u16> pids;
};

const std::vector<KnownVendor>& KnownRealtekAdapters()
{
  static const std::vector<KnownVendor> s_adapters = {
      {0x0489, {0xe085, 0xe08b, 0xe112, 0xe122, 0xe123, 0xe125, 0xe12f, 0xe130}},
      {0x04bb, {0x0a15}},
      {0x04c5, {0x161f, 0x165c, 0x1675}},
      {0x04ca, {0x4005, 0x4006, 0x4007}},
      {0x04f2, {0xb49f}},
      {0x0930, {0x021d}},
      {0x0b05, {0x17dc, 0x185c, 0x18ef, 0x190e}},
      {0x0bda, {0x2550, 0x2852, 0x385a, 0x4852, 0x4853, 0x8520, 0x876e, 0x8771, 0x877b,
                0x877c, 0x887b, 0x8922, 0xa725, 0xa729, 0xb009, 0xb00c, 0xb850, 0xb85b,
                0xc123, 0xc822, 0xc852, 0xd922, 0xd923}},
      {0x0cb5, {0xc547}},
      {0x0cb8, {0xc549, 0xc558, 0xc559}},
      {0x1358, {0xc123}},
      {0x13d3, {0x3394, 0x3410, 0x3414, 0x3416, 0x3458, 0x3459, 0x3461, 0x3462, 0x3494,
                0x3526, 0x3529, 0x3533, 0x3548, 0x3549, 0x3553, 0x3555, 0x3570, 0x3571,
                0x3572, 0x3586, 0x3587, 0x3591, 0x3592, 0x3600, 0x3601, 0x3612, 0x3616,
                0x3617, 0x3618, 0x3619}},
      {0x2001, {0x332a}},
      {0x2357, {0x0604, 0x0607}},
      {0x2550, {0x8761}},
      {0x2b89, {0x3cfa, 0x6275, 0x8761}},
      {0x2c0a, {0x8761}},
      {0x2c4e, {0x0115, 0x0128}},
      {0x2ff8, {0x3051, 0xb011}},
      {0x3188, {0x8771}},
      {0x33f2, {0x1688}},
      {0x3625, {0x010b}},
      {0x37ad, {0x0600}},
      {0x6655, {0x8771}},
      {0x7392, {0xa611, 0xc611, 0xe611}},
  };
  return s_adapters;
}

// Each patch in a file names the project it was built for, which must match the chip family.
struct ProjectFamily
{
  u8 project_id;
  u16 lmp_subversion;
};
constexpr ProjectFamily PROJECT_FAMILIES[] = {
    {1, LMP_8723B},  {2, LMP_8821A},  {3, LMP_8761A},  {8, LMP_8822B},  {9, LMP_8723B},
    {10, LMP_8821A}, {13, LMP_8822B}, {14, LMP_8761A}, {18, LMP_8852A}, {20, LMP_8852A},
    {25, LMP_8852A}, {36, LMP_8851B}, {44, LMP_8922A}, {47, LMP_8852A},
};

// The firmware file name for a chip, by LMP subversion, HCI revision and HCI version.
struct FirmwareName
{
  u16 lmp_subversion;
  u16 hci_revision;
  u8 hci_version;
  std::string_view name;
};
constexpr FirmwareName FIRMWARE_NAMES[] = {
    {LMP_8723A, 0x000b, 0x6, "8723a"},   {LMP_8723B, 0x000b, 0x6, "8723b"},
    {LMP_8723B, 0x000d, 0x8, "8723d"},   {LMP_8761A, 0x000a, 0x6, "8761a"},
    {LMP_8761A, 0x000b, 0xa, "8761bu"},  {LMP_8821A, 0x000a, 0x6, "8821a"},
    {LMP_8821A, 0x000c, 0x8, "8821c"},   {LMP_8822B, 0x000c, 0xa, "8822cu"},
    {LMP_8822B, 0x000b, 0x7, "8822b"},   {LMP_8851B, 0x000b, 0xc, "8851bu"},
    {LMP_8852A, 0x000a, 0xb, "8852au"},  {LMP_8852A, 0x000b, 0xb, "8852bu"},
    {LMP_8852A, 0x000c, 0xc, "8852cu"},  {LMP_8852A, 0x0087, 0xc, "8852btu"},
    {LMP_8922A, 0x000a, 0xc, "8922au"},
};

std::string_view FindFirmwareName(const VersionInfo& version)
{
  for (const FirmwareName& entry : FIRMWARE_NAMES)
  {
    if (entry.lmp_subversion == version.lmp_subversion &&
        entry.hci_revision == version.hci_revision && entry.hci_version == version.hci_version)
    {
      return entry.name;
    }
  }
  return {};
}

// Walks the extension section backwards from its signature. Each instruction is stored as
// data, length, opcode (reading backwards); opcode 0 with one byte of data is the project ID
// and opcode 0xff ends the list. The walk steps over one byte more than the data length,
// exactly as Realtek's own loaders do.
int FindProjectID(std::span<const u8> file)
{
  std::ptrdiff_t pos = static_cast<std::ptrdiff_t>(file.size() - EXTENSION_MAGIC.size());
  constexpr std::ptrdiff_t lowest = static_cast<std::ptrdiff_t>(EPATCH_HEADER_SIZE + 3);
  while (pos >= lowest)
  {
    const u8 opcode = file[static_cast<std::size_t>(pos - 1)];
    const u8 length = file[static_cast<std::size_t>(pos - 2)];
    const u8 value = file[static_cast<std::size_t>(pos - 3)];
    pos -= 3;

    if (opcode == 0xff)
      return -1;
    if (length == 0)
    {
      ERROR_LOG_FMT(IOS_WIIMOTE, "Realtek firmware: zero-length extension entry");
      return -2;
    }
    if (opcode == 0x00 && length == 1)
      return value;
    pos -= length;
  }
  return -1;
}

// Builds the image to upload: the patch for this ROM version, whose last four bytes are
// replaced by the file's firmware version, followed by the config file. Empty on error.
Common::UniqueBuffer<u8> BuildImage(std::span<const u8> file, std::span<const u8> config,
                                    const VersionInfo& version, u8 rom_version)
{
  if (file.size() < EPATCH_HEADER_SIZE)
  {
    ERROR_LOG_FMT(IOS_WIIMOTE, "Realtek firmware file is too small");
    return {};
  }

  const auto magic = file.first(EPATCH_MAGIC.size());
  if (!std::ranges::equal(magic, AsBytes(EPATCH_MAGIC)))
  {
    if (std::ranges::equal(magic, AsBytes(EPATCH_MAGIC_V2)))
      ERROR_LOG_FMT(IOS_WIIMOTE, "Realtek EPATCH v2 files are not supported yet");
    else
      ERROR_LOG_FMT(IOS_WIIMOTE, "Realtek firmware file has no EPATCH signature");
    return {};
  }

  const u32 fw_version = ReadLE32(file, 8);
  const u16 patch_count = ReadLE16(file, 12);
  INFO_LOG_FMT(IOS_WIIMOTE, "Realtek firmware version {}, {} patches", fw_version, patch_count);

  if (!std::ranges::equal(file.last(EXTENSION_MAGIC.size()), EXTENSION_MAGIC))
  {
    ERROR_LOG_FMT(IOS_WIIMOTE, "Realtek firmware file has no extension section");
    return {};
  }

  const int project_id = FindProjectID(file);
  if (project_id == -2)
    return {};
  const auto family = std::ranges::find(PROJECT_FAMILIES, project_id, [](const ProjectFamily& f) {
    return static_cast<int>(f.project_id);
  });
  if (family == std::end(PROJECT_FAMILIES) || family->lmp_subversion != version.lmp_subversion)
  {
    ERROR_LOG_FMT(IOS_WIIMOTE, "Realtek firmware is for another chip (project {})", project_id);
    return {};
  }

  const std::size_t chip_ids = EPATCH_HEADER_SIZE;
  const std::size_t lengths = chip_ids + 2 * std::size_t{patch_count};
  const std::size_t offsets = lengths + 2 * std::size_t{patch_count};
  if (file.size() < offsets + 4 * std::size_t{patch_count})
  {
    ERROR_LOG_FMT(IOS_WIIMOTE, "Realtek firmware patch table is truncated");
    return {};
  }

  for (std::size_t i = 0; i < patch_count; ++i)
  {
    if (ReadLE16(file, chip_ids + 2 * i) != rom_version + 1)
      continue;

    const u16 length = ReadLE16(file, lengths + 2 * i);
    const u32 offset = ReadLE32(file, offsets + 4 * i);
    INFO_LOG_FMT(IOS_WIIMOTE, "Realtek patch: {} bytes at {}", length, offset);

    if (u64{offset} + length > file.size() || length < sizeof(fw_version))
    {
      ERROR_LOG_FMT(IOS_WIIMOTE, "Realtek firmware patch does not fit the file");
      return {};
    }

    Common::UniqueBuffer<u8> image{length + config.size()};
    const std::size_t code_size = length - sizeof(fw_version);
    std::copy_n(file.begin() + static_cast<std::ptrdiff_t>(offset), code_size, image.data());
    for (std::size_t b = 0; b < sizeof(fw_version); ++b)
      image[code_size + b] = static_cast<u8>(fw_version >> (8 * b));
    std::ranges::copy(config, image.data() + length);
    return image;
  }

  ERROR_LOG_FMT(IOS_WIIMOTE, "Realtek firmware has no patch for ROM version {}", rom_version);
  return {};
}

// Uploads an image in DOWNLOAD_CHUNK pieces. The sequence byte counts 0..0x7f and then wraps
// to 1; the last piece has bit 7 set.
bool UploadImage(LibUSBBluetoothAdapter& adapter, std::span<const u8> image)
{
  if (image.empty())
  {
    ERROR_LOG_FMT(IOS_WIIMOTE, "No Realtek firmware to upload");
    return false;
  }
  INFO_LOG_FMT(IOS_WIIMOTE, "Uploading {} bytes of Realtek firmware", image.size());

  // Command header (opcode, length), sequence byte, data.
  std::array<u8, 4 + DOWNLOAD_CHUNK> command{};
  command[0] = static_cast<u8>(OPCODE_DOWNLOAD);
  command[1] = static_cast<u8>(OPCODE_DOWNLOAD >> 8);
  u8 sequence = 0;

  std::size_t done = 0;
  while (true)
  {
    const std::size_t chunk = std::min(image.size() - done, DOWNLOAD_CHUNK);
    const bool is_last = done + chunk == image.size();

    command[2] = static_cast<u8>(1 + chunk);
    command[3] = is_last ? static_cast<u8>(sequence | 0x80) : sequence;
    std::copy_n(image.begin() + static_cast<std::ptrdiff_t>(done), chunk, command.begin() + 4);

    // Return parameters: status, sequence byte.
    std::array<u8, 2> reply{};
    if (!adapter.SendBlockingCommand(std::span{command}.first(4 + chunk), reply))
      return false;
    if (reply[0] != 0x00)
    {
      ERROR_LOG_FMT(IOS_WIIMOTE, "Realtek firmware upload failed with status {:#04x}", reply[0]);
      return false;
    }

    if (is_last)
      break;
    done += chunk;
    sequence = sequence == 0x7f ? 1 : static_cast<u8>(sequence + 1);
  }

  NOTICE_LOG_FMT(IOS_WIIMOTE, "Realtek firmware uploaded");
  return true;
}

std::string FirmwareDirectory()
{
  return File::GetUserPath(D_FIRMWARE_IDX) + "rtl_bt/";
}

bool SaveDownload(Common::HttpRequest& request, const std::string& file_name, bool is_optional)
{
  // linux-firmware's mirror on gitlab.com (git.kernel.org hosts the same files).
  const std::string url =
      "https://gitlab.com/kernel-firmware/linux-firmware/-/raw/main/rtl_bt/" + file_name;
  const auto response = request.Get(url);
  if (!response)
  {
    if (is_optional && request.GetLastResponseCode() == 404)
    {
      // Plenty of chips have no config file, but the common 8761B does, so it is still
      // mentioned.
      PanicAlertFmtT("File {0} was not found on the server.\n\nThis might be normal.\n\n"
                     "Not all devices expect this file.",
                     file_name);
    }
    else
    {
      PanicAlertFmtT("HTTP GET {0}\n\nError: {1}", url, request.GetLastResponseCode());
    }
    return false;
  }

  File::WriteStringToFile(
      FirmwareDirectory() + file_name,
      std::string_view{reinterpret_cast<const char*>(response->data()), response->size()});
  return true;
}

void DownloadFirmware(const std::string& fw_file, const std::string& config_file)
{
  Common::HttpRequest request;
  request.FollowRedirects(1);
  File::CreateFullPath(FirmwareDirectory());
  if (SaveDownload(request, fw_file, false))
    SaveDownload(request, config_file, true);
}
}  // namespace

bool IsRealtekVID(u16 vid)
{
  return vid == 0x0bda;
}

bool IsKnownRealtekBluetoothDevice(u16 vid, u16 pid)
{
  for (const KnownVendor& vendor : KnownRealtekAdapters())
  {
    if (vendor.vid == vid)
      return std::ranges::find(vendor.pids, pid) != vendor.pids.end();
  }
  return false;
}

bool InitializeRealtekBluetoothDevice(LibUSBBluetoothAdapter& adapter)
{
  // Read Local Version Information.
  constexpr u16 read_version = BluetoothHCI::Opcode::READ_LOCAL_VERSION;
  const std::array<u8, 3> version_command{static_cast<u8>(read_version),
                                          static_cast<u8>(read_version >> 8), 0};
  BluetoothHCI::ReadLocalVersionReply version_reply{};
  if (!adapter.SendBlockingCommand(
          version_command,
          std::span{reinterpret_cast<u8*>(&version_reply), sizeof(version_reply)}))
  {
    return false;
  }
  if (version_reply.status != 0x00)
  {
    ERROR_LOG_FMT(IOS_WIIMOTE, "Realtek: Read Local Version failed with status {:#04x}",
                  version_reply.status);
    return false;
  }
  const VersionInfo version{version_reply.status, version_reply.hci_version,
                            version_reply.hci_revision, version_reply.lmp_subversion};

  // A chip that already runs a patch reports another subversion and is left alone.
  const std::string_view chip = FindFirmwareName(version);
  if (chip.empty())
  {
    INFO_LOG_FMT(IOS_WIIMOTE, "Realtek adapter needs no firmware");
    return true;
  }

  const std::string base_name = "rtl" + std::string{chip};
  const std::string fw_file = base_name + "_fw.bin";
  const std::string config_file = base_name + "_config.bin";
  const std::string fw_path = FirmwareDirectory() + fw_file;
  const std::string config_path = FirmwareDirectory() + config_file;
  NOTICE_LOG_FMT(IOS_WIIMOTE, "Realtek adapter needs {}", base_name);

  if (!File::Exists(fw_path) &&
      AskYesNoFmtT("Bluetooth passthrough requires missing firmware: {0}\n\n"
                   "Automatically download from gitlab.com now?",
                   base_name))
  {
    DownloadFirmware(fw_file, config_file);
  }

  std::string fw_data;
  if (!File::ReadFileToString(fw_path, fw_data))
  {
    PanicAlertFmtT("Bluetooth passthrough failed to read firmware file from:\n"
                   "{0}\n\n"
                   "Refer to https://wiki.dolphin-emu.org/index.php?title=Bluetooth_Passthrough "
                   "for instructions.",
                   fw_path);
    return false;
  }

  // The old 8723A takes its file as it is, which must not be an epatch container.
  if (version.lmp_subversion == LMP_8723A)
  {
    if (fw_data.starts_with(EPATCH_MAGIC))
    {
      ERROR_LOG_FMT(IOS_WIIMOTE, "Realtek 8723A firmware must not be an EPATCH file");
      return false;
    }
    return UploadImage(adapter, AsBytes(fw_data));
  }

  const std::array<u8, 3> rom_command{static_cast<u8>(OPCODE_READ_ROM_VERSION),
                                      static_cast<u8>(OPCODE_READ_ROM_VERSION >> 8), 0};
  // Return parameters: status, ROM version.
  std::array<u8, 2> rom_reply{};
  if (!adapter.SendBlockingCommand(rom_command, rom_reply))
    return false;
  if (rom_reply[0] != 0x00)
  {
    ERROR_LOG_FMT(IOS_WIIMOTE, "Realtek: Read ROM Version failed with status {:#04x}",
                  rom_reply[0]);
    return false;
  }
  const u8 rom_version = rom_reply[1];
  INFO_LOG_FMT(IOS_WIIMOTE, "Realtek ROM version {:#04x}", rom_version);

  // Only some chips have a config file; it is used whenever it is there.
  std::string config_data;
  File::ReadFileToString(config_path, config_data);

  const auto image = BuildImage(AsBytes(fw_data), AsBytes(config_data), version, rom_version);
  return UploadImage(adapter, std::span<const u8>{image.data(), image.size()});
}
