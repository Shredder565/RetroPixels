// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <unordered_set>

#include "Common/CommonTypes.h"
#include "Core/IOS/ES/Formats.h"
#include "Core/IOS/USB/Bluetooth/BTReal.h"

// Wii NAND helpers: installing and removing titles, system updates, NAND
// checks, and access to the emulated Bluetooth device.

namespace DiscIO
{
class VolumeWAD;
}

namespace IOS::HLE
{
class BluetoothEmuDevice;
class ESCore;
class Kernel;
}  // namespace IOS::HLE

namespace IOS::HLE::FS
{
class FileSystem;
}

namespace WiiUtils
{
enum class InstallType
{
  Permanent,
  // Recorded in SYSCONF (IPL.TID) so the next install can delete it again.
  Temporary,
};

bool InstallWAD(IOS::HLE::Kernel& ios, const DiscIO::VolumeWAD& wad, InstallType type);
// Opens the WAD at `wad_path` and installs it permanently using a throwaway IOS.
bool InstallWAD(const std::string& wad_path);

bool UninstallTitle(u64 title_id);

bool IsTitleInstalled(u64 title_id);

// Whether the title's content folder holds a title.tmd.
bool IsTMDImported(IOS::HLE::FS::FileSystem& fs, u64 title_id);

// Looks up the title in the System Menu's TMD store
// (/title/00000001/00000002/data/tmds.sys). Returns an invalid TMD if absent.
IOS::ES::TMDReader FindBackupTMD(IOS::HLE::FS::FileSystem& fs, u64 title_id);

// Makes sure the title has a title.tmd, importing one from tmds.sys if needed.
// Returns whether a title.tmd exists afterwards.
bool EnsureTMDIsImported(IOS::HLE::FS::FileSystem& fs, IOS::HLE::ESCore& es, u64 title_id);

enum class UpdateResult
{
  Succeeded,
  AlreadyUpToDate,

  // The disc is for another region than the installed System Menu.
  RegionMismatch,
  // The disc has no update partition.
  MissingUpdatePartition,
  // The disc's update files are missing or unreadable.
  DiscReadFailed,

  // The update server failed or returned nothing usable.
  ServerFailed,
  // A download failed.
  DownloadFailed,

  // ES refused an import.
  ImportFailed,
  // The callback asked to stop.
  Cancelled,

  NumberOfEntries,
};

// Progress report; return false to stop once the current title is done.
using UpdateCallback = std::function<bool(size_t processed, size_t total, u64 title_id)>;
// Brings all system titles up to the versions the update server lists. An
// empty region means the installed System Menu's region; without either the
// update fails.
UpdateResult DoOnlineUpdate(UpdateCallback update_callback, const std::string& region);

// Installs the update partition of a disc, as the System Menu would.
UpdateResult DoDiscUpdate(UpdateCallback update_callback, const std::string& image_path);

// Result of scanning the configured NAND for known kinds of damage, with
// usage statistics.
struct NANDCheckResult
{
  bool bad = false;
  std::unordered_set<u64> titles_to_remove;
  u64 used_clusters_user = 0;
  u64 used_clusters_system = 0;
  u64 used_inodes_user = 0;
  u64 used_inodes_system = 0;
};
NANDCheckResult CheckNAND(IOS::HLE::Kernel& ios);
bool RepairNAND(IOS::HLE::Kernel& ios);

// The running Wii's emulated Bluetooth device, or nullptr when no Wii software
// runs or passthrough is on. Call only from the CPU thread.
std::shared_ptr<IOS::HLE::BluetoothEmuDevice> GetBluetoothEmuDevice();
// The passthrough counterpart of GetBluetoothEmuDevice.
std::shared_ptr<IOS::HLE::BluetoothRealDevice> GetBluetoothRealDevice();
}  // namespace WiiUtils
