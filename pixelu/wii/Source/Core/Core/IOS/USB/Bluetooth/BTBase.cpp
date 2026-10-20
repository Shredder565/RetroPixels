// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/IOS/USB/Bluetooth/BTBase.h"

#include <string>
#include <vector>

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Core/SysConf.h"

namespace IOS::HLE
{
namespace
{
constexpr const char* DINF_ENTRY = "BT.DINF";

std::string BackupPath()
{
  return File::GetUserPath(D_CONFIG_IDX) + DIR_SEP WII_BTDINF_BACKUP;
}
}  // namespace

void BackUpBTInfoSection(const SysConf* sysconf)
{
  const std::string path = BackupPath();

  // An existing backup is the user's original list; never overwrite it.
  if (File::Exists(path))
    return;

  // The file is created even when there is nothing to back up.
  File::IOFile file(path, "wb");
  const SysConf::Entry* entry = sysconf->GetEntry(DINF_ENTRY);
  if (entry == nullptr)
    return;

  if (!file.WriteBytes(entry->bytes.data(), entry->bytes.size()))
    ERROR_LOG_FMT(IOS_WIIMOTE, "Could not write the BT.DINF backup");
}

void RestoreBTInfoSection(SysConf* sysconf)
{
  const std::string path = BackupPath();

  {
    File::IOFile file(path, "rb");
    if (!file)
      return;

    std::vector<u8>& bytes =
        sysconf->GetOrAddEntry(DINF_ENTRY, SysConf::Entry::Type::BigArray)->bytes;
    if (!file.ReadBytes(bytes.data(), bytes.size()))
    {
      // Keep the backup so that a later run can try again.
      ERROR_LOG_FMT(IOS_WIIMOTE, "Could not read the BT.DINF backup");
      return;
    }
  }

  File::Delete(path);
}
}  // namespace IOS::HLE
