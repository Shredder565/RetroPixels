// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/FifoPlayer/FifoDataFile.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "Common/IOFile.h"
#include "Common/MsgHandler.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/HW/Memmap.h"
#include "Core/System.h"

// DFF (Dolphin FIFO file) layout, all values little-endian:
//
//   DffHeader                       at 0
//   DffFrame[frame_count]           right after the header
//   BP, CP, XF memory, XF registers, texture memory (in that order)
//   then for each frame: its FIFO bytes, its DffMemoryUpdate table, and the bytes of each
//   update.
//
// Every block is located through an absolute offset, so readers don't depend on this order.
// Reserved bytes are written as zero.
namespace
{
constexpr u32 DFF_MAGIC = 0x0d01f1f0;
constexpr u32 DFF_VERSION = 6;

// Oldest player able to read what this version writes...
constexpr u32 DFF_COMPATIBLE_LOADER = 1;
// ...unless the recording used non-default RAM sizes, which players before version 5 ignore.
constexpr u32 DFF_COMPATIBLE_LOADER_WITH_RAM_SIZES = 5;

// Format history relevant to loading.
constexpr u32 VERSION_WITH_TEXTURE_MEMORY = 4;
constexpr u32 VERSION_WITH_RAM_SIZES = 5;
constexpr u32 VERSION_WITH_GAME_ID = 6;

#pragma pack(push, 1)

struct DffHeader
{
  u32 magic;
  u32 version;
  u32 min_loader_version;
  u64 bp_mem_offset;
  u32 bp_mem_count;
  u64 cp_mem_offset;
  u32 cp_mem_count;
  u64 xf_mem_offset;
  u32 xf_mem_count;
  u64 xf_regs_offset;
  u32 xf_regs_count;
  u64 frame_table_offset;
  u32 frame_count;
  u32 flags;
  u64 tex_mem_offset;
  u32 tex_mem_size;
  u32 mem1_size;
  u32 mem2_size;
  char game_id[8];
  u8 reserved[24];
};
static_assert(sizeof(DffHeader) == 128);
static_assert(sizeof(DffHeader::game_id) == DEFAULT_GAME_ID.size());

struct DffFrame
{
  u64 fifo_data_offset;
  u32 fifo_data_size;
  u32 fifo_start;
  u32 fifo_end;
  u64 updates_offset;
  u32 update_count;
  u8 reserved[32];
};
static_assert(sizeof(DffFrame) == 64);

struct DffMemoryUpdate
{
  u32 fifo_position;
  u32 address;
  u64 data_offset;
  u32 data_size;
  u8 type;
  u8 reserved[3];
};
static_assert(sizeof(DffMemoryUpdate) == 24);

#pragma pack(pop)

bool SeekTo(File::IOFile& file, u64 offset)
{
  return file.Seek(static_cast<s64>(offset), File::SeekOrigin::Begin);
}

// Writes `block` at the current end of the file and returns where it went.
template <typename T, std::size_t N>
u64 AppendBlock(File::IOFile& file, const std::array<T, N>& block)
{
  const u64 position = file.Tell();
  file.WriteArray(block);
  return position;
}

std::nullptr_t ReportReadFailure()
{
  CriticalAlertFmtT("Failed to read DFF file.");
  return nullptr;
}
}  // namespace

FifoDataFile::FifoDataFile() = default;

FifoDataFile::~FifoDataFile() = default;

void FifoDataFile::SetFlag(u32 flag, bool set)
{
  m_Flags = set ? (m_Flags | flag) : (m_Flags & ~flag);
}

bool FifoDataFile::GetFlag(u32 flag) const
{
  return (m_Flags & flag) != 0;
}

void FifoDataFile::SetIsWii(bool isWii)
{
  SetFlag(FLAG_IS_WII, isWii);
}

bool FifoDataFile::GetIsWii() const
{
  return GetFlag(FLAG_IS_WII);
}

// Version 1 logs were made before EFB copies were handled; see FifoPlayer.h.
bool FifoDataFile::HasBrokenEFBCopies() const
{
  return m_Version < 2;
}

// Recordings never contain VI events, so playback always fakes them.
bool FifoDataFile::ShouldGenerateFakeVIUpdates()
{
  return true;
}

void FifoDataFile::AddFrame(const FifoFrameInfo& frameInfo)
{
  m_Frames.push_back(frameInfo);
}

void FifoDataFile::PadFile(size_t numBytes, File::IOFile& file)
{
  // Reserves space that is filled in later, so write errors are not tracked here.
  static constexpr std::array<u8, 4096> zeros{};
  std::FILE* const handle = file.GetHandle();
  while (numBytes != 0)
  {
    const size_t chunk = std::min(numBytes, zeros.size());
    std::fwrite(zeros.data(), 1, chunk, handle);
    numBytes -= chunk;
  }
}

bool FifoDataFile::Save(const std::string& filename)
{
  File::IOFile file;
  if (!file.Open(filename, "wb"))
    return false;

  // Room for the header and the frame table, which can only be written once the offsets of
  // everything else are known.
  PadFile(sizeof(DffHeader), file);
  const u64 frame_table_offset = file.Tell();
  PadFile(m_Frames.size() * sizeof(DffFrame), file);

  DffHeader header{};
  header.bp_mem_offset = AppendBlock(file, m_BPMem);
  header.cp_mem_offset = AppendBlock(file, m_CPMem);
  header.xf_mem_offset = AppendBlock(file, m_XFMem);
  header.xf_regs_offset = AppendBlock(file, m_XFRegs);
  header.tex_mem_offset = AppendBlock(file, m_TexMem);

  header.magic = DFF_MAGIC;
  header.version = DFF_VERSION;
  // Only demand a newer player when the RAM sizes actually matter.
  const bool custom_ram_size =
      Config::Get(Config::MAIN_RAM_OVERRIDE_ENABLE) ||
      SConfig::GetInstance().GetSimulatedMemorySize() > Memory::MEM1_SIZE_RETAIL;
  header.min_loader_version =
      custom_ram_size ? DFF_COMPATIBLE_LOADER_WITH_RAM_SIZES : DFF_COMPATIBLE_LOADER;
  header.bp_mem_count = BP_MEM_SIZE;
  header.cp_mem_count = CP_MEM_SIZE;
  header.xf_mem_count = XF_MEM_SIZE;
  header.xf_regs_count = XF_REGS_SIZE;
  header.tex_mem_size = TEX_MEM_SIZE;
  header.frame_table_offset = frame_table_offset;
  header.frame_count = static_cast<u32>(m_Frames.size());
  header.flags = m_Flags;

  const Memory::MemoryManager& memory = Core::System::GetInstance().GetMemory();
  header.mem1_size = memory.GetRamSizeReal();
  header.mem2_size = memory.GetExRamSizeReal();

  // The id field is fixed-size and NUL padded; an id that doesn't fit is replaced by the
  // default one.
  const std::string game_id = SConfig::GetInstance().GetGameID();
  const std::string_view stored_id =
      game_id.size() > sizeof(header.game_id) ? DEFAULT_GAME_ID : std::string_view(game_id.c_str());
  std::memcpy(header.game_id, stored_id.data(), stored_id.size());

  file.Seek(0, File::SeekOrigin::Begin);
  file.WriteBytes(&header, sizeof(header));

  for (size_t index = 0; index < m_Frames.size(); ++index)
  {
    const FifoFrameInfo& frame = m_Frames[index];

    DffFrame entry{};
    file.Seek(0, File::SeekOrigin::End);
    entry.fifo_data_offset = file.Tell();
    file.WriteBytes(frame.fifoData.data(), frame.fifoData.size());
    entry.updates_offset = WriteMemoryUpdates(frame.memoryUpdates, file);
    entry.fifo_data_size = static_cast<u32>(frame.fifoData.size());
    entry.fifo_start = frame.fifoStart;
    entry.fifo_end = frame.fifoEnd;
    entry.update_count = static_cast<u32>(frame.memoryUpdates.size());

    SeekTo(file, frame_table_offset + index * sizeof(DffFrame));
    file.WriteBytes(&entry, sizeof(entry));
  }

  return file.Close();
}

u64 FifoDataFile::WriteMemoryUpdates(std::span<const MemoryUpdate> memUpdates, File::IOFile& file)
{
  // The table goes first, each update's bytes are appended after it.
  const u64 table_offset = file.Tell();
  PadFile(memUpdates.size() * sizeof(DffMemoryUpdate), file);

  u64 entry_offset = table_offset;
  for (const MemoryUpdate& update : memUpdates)
  {
    DffMemoryUpdate entry{};
    file.Seek(0, File::SeekOrigin::End);
    entry.data_offset = file.Tell();
    file.WriteBytes(update.data.data(), update.data.size());
    entry.fifo_position = update.fifoPosition;
    entry.address = update.address;
    entry.data_size = static_cast<u32>(update.data.size());
    entry.type = static_cast<u8>(update.type);

    SeekTo(file, entry_offset);
    file.WriteBytes(&entry, sizeof(entry));
    entry_offset += sizeof(DffMemoryUpdate);
  }
  return table_offset;
}

void FifoDataFile::ReadMemoryUpdates(u64 fileOffset, u32 numUpdates,
                                     std::vector<MemoryUpdate>& memUpdates, File::IOFile& file)
{
  // Read errors are left to the caller's IsGood() check.
  memUpdates.resize(numUpdates);
  u64 entry_offset = fileOffset;
  for (MemoryUpdate& update : memUpdates)
  {
    DffMemoryUpdate entry{};
    SeekTo(file, entry_offset);
    file.ReadBytes(&entry, sizeof(entry));
    entry_offset += sizeof(DffMemoryUpdate);

    update.fifoPosition = entry.fifo_position;
    update.address = entry.address;
    update.type = static_cast<MemoryUpdate::Type>(entry.type);
    update.data.resize(entry.data_size);
    SeekTo(file, entry.data_offset);
    file.ReadBytes(update.data.data(), entry.data_size);
  }
}

std::unique_ptr<FifoDataFile> FifoDataFile::Load(const std::string& filename, bool flagsOnly)
{
  File::IOFile file;
  file.Open(filename, "rb");
  if (!file)
    return nullptr;

  if (file.GetSize() == 0)
  {
    CriticalAlertFmtT("DFF file size is 0; corrupt/incomplete file?");
    return nullptr;
  }

  DffHeader header;
  if (!file.ReadBytes(&header, sizeof(header)))
    return ReportReadFailure();

  if (header.magic != DFF_MAGIC)
  {
    CriticalAlertFmtT("DFF file magic number is incorrect: got {0:08x}, expected {1:08x}",
                      header.magic, DFF_MAGIC);
    return nullptr;
  }
  if (header.min_loader_version > DFF_VERSION)
  {
    CriticalAlertFmtT(
        "The DFF's minimum loader version ({0}) exceeds the version of this FIFO Player ({1})",
        header.min_loader_version, DFF_VERSION);
    return nullptr;
  }

  // Older files have no RAM size fields; they were always recorded with retail sizes.
  if (header.version < VERSION_WITH_RAM_SIZES)
  {
    header.mem1_size = Memory::MEM1_SIZE_RETAIL;
    header.mem2_size = Memory::MEM2_SIZE_RETAIL;
  }

  auto loaded = std::make_unique<FifoDataFile>();
  loaded->m_Flags = header.flags;
  loaded->m_Version = header.version;
  if (header.version >= VERSION_WITH_GAME_ID)
    loaded->m_game_id.assign(header.game_id, strnlen(header.game_id, sizeof(header.game_id)));
  else
    loaded->m_game_id = DEFAULT_GAME_ID;

  if (flagsOnly)
  {
    // The first, flags-only load happens before the emulated hardware is set up, so this is the
    // moment to make the RAM sizes match the recording.
    Config::SetCurrent(Config::MAIN_RAM_OVERRIDE_ENABLE, true);
    Config::SetCurrent(Config::MAIN_MEM1_SIZE, header.mem1_size);
    Config::SetCurrent(Config::MAIN_MEM2_SIZE, header.mem2_size);
    return loaded;
  }

  // By the full load the sizes should match; playing back with different ones can't work.
  const Memory::MemoryManager& memory = Core::System::GetInstance().GetMemory();
  const u32 have_mem1 = memory.GetRamSizeReal();
  const u32 have_mem2 = memory.GetExRamSizeReal();
  if (header.mem1_size != have_mem1 || header.mem2_size != have_mem2)
  {
    constexpr u32 MiB = 0x100000;
    CriticalAlertFmtT("Emulated memory size mismatch!\n"
                      "Current: MEM1 {0:08X} ({1} MiB), MEM2 {2:08X} ({3} MiB)\n"
                      "DFF: MEM1 {4:08X} ({5} MiB), MEM2 {6:08X} ({7} MiB)",
                      have_mem1, have_mem1 / MiB, have_mem2, have_mem2 / MiB, header.mem1_size,
                      header.mem1_size / MiB, header.mem2_size, header.mem2_size / MiB);
    return nullptr;
  }

  // Register blocks; a file may store fewer entries than we keep, never more.
  const auto read_registers = [&file](u64 offset, u32 stored_count, auto& block) {
    const u32 count = std::min(static_cast<u32>(block.size()), stored_count);
    SeekTo(file, offset);
    file.ReadArray(block.data(), count);
  };
  read_registers(header.bp_mem_offset, header.bp_mem_count, loaded->m_BPMem);
  read_registers(header.cp_mem_offset, header.cp_mem_count, loaded->m_CPMem);
  read_registers(header.xf_mem_offset, header.xf_mem_count, loaded->m_XFMem);
  read_registers(header.xf_regs_offset, header.xf_regs_count, loaded->m_XFRegs);

  // Texture memory is stored whole (the size field is not consulted).
  loaded->m_TexMem.fill(0);
  if (loaded->m_Version >= VERSION_WITH_TEXTURE_MEMORY)
  {
    SeekTo(file, header.tex_mem_offset);
    file.ReadArray(&loaded->m_TexMem);
  }

  if (!file.IsGood())
    return ReportReadFailure();

  loaded->m_ram_size_real = header.mem1_size;
  loaded->m_exram_size_real = header.mem2_size;

  u64 frame_entry_offset = header.frame_table_offset;
  for (u32 remaining = header.frame_count; remaining != 0; --remaining)
  {
    DffFrame entry;
    SeekTo(file, frame_entry_offset);
    if (!file.ReadBytes(&entry, sizeof(entry)))
      return ReportReadFailure();
    frame_entry_offset += sizeof(DffFrame);

    FifoFrameInfo frame;
    frame.fifoStart = entry.fifo_start;
    frame.fifoEnd = entry.fifo_end;
    frame.fifoData.resize(entry.fifo_data_size);
    SeekTo(file, entry.fifo_data_offset);
    file.ReadBytes(frame.fifoData.data(), entry.fifo_data_size);
    ReadMemoryUpdates(entry.updates_offset, entry.update_count, frame.memoryUpdates, file);

    if (!file.IsGood())
      return ReportReadFailure();
    loaded->AddFrame(frame);
  }

  return loaded;
}
