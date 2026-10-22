// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/NetPlayCommon.h"

#include <algorithm>
#include <functional>

#include <fmt/format.h>
#include <lzo/lzo1x.h>

#include "Common/FileUtil.h"
#include "Common/HttpRequest.h"
#include "Common/IOFile.h"
#include "Common/MsgHandler.h"
#include "Common/SFMLHelper.h"
#include "InputCommon/GCPadStatus.h"

namespace NetPlay
{
namespace
{
// Uncompressed bytes per chunk, and the worst-case size of one compressed chunk (LZO1X can grow
// incompressible input by 1/16 plus a few bytes).
constexpr u32 kChunkSize = 64 * 1024;
constexpr u32 kMaxCompressedChunk = kChunkSize + kChunkSize / 16 + 64 + 3;

// Fills the buffer with the next `length` input bytes; returns false on a read error.
using ChunkSource = std::function<bool(u64 offset, u32 length, const u8** data)>;

// Writes `total` bytes from `source` as compressed chunks, after the size.
bool CompressInto(sf::Packet& packet, u64 total, const ChunkSource& source)
{
  packet << total;
  if (total == 0)
    return true;

  std::vector<u8> compressed(kMaxCompressedChunk);
  std::vector<u8> work_memory(LZO1X_1_MEM_COMPRESS);

  u64 offset = 0;
  while (offset < total)
  {
    const u32 length = static_cast<u32>(std::min<u64>(total - offset, kChunkSize));
    const u8* input = nullptr;
    if (!source(offset, length, &input))
      return false;

    lzo_uint compressed_length = 0;
    if (lzo1x_1_compress(input, length, compressed.data(), &compressed_length,
                         work_memory.data()) != LZO_E_OK)
    {
      PanicAlertFmtT("Internal LZO Error - compression failed");
      return false;
    }

    packet << static_cast<u32>(compressed_length);
    for (lzo_uint i = 0; i < compressed_length; ++i)
      packet << compressed[i];

    offset += length;
    // A short chunk is the last one.
    if (length != kChunkSize)
      break;
  }

  // End of the chunks.
  packet << u32{0};
  return true;
}

// Reads chunks until the terminator and hands each decompressed chunk to `sink` (which may refuse
// it). `total` is the announced size, which the chunks may not exceed.
enum class ChunkResult
{
  Ok,
  Failed,
};
using ChunkSink = std::function<bool(const u8* data, std::size_t length)>;
ChunkResult DecompressFrom(sf::Packet& packet, u64 total, u64* produced, const ChunkSink& sink,
                           bool* too_large)
{
  std::vector<u8> compressed(kMaxCompressedChunk);
  std::vector<u8> chunk(kChunkSize);
  *produced = 0;
  *too_large = false;

  for (;;)
  {
    u32 compressed_length = 0;
    packet >> compressed_length;
    // The terminator (or the end of the packet).
    if (compressed_length == 0)
      return ChunkResult::Ok;

    if (compressed_length > compressed.size())
    {
      PanicAlertFmt("LZO error - input is too large");
      return ChunkResult::Failed;
    }
    for (u32 i = 0; i < compressed_length; ++i)
      packet >> compressed[i];

    lzo_uint chunk_length = chunk.size();
    if (lzo1x_decompress_safe(compressed.data(), compressed_length, chunk.data(), &chunk_length,
                              nullptr) != LZO_E_OK)
    {
      PanicAlertFmtT("Internal LZO Error - decompression failed");
      return ChunkResult::Failed;
    }

    if (chunk_length > total - *produced)
    {
      *too_large = true;
      return ChunkResult::Failed;
    }
    if (!sink(chunk.data(), chunk_length))
      return ChunkResult::Failed;
    *produced += chunk_length;
  }
}

bool IsSafeEntryName(const std::string& name)
{
  // No path separators, and not "", "." or "..".
  if (name.contains('/'))
    return false;
#ifdef _WIN32
  if (name.contains('\\'))
    return false;
#endif
  return !std::ranges::all_of(name, [](char c) { return c == '.'; });
}

bool WriteFolderLevel(const File::FSTEntry& folder, sf::Packet& packet)
{
  packet << static_cast<u64>(folder.children.size());
  for (const File::FSTEntry& entry : folder.children)
  {
    const bool is_folder = entry.isDirectory;
    packet << entry.virtualName;
    packet << is_folder;
    const bool written = is_folder ? WriteFolderLevel(entry, packet) :
                                     CompressFileIntoPacket(entry.physicalName, packet);
    if (!written)
      return false;
  }
  return true;
}

bool ReadFolderLevel(sf::Packet& packet, const std::string& folder_path)
{
  if (!File::CreateFullPath(folder_path + "/"))
    return false;

  // A level whose entry count is missing is malformed.
  u64 entries = 0;
  if (!(packet >> entries))
    return false;
  for (u64 i = 0; i < entries; ++i)
  {
    std::string name;
    packet >> name;
    if (!IsSafeEntryName(name))
      return false;

    bool is_folder = false;
    packet >> is_folder;
    const std::string path = fmt::format("{}/{}", folder_path, name);
    const bool read =
        is_folder ? ReadFolderLevel(packet, path) : DecompressPacketIntoFile(packet, path);
    if (!read)
      return false;
  }
  return true;
}
}  // namespace

bool CompressFileIntoPacket(const std::string& file_path, sf::Packet& packet)
{
  File::IOFile file(file_path, "rb");
  if (!file)
  {
    PanicAlertFmtT("Failed to open file \"{0}\".", file_path);
    return false;
  }

  std::vector<u8> buffer(kChunkSize);
  return CompressInto(packet, file.GetSize(), [&](u64, u32 length, const u8** data) {
    if (!file.ReadBytes(buffer.data(), length))
    {
      PanicAlertFmtT("Error reading file: {0}", file_path.c_str());
      return false;
    }
    *data = buffer.data();
    return true;
  });
}

bool CompressFolderIntoPacket(const std::string& folder_path, sf::Packet& packet)
{
  const bool exists = File::IsDirectory(folder_path);
  packet << exists;
  if (!exists)
    return true;
  return WriteFolderLevel(File::ScanDirectoryTree(folder_path, true), packet);
}

bool CompressBufferIntoPacket(std::span<const u8> in_buffer, sf::Packet& packet)
{
  return CompressInto(packet, in_buffer.size(), [&](u64 offset, u32, const u8** data) {
    *data = in_buffer.data() + offset;
    return true;
  });
}

bool DecompressPacketIntoFile(sf::Packet& packet, const std::string& file_path)
{
  const u64 size = Common::PacketReadU64(packet);
  // Empty files aren't created.
  if (size == 0)
    return true;

  File::IOFile file(file_path, "wb");
  if (!file)
  {
    PanicAlertFmtT("Failed to open file \"{0}\". Verify your write permissions.", file_path);
    return false;
  }

  u64 written = 0;
  bool too_large = false;
  const ChunkResult result =
      DecompressFrom(packet, size, &written, [&](const u8* data, std::size_t length) {
        if (file.WriteBytes(data, length))
          return true;
        PanicAlertFmtT("Error writing file: {0}", file_path);
        return false;
      }, &too_large);
  if (too_large)
    PanicAlertFmtT("LZO error - output is too large");
  return result == ChunkResult::Ok && written == size;
}

bool DecompressPacketIntoFolder(sf::Packet& packet, const std::string& folder_path)
{
  bool existed = false;
  packet >> existed;
  return !existed || ReadFolderLevel(packet, folder_path);
}

std::optional<std::vector<u8>> DecompressPacketIntoBuffer(sf::Packet& packet)
{
  const u64 size = Common::PacketReadU64(packet);
  std::vector<u8> output;
  if (size == 0)
    return output;

  u64 produced = 0;
  bool too_large = false;
  const ChunkResult result =
      DecompressFrom(packet, size, &produced, [&](const u8* data, std::size_t length) {
        if (length > output.max_size() - output.size())
        {
          too_large = true;
          return false;
        }
        output.insert(output.end(), data, data + length);
        return true;
      }, &too_large);
  if (too_large)
  {
    PanicAlertFmtT("LZO error - output is too large");
    return std::nullopt;
  }
  if (result != ChunkResult::Ok)
    return std::nullopt;
  if (produced != size)
  {
    PanicAlertFmtT("LZO error - output size mismatch");
    return std::nullopt;
  }
  return output;
}

void WriteSyncIdentifier(sf::Packet& packet, const SyncIdentifier& identifier)
{
  packet << u64{identifier.dol_elf_size} << identifier.game_id << identifier.revision
         << identifier.disc_number << identifier.is_datel;
  for (const u8 hash_byte : identifier.sync_hash)
    packet << hash_byte;
}

void ReadSyncIdentifier(sf::Packet& packet, SyncIdentifier* identifier)
{
  identifier->dol_elf_size = Common::PacketReadU64(packet);
  packet >> identifier->game_id >> identifier->revision >> identifier->disc_number >>
      identifier->is_datel;
  for (u8& hash_byte : identifier->sync_hash)
    packet >> hash_byte;
}

void WritePadState(sf::Packet& packet, const GCPadStatus& pad, bool buttons_only)
{
  packet << pad.button;
  if (buttons_only)
    return;
  packet << pad.analogA << pad.analogB;
  packet << pad.stickX << pad.stickY << pad.substickX << pad.substickY;
  packet << pad.triggerLeft << pad.triggerRight;
  packet << pad.isConnected;
}

void ReadPadState(sf::Packet& packet, GCPadStatus* pad, bool buttons_only)
{
  packet >> pad->button;
  if (buttons_only)
    return;
  packet >> pad->analogA >> pad->analogB;
  packet >> pad->stickX >> pad->stickY >> pad->substickX >> pad->substickY;
  packet >> pad->triggerLeft >> pad->triggerRight;
  packet >> pad->isConnected;
}

std::string GetExternalIPAddress()
{
  Common::HttpRequest request;
  // ENet only does IPv4.
  request.UseIPv4();
  const Common::HttpRequest::Response response =
      request.Get("https://ip.dolphin-emu.org/", {{"X-Is-Dolphin", "1"}});
  if (!response)
    return "";
  return std::string(response->begin(), response->end());
}

}  // namespace NetPlay
