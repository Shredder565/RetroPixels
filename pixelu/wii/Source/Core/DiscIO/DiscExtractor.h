// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <functional>
#include <string>
#include <string_view>

#include "Common/CommonTypes.h"

namespace DiscIO
{
class FileInfo;
struct Partition;
class Volume;

// Reads up to max_buffer_size bytes of a file, starting offset_in_file bytes in. Returns how many
// bytes were read (0 on failure or for directories).
u64 ReadFile(const Volume& volume, const Partition& partition, const FileInfo* file_info,
             u8* buffer, u64 max_buffer_size, u64 offset_in_file = 0);
u64 ReadFile(const Volume& volume, const Partition& partition, std::string_view path, u8* buffer,
             u64 max_buffer_size, u64 offset_in_file = 0);
// Writes a range of the volume to a host file.
bool ExportData(const Volume& volume, const Partition& partition, u64 offset, u64 size,
                const std::string& export_filename);
bool ExportFile(const Volume& volume, const Partition& partition, const FileInfo* file_info,
                const std::string& export_filename);
bool ExportFile(const Volume& volume, const Partition& partition, std::string_view path,
                const std::string& export_filename);

// Extracts the entries of a directory into export_folder (a subfolder named after the directory,
// unless it is the root). update_progress gets the disc path of each entry before it is handled
// and can return true to stop. filesystem_path is the disc path of `directory`.
void ExportDirectory(const Volume& volume, const Partition& partition, const FileInfo& directory,
                     bool recursive, const std::string& filesystem_path,
                     const std::string& export_folder,
                     const std::function<bool(const std::string& path)>& update_progress);

// The system files below can all be written at once with ExportSystemData.

bool ExportWiiUnencryptedHeader(const Volume& volume, const std::string& export_filename);
bool ExportWiiRegionData(const Volume& volume, const std::string& export_filename);

bool ExportTicket(const Volume& volume, const Partition& partition,
                  const std::string& export_filename);
bool ExportTMD(const Volume& volume, const Partition& partition,
               const std::string& export_filename);
bool ExportCertificateChain(const Volume& volume, const Partition& partition,
                            const std::string& export_filename);
bool ExportH3Hashes(const Volume& volume, const Partition& partition,
                    const std::string& export_filename);

bool ExportHeader(const Volume& volume, const Partition& partition,
                  const std::string& export_filename);
bool ExportBI2Data(const Volume& volume, const Partition& partition,
                   const std::string& export_filename);
bool ExportApploader(const Volume& volume, const Partition& partition,
                     const std::string& export_filename);
bool ExportDOL(const Volume& volume, const Partition& partition,
               const std::string& export_filename);
bool ExportFST(const Volume& volume, const Partition& partition,
               const std::string& export_filename);

// Writes sys/ (boot.bin, bi2.bin, apploader.img, main.dol, fst.bin) and, for Wii discs, disc/
// (header.bin, region.bin) plus ticket.bin, tmd.bin, cert.bin and h3.bin, in the layout that
// extracted-disc tools use.
bool ExportSystemData(const Volume& volume, const Partition& partition,
                      const std::string& export_folder);

}  // namespace DiscIO
