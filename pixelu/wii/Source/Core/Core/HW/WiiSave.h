// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>

#include "Common/CommonTypes.h"

namespace IOS::HLE
{
namespace FS
{
class FileSystem;
}
class IOSC;
}  // namespace IOS::HLE

// Wii save data, either in a title's NAND data directory or in the
// encrypted data.bin format the System Menu uses on SD cards.
namespace WiiSave
{
class Storage;
struct StorageDeleter
{
  void operator()(Storage* p) const;
};

using StoragePointer = std::unique_ptr<Storage, StorageDeleter>;
// The save of title `tid` on the NAND behind `fs`.
StoragePointer MakeNandStorage(IOS::HLE::FS::FileSystem* fs, u64 tid);
// A data.bin file at `path`, opened with the fopen `mode`.
StoragePointer MakeDataBinStorage(IOS::HLE::IOSC* iosc, const std::string& path, const char* mode);

enum class CopyResult
{
  Success,
  Error,
  Cancelled,
  CorruptedSource,
  TitleMissing,
  NumberOfEntries
};

// Replaces the save in `destination` with the one in `source`. The
// destination is untouched when the source cannot be read completely.
CopyResult Copy(Storage* source, Storage* destination);

// Copies a data.bin into the NAND. `can_overwrite` is asked before an
// existing save is replaced.
CopyResult Import(const std::string& data_bin_path, const std::function<bool()>& can_overwrite);
// Writes a title's save to <export_path>/private/wii/title/<game code>/data.bin.
CopyResult Export(u64 tid, std::string_view export_path);
// Exports the save of every installed title; returns how many succeeded.
size_t ExportAll(std::string_view export_path);
}  // namespace WiiSave
