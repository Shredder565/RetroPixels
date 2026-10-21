// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/State.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <locale>
#include <map>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <fmt/chrono.h>
#include <fmt/format.h>
#include <fmt/std.h>

#include <lz4.h>
#include <lzo/lzo1x.h>

#include "Common/Buffer.h"
#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/Contains.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/StringUtil.h"
#include "Common/Thread.h"
#include "Common/TimeUtil.h"
#include "Common/TransferableSharedMutex.h"
#include "Common/Version.h"
#include "Common/WorkQueueThread.h"

#include "Core/AchievementManager.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/GeckoCode.h"
#include "Core/HW/HW.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/Wiimote.h"
#include "Core/Movie.h"
#include "Core/NetPlayProto.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

#include "UICommon/UICommon.h"

#include "VideoCommon/FrameDumpFFMpeg.h"
#include "VideoCommon/OnScreenDisplay.h"
#include "VideoCommon/VideoBackendBase.h"

namespace State
{
namespace
{
// --- Format constants ------------------------------------------------------------------

// Bump whenever anything saved by DoState changes.
constexpr u32 STATE_VERSION = 192;
// Bump when StateExtendedHeader changes.
constexpr u32 EXTENDED_HEADER_VERSION = 1;
// Bytes between the extended header and the payload.
constexpr u32 COMPRESSED_DATA_OFFSET = 0;
// version_cookie = COOKIE_BASE + STATE_VERSION.
constexpr u32 COOKIE_BASE = 0xBAADBABE;

constexpr bool USE_COMPRESSION = true;

// States before version 43 did not store the build that made them; this
// names the range of releases that wrote each such version. States older than
// 17 have a different header and cannot be read at all.
const std::map<u32, std::pair<std::string, std::string>> s_old_versions = {
    {17, {"3.5-1311", "3.5-1364"}},
    {18, {"3.5-1366", "3.5-1371"}},
    {19, {"3.5-1372", "3.5-1408"}},
    {20, {"3.5-1409", "4.0-704"}},
    {21, {"4.0-705", "4.0-889"}},
    {22, {"4.0-905", "4.0-1871"}},
    {23, {"4.0-1873", "4.0-1900"}},
    {24, {"4.0-1902", "4.0-1919"}},
    {25, {"4.0-1921", "4.0-1936"}},
    {26, {"4.0-1939", "4.0-1959"}},
    {27, {"4.0-1961", "4.0-2018"}},
    {28, {"4.0-2020", "4.0-2291"}},
    {29, {"4.0-2293", "4.0-2360"}},
    {30, {"4.0-2362", "4.0-2628"}},
    {31, {"4.0-2632", "4.0-3331"}},
    {32, {"4.0-3334", "4.0-3340"}},
    {33, {"4.0-3342", "4.0-3373"}},
    {34, {"4.0-3376", "4.0-3402"}},
    {35, {"4.0-3409", "4.0-3603"}},
    {36, {"4.0-3610", "4.0-4480"}},
    {37, {"4.0-4484", "4.0-4943"}},
    {38, {"4.0-4963", "4.0-5267"}},
    {39, {"4.0-5279", "4.0-5525"}},
    {40, {"4.0-5531", "4.0-5809"}},
    {41, {"4.0-5811", "4.0-5923"}},
    {42, {"4.0-5925", "4.0-5946"}},
};

// Legacy LZO states: the first compressed block is read into this buffer. Its
// size is the LZO worst case for a 128 KiB input block.
constexpr u32 LZO_BLOCK_SIZE = 128 * 1024u;
constexpr u32 LZO_SCRATCH_SIZE = LZO_BLOCK_SIZE + LZO_BLOCK_SIZE / 16 + 64 + 3;
unsigned char s_lzo_scratch[LZO_SCRATCH_SIZE];

// Header times count seconds from 38 (365-day) years after 1970, which keeps
// more sub-second precision in the double.
constexpr int TIME_EPOCH_SHIFT = 38 * 365 * 24 * 60 * 60;

// --- Shared state -------------------------------------------------------------------

AfterLoadCallbackFunc s_after_load;
Common::EventHook s_flush_hook;

// The machine as it was before the last load, for UndoLoadState.
Common::UniqueBuffer<u8> s_undo_load_buffer;

// Size of the last state saved; the next save starts with a buffer a bit larger.
u32 s_last_state_size = 0;

// Every save task holds this shared from the moment it is requested until its
// file is written (passing from the caller to the CPU thread to the writer).
// Taking it exclusively therefore waits for all pending saves and keeps new
// ones from starting.
Common::TransferableSharedMutex s_save_tasks;

[[nodiscard]] std::shared_lock<Common::TransferableSharedMutex> HoldSaveTask()
{
  return std::shared_lock{s_save_tasks};
}

struct PendingWrite
{
  Common::UniqueBuffer<u8> buffer;
  std::string filename;
  std::shared_lock<Common::TransferableSharedMutex> task_lock;
};

// Compresses and writes states in the background. Only the CPU thread queues to it.
Common::WorkQueueThreadSP<PendingWrite> s_writer;

std::string StateDir()
{
  return File::GetUserPath(D_STATESAVES_IDX);
}

std::string SlotPath(u32 slot)
{
  return fmt::format("{}{}.s{:02d}", StateDir(), SConfig::GetInstance().GetGameID(), slot);
}

std::string UndoMoviePath()
{
  return StateDir() + "undo.dtm";
}

std::string FileNameOnly(const std::string& path)
{
  return PathToString(std::filesystem::path(StringToPath(path)).filename());
}

double NowForHeader()
{
  using namespace std::chrono;
  const auto shifted = system_clock::now().time_since_epoch() - seconds(TIME_EPOCH_SHIFT);
  return duration_cast<duration<double>>(shifted).count();
}

std::string HeaderTimeToText(double time)
{
  const auto local = Common::LocalTime(static_cast<time_t>(time) + TIME_EPOCH_SHIFT);
  if (!local)
    return "";
  // fmt needs the locale passed explicitly to honour the user's date format.
  return fmt::format(std::locale{""}, "{:%x %X}", *local);
}

// --- The machine state ----------------------------------------------------------------

void DoState(Core::System& system, PointerWrap& p)
{
  // A Wii state cannot load in GameCube mode or the other way round.
  const bool running_wii = system.IsWii() || system.IsMIOS();
  bool state_wii = running_wii;
  p.Do(state_wii);
  if (state_wii != running_wii)
  {
    OSD::AddMessage(fmt::format("Cannot load a savestate created under {} mode in {} mode",
                                state_wii ? "Wii" : "GC", running_wii ? "Wii" : "GC"),
                    OSD::Duration::NORMAL, OSD::Color::RED);
    p.SetMeasureMode();
    return;
  }

  // Neither can a state made with different MEM1/MEM2 sizes.
  auto& memory = system.GetMemory();
  u32 mem1 = memory.GetRamSizeReal();
  u32 mem2 = memory.GetExRamSizeReal();
  p.Do(mem1);
  p.Do(mem2);
  if (mem1 != memory.GetRamSizeReal() || mem2 != memory.GetExRamSizeReal())
  {
    constexpr u32 MIB = 0x100000U;
    OSD::AddMessage(fmt::format("Memory size mismatch!\nCurrent | MEM1 {:08X} ({:3}MB)    "
                                "MEM2 {:08X} ({:3}MB)\nState   | MEM1 {:08X} ({:3}MB)    "
                                "MEM2 {:08X} ({:3}MB)",
                                memory.GetRamSizeReal(), memory.GetRamSizeReal() / MIB,
                                memory.GetExRamSizeReal(), memory.GetExRamSizeReal() / MIB, mem1,
                                mem1 / MIB, mem2, mem2 / MIB));
    p.SetMeasureMode();
    return;
  }

  // The order below is part of the format, and also matters at load time:
  // - Movie first, so the frame counter is current when the video backend
  //   redraws during its load.
  // - The video backend next, so it can flush its caches back to RAM.
  // - CoreTiming before HW, since loading controller state may schedule events.
  // - HW before PowerPC, since the data cache may need flushing.
  system.GetMovie().DoState(p);
  p.DoMarker("Movie");

  g_video_backend->DoState(p);
  p.DoMarker("video_backend");

  system.GetCoreTiming().DoState(p);
  p.DoMarker("CoreTiming");

  HW::DoState(system, p);
  p.DoMarker("HW");

  system.GetPowerPC().DoState(p);
  p.DoMarker("PowerPC");

  if (system.IsWii())
    Wiimote::DoState(p);
  p.DoMarker("Wiimote");

  Gecko::DoState(p);
  p.DoMarker("Gecko");

#ifdef USE_RETRO_ACHIEVEMENTS
  AchievementManager::GetInstance().DoState(p);
#endif  // USE_RETRO_ACHIEVEMENTS
}

bool LoadingAllowed(Core::System& system)
{
  if (!Core::IsRunningOrStarting(system))
    return false;

  const char* refusal = nullptr;
  if (NetPlay::IsNetPlayRunning())
    refusal = "Loading savestates is disabled in Netplay to prevent desyncs";
  else if (AchievementManager::GetInstance().IsHardcoreModeActive())
    refusal = "Loading savestates is disabled in RetroAchievements hardcore mode";

  if (refusal != nullptr)
    OSD::AddMessage(refusal);
  return refusal == nullptr;
}

bool RestoreFrom(Core::System& system, std::span<u8> data)
{
  u8* cursor = data.data();
  PointerWrap p(&cursor, data.size(), PointerWrap::Mode::Read);
  DoState(system, p);
  // A failed check switches the wrapper out of read mode.
  return p.IsReadMode();
}

// Serialises the machine into `buffer`, growing it if needed. Returns the
// state's size, or 0 on failure.
size_t CaptureInto(Core::System& system, Common::UniqueBuffer<u8>& buffer)
{
  for (;;)
  {
    u8* cursor = buffer.data();
    PointerWrap p(&cursor, buffer.size(), PointerWrap::Mode::Write);
    DoState(system, p);
    const auto needed = p.GetOffsetFromPreviousPosition(buffer.data());

    if (p.IsWriteMode())
    {
      s_last_state_size = needed;
      return needed;
    }
    // Leaving write mode with a fitting buffer means DoState itself failed.
    if (needed <= buffer.size())
      return 0;

    // Too small: the wrapper measured what is needed; try again with that.
    DEBUG_LOG_FMT(CORE, "SaveToBuffer: Growing buffer from size {} to measured size {}",
                  buffer.size(), needed);
    buffer.reset(needed);
  }
}

// --- Writing state files --------------------------------------------------------------

struct UsedSlot
{
  u32 slot;  // 1-based
  double time;
};

bool ReadLegacyHeaderOnly(const std::string& path, StateHeader& header);

std::vector<UsedSlot> FindUsedSlots()
{
  std::vector<UsedSlot> used;
  StateHeader header;
  for (u32 slot = 1; slot <= NUM_STATES; ++slot)
  {
    const std::string path = SlotPath(slot);
    if (File::Exists(path) && ReadLegacyHeaderOnly(path, header))
      used.push_back({slot, header.legacy_header.time});
  }
  return used;
}

std::optional<u32> FirstFreeSlot(std::span<const UsedSlot> used)
{
  for (u32 slot = 1; slot <= NUM_STATES; ++slot)
  {
    if (!Common::Contains(used, slot, &UsedSlot::slot))
      return slot;
  }
  return std::nullopt;
}

// The LZ4 payload: blocks of at most LZ4_MAX_INPUT_SIZE input bytes, each
// written as its compressed length (a native int) followed by the data.
void WriteLZ4Payload(std::span<const u8> data, File::IOFile& file)
{
  const char* const source = reinterpret_cast<const char*>(data.data());
  u64 done = 0;
  do
  {
    const int chunk =
        static_cast<int>(std::min<u64>(static_cast<u64>(LZ4_MAX_INPUT_SIZE), data.size() - done));
    Common::UniqueBuffer<char> packed(static_cast<size_t>(LZ4_compressBound(chunk)));
    const int packed_size =
        LZ4_compress_default(source + done, packed.get(), chunk, static_cast<int>(packed.size()));
    if (packed_size == 0)
    {
      PanicAlertFmtT("Internal LZ4 Error - compression failed");
      return;
    }

    file.WriteArray(&packed_size, 1);
    file.WriteBytes(packed.get(), static_cast<size_t>(packed_size));
    done += static_cast<u64>(chunk);
  } while (done != data.size());
}

void WriteHeaders(size_t payload_size, File::IOFile& file)
{
  StateHeader header{};
  auto& legacy = header.legacy_header;
  SConfig::GetInstance().GetGameID().copy(legacy.game_id, std::size(legacy.game_id));
  legacy.time = NowForHeader();

  header.version_string = Common::GetScmRevStr();
  header.version_header.version_cookie = COOKIE_BASE + STATE_VERSION;
  header.version_header.version_string_length = static_cast<u32>(header.version_string.size());

  StateExtendedHeader extended{};
  extended.base_header.header_version = EXTENDED_HEADER_VERSION;
  extended.base_header.compression_type =
      USE_COMPRESSION ? CompressionType::LZ4 : CompressionType::Uncompressed;
  extended.base_header.payload_offset = COMPRESSED_DATA_OFFSET;
  extended.base_header.uncompressed_size = payload_size;

  file.WriteArray(&header.legacy_header, 1);
  file.WriteArray(&header.version_header, 1);
  file.WriteString(header.version_string);
  file.WriteArray(&extended.base_header, 1);
}

// Before a slot is overwritten, its old state (and movie) become lastState.sav
// (and lastState.sav.dtm) for UndoSaveState.
void KeepOverwrittenState(const std::string& path, const std::string& movie_path)
{
  if (!File::Exists(path))
    return;

  const std::string backup = StateDir() + "lastState.sav";
  const std::string backup_movie = backup + ".dtm";
  if (File::Exists(backup))
    File::Delete(backup);
  if (File::Exists(backup_movie))
    File::Delete(backup_movie);

  if (!File::Rename(path, backup))
  {
    Core::DisplayMessage("Failed to move previous state to state undo backup", 1000);
    return;
  }
  if (File::Exists(movie_path) && !File::Rename(movie_path, backup_movie))
    Core::DisplayMessage("Failed to move previous state's dtm to state undo backup", 1000);
}

// Runs on the writer thread.
void WriteStateFile(Core::System& system, const PendingWrite& job)
{
  const std::string& path = job.filename;

  // Write next to the target under an unused temporary name, then rename.
  // (Another instance using the same folder could still race for the name.)
  std::string temp_path;
  for (auto suffix = static_cast<size_t>(Common::CurrentThreadId());; ++suffix)
  {
    temp_path = fmt::format("{}{}.tmp", path, suffix);
    if (!File::Exists(temp_path))
      break;
  }

  File::IOFile file(temp_path, "wb");
  if (!file)
  {
    Core::DisplayMessage("Failed to create state file", 2000);
    return;
  }

  WriteHeaders(job.buffer.size(), file);
  if (USE_COMPRESSION)
    WriteLZ4Payload(job.buffer, file);
  else
    file.WriteBytes(job.buffer.data(), job.buffer.size());

  if (!file.IsGood())
    Core::DisplayMessage("Failed to write state file", 2000);

  const std::string movie_path = path + ".dtm";
  KeepOverwrittenState(path, movie_path);

  // A state saved during a movie carries the movie up to this point.
  auto& movie = system.GetMovie();
  if (!movie.IsMovieActive())
    File::Delete(movie_path);
  else if (!movie.IsJustStartingRecordingInputFromSaveState())
    movie.SaveRecording(movie_path);

  if (!file.Close())
    Core::DisplayMessage("Failed to close state file", 2000);

  if (File::Rename(temp_path, path))
    Core::DisplayMessage(fmt::format("Saved State to {}", FileNameOnly(path)), 2000);
  else
    Core::DisplayMessage("Failed to rename state file", 2000);
}

// Runs on the CPU thread.
void CaptureAndQueue(Core::System& system, std::string filename)
{
  // Start 10% above the last size, which usually avoids a second pass.
  Common::UniqueBuffer<u8> buffer{static_cast<size_t>(s_last_state_size) * 110 / 100};
  const size_t size = CaptureInto(system, buffer);
  if (size == 0)
  {
    Core::DisplayMessage("Unable to save: Internal DoState Error", 4000);
    return;
  }

  // Trim the buffer's length to the state; the memory itself is kept.
  buffer.assign(buffer.extract().first, size);

  PendingWrite job{
      .buffer = std::move(buffer),
      .filename = std::move(filename),
      .task_lock = HoldSaveTask(),
  };
  Core::DisplayMessage("Saving State...", 1000);
  s_writer.EmplaceItem(std::move(job));
}

// --- Reading state files --------------------------------------------------------------

// Very old states keep their version header and string at the start of the
// first LZO block; decompress that block to get them.
bool ReadVersionFromLZO(StateHeader& header, File::IOFile& file)
{
  lzo_uint32 packed_size = 0;
  lzo_uint unpacked_size = 0;
  Common::UniqueBuffer<u8> block(header.legacy_header.lzo_size);

  if (!file.ReadArray(&packed_size, 1))
    return false;
  // Refuse a block larger than the scratch buffer instead of overrunning it.
  if (packed_size > LZO_SCRATCH_SIZE || !file.ReadBytes(s_lzo_scratch, packed_size))
    return false;

  const int result =
      lzo1x_decompress(s_lzo_scratch, packed_size, block.data(), &unpacked_size, nullptr);
  if (result != LZO_E_OK)
  {
    PanicAlertFmtT("Internal LZO Error - decompression failed ({0}) ({1}) \nUnable to "
                   "retrieve outdated savestate version info.",
                   result, unpacked_size);
    return false;
  }

  constexpr size_t VERSION_SIZE = sizeof(StateHeaderVersion);
  if (block.size() < VERSION_SIZE)
  {
    PanicAlertFmtT("Internal LZO Error - failed to parse decompressed version cookie "
                   "and version string length ({0})",
                   block.size());
    return false;
  }
  std::memcpy(&header.version_header, block.data(), VERSION_SIZE);

  const u32 string_length = header.version_header.version_string_length;
  if (block.size() < VERSION_SIZE + string_length)
  {
    PanicAlertFmtT("Internal LZO Error - failed to parse decompressed version string "
                   "({0} / {1})",
                   string_length, block.size());
    return false;
  }
  header.version_string.assign(reinterpret_cast<const char*>(block.data() + VERSION_SIZE),
                               string_length);
  return true;
}

bool ReadHeaders(StateHeader& header, File::IOFile& file, bool with_version = true)
{
  if (!file.IsOpen())
  {
    Core::DisplayMessage("State not found", 2000);
    return false;
  }
  if (!file.ReadArray(&header.legacy_header, 1))
  {
    Core::DisplayMessage("Failed to read state legacy header", 2000);
    return false;
  }
  if (!with_version)
    return true;

  if (header.legacy_header.lzo_size != 0)
    return ReadVersionFromLZO(header, file);

  if (!file.ReadArray(&header.version_header, 1))
  {
    Core::DisplayMessage("Failed to read state version header", 2000);
    return false;
  }
  std::string version(header.version_header.version_string_length, '\0');
  if (!file.ReadBytes(version.data(), version.size()))
  {
    Core::DisplayMessage("Failed to read state version string", 2000);
    return false;
  }
  header.version_string = std::move(version);
  return true;
}

bool ReadLegacyHeaderOnly(const std::string& path, StateHeader& header)
{
  File::IOFile file(path, "rb");
  return ReadHeaders(header, file, false);
}

bool ReadLZ4Payload(Common::UniqueBuffer<u8>& out, u64 size, File::IOFile& file)
{
  out.reset(size);
  char* const target = reinterpret_cast<char*>(out.data());

  u64 filled = 0;
  for (;;)
  {
    s32 packed_size = 0;
    if (!file.ReadArray(&packed_size, 1))
    {
      PanicAlertFmt("Could not read state data length");
      return false;
    }
    if (packed_size <= 0)
    {
      PanicAlertFmtT("Internal LZ4 Error - Tried decompressing {0} bytes", packed_size);
      return false;
    }

    Common::UniqueBuffer<char> packed(static_cast<size_t>(packed_size));
    if (!file.ReadBytes(packed.get(), packed.size()))
    {
      PanicAlertFmt("Could not read state data");
      return false;
    }

    const int room =
        static_cast<int>(std::min<u64>(static_cast<u64>(LZ4_MAX_INPUT_SIZE), size - filled));
    const int unpacked = LZ4_decompress_safe(packed.get(), target + filled, packed_size, room);
    if (unpacked < 0)
    {
      PanicAlertFmtT("Internal LZ4 Error - decompression failed ({0}, {1}, {2})", unpacked,
                     packed_size, room);
      return false;
    }

    filled += static_cast<u64>(unpacked);
    if (filled == size)
      return true;
    if (filled > size)
    {
      PanicAlertFmtT("Internal LZ4 Error - payload size mismatch ({0} / {1}))", filled, size);
      return false;
    }
  }
}

// The state must be for the running game and of the current format version
// (the build string may differ).
bool HeadersAcceptable(const StateHeader& header)
{
  const auto& game_id = header.legacy_header.game_id;
  if (std::strncmp(SConfig::GetInstance().GetGameID().c_str(), game_id, 6) != 0)
  {
    Core::DisplayMessage(fmt::format("State belongs to a different game (ID {})",
                                     std::string_view{game_id, std::size(game_id)}),
                         2000);
    return false;
  }

  const u32 version = header.version_header.version_cookie - COOKIE_BASE;
  if (version == STATE_VERSION)
    return true;

  std::string made_by = header.version_string;
  if (const auto old = s_old_versions.find(version); old != s_old_versions.end())
    made_by = "Dolphin " + old->second.first + " - " + old->second.second;

  Core::DisplayMessage(
      made_by.empty() ? "This savestate was created using an incompatible version of Dolphin" :
                        "This savestate was created using the incompatible version " + made_by,
      OSD::Duration::NORMAL);
  return false;
}

// Reads and unpacks a state's payload. `out` is left untouched on failure.
void ReadStateFile(const std::string& path, Common::UniqueBuffer<u8>& out)
{
  File::IOFile file;
  file.Open(path, "rb");

  StateHeader header;
  if (!ReadHeaders(header, file) || !HeadersAcceptable(header))
    return;

  StateExtendedHeader extended;
  if (!file.ReadArray(&extended.base_header, 1))
  {
    PanicAlertFmt("Unable to read state header");
    return;
  }
  const StateExtendedBaseHeader& base = extended.base_header;
  if (base.header_version != EXTENDED_HEADER_VERSION)
  {
    PanicAlertFmt("State header corrupted");
    return;
  }

  Common::UniqueBuffer<u8> payload;
  if (base.compression_type == CompressionType::LZ4)
  {
    Core::DisplayMessage("Decompressing State...", OSD::Duration::SHORT);
    if (!ReadLZ4Payload(payload, base.uncompressed_size, file))
      return;
  }
  else if (base.compression_type == CompressionType::Uncompressed)
  {
    // Everything after the headers is the payload.
    const u64 headers_size = sizeof(StateHeaderLegacy) + sizeof(StateHeaderVersion) +
                             header.version_header.version_string_length +
                             sizeof(StateExtendedBaseHeader) + base.payload_offset;
    const u64 file_size = file.GetSize();
    if (file_size < headers_size)
    {
      PanicAlertFmt("State header length corrupted");
      return;
    }
    const auto size = static_cast<size_t>(file_size - headers_size);
    payload.reset(size);
    if (!file.ReadBytes(payload.data(), size))
    {
      PanicAlertFmt("Error reading bytes: {0}", size);
      return;
    }
  }
  else
  {
    PanicAlertFmt("Unknown compression type {0}", base.compression_type);
    return;
  }

  out.swap(payload);
}

// Runs on the CPU thread.
void LoadNow(Core::System& system, std::string filename)
{
  // Pending saves may be writing the very file we want.
  s_writer.WaitForCompletion();

  auto& movie = system.GetMovie();

  // Keep the current machine (and movie) so the load can be undone.
  if (!movie.IsJustStartingRecordingInputFromSaveState())
  {
    CaptureInto(system, s_undo_load_buffer);
    const std::string undo_movie = UndoMoviePath();
    if (movie.IsMovieActive())
      movie.SaveRecording(undo_movie);
    else if (File::Exists(undo_movie))
      File::Delete(undo_movie);
  }

  std::optional<bool> outcome;  // empty when the file could not be read
  {
    // Scoped so the payload is freed before anything else happens.
    Common::UniqueBuffer<u8> payload;
    ReadStateFile(filename, payload);
    if (!payload.empty())
      outcome = RestoreFrom(system, payload);
  }

  if (outcome == true)
  {
    Core::DisplayMessage(fmt::format("Loaded State from {}", FileNameOnly(filename)), 2000);
    const std::string state_movie = filename + ".dtm";
    if (File::Exists(state_movie))
    {
      movie.LoadInput(state_movie);
    }
    else if (!movie.IsJustStartingRecordingInputFromSaveState() &&
             !movie.IsJustStartingPlayingInputFromSaveState())
    {
      movie.EndPlayInput(false);
    }
  }
  else if (outcome == false)
  {
    Core::DisplayMessage("The savestate could not be loaded", OSD::Duration::NORMAL);
    // A half-applied state may be inconsistent; go back to where we were.
    UndoLoadState(system);
  }

  if (s_after_load)
    s_after_load();
}
}  // namespace

std::string GetInfoStringOfSlot(u32 slot, bool translate)
{
  // Wait for pending saves so the answer reflects them.
  std::lock_guard wait_for_saves{s_save_tasks};

  const auto label = [translate](const char* text) {
    return translate ? Common::GetStringT(text) : std::string(text);
  };

  const std::string path = SlotPath(slot);
  if (!File::Exists(path))
    return label("Empty");

  StateHeader header;
  if (!ReadLegacyHeaderOnly(path, header))
    return label("Unknown");
  return HeaderTimeToText(header.legacy_header.time);
}

u64 GetUnixTimeOfSlot(u32 slot)
{
  std::lock_guard wait_for_saves{s_save_tasks};

  StateHeader header;
  if (!ReadLegacyHeaderOnly(SlotPath(slot), header))
    return 0;

  constexpr u64 MS = 1000;
  constexpr u64 SHIFT_MS = static_cast<u64>(TIME_EPOCH_SHIFT) * MS;
  return static_cast<u64>(header.legacy_header.time * MS) + SHIFT_MS;
}

void SaveAs(Core::System& system, std::string filename)
{
  Core::RunOnCPUThread(system, [&system, path = std::move(filename), lock = HoldSaveTask()]() mutable {
    CaptureAndQueue(system, std::move(path));
  });
}

void LoadAs(Core::System& system, std::string filename)
{
  if (!LoadingAllowed(system))
    return;

  Core::RunOnCPUThread(system, [&system, path = std::move(filename)]() mutable {
    LoadNow(system, std::move(path));
  });
}

void SetOnAfterLoadCallback(AfterLoadCallbackFunc callback)
{
  s_after_load = std::move(callback);
}

void Init(Core::System& system)
{
  s_writer.Reset("Savestate Worker", [&system](const PendingWrite& job) {
    WriteStateFile(system, job);
  });

  // Flushing unsaved data means waiting until no save is pending.
  s_flush_hook = UICommon::AddFlushUnsavedDataCallback([] { std::lock_guard wait{s_save_tasks}; });
}

void Shutdown()
{
  s_writer.Shutdown();
  s_undo_load_buffer.reset();
  s_flush_hook.reset();
}

void Save(Core::System& system, u32 slot)
{
  SaveAs(system, SlotPath(slot));
}

void Load(Core::System& system, u32 slot)
{
  LoadAs(system, SlotPath(slot));
}

void LoadLastSaved(Core::System& system, int i)
{
  if (!LoadingAllowed(system))
    return;

  Core::RunOnCPUThread(system, [&system, i] {
    // The slot list must include saves still being written.
    s_writer.WaitForCompletion();

    std::vector<UsedSlot> used = FindUsedSlots();
    // `i` indexes the list directly. An index equal to the list size used to
    // read past its end; it is now refused like any larger one.
    if (i < 0 || static_cast<size_t>(i) >= used.size())
    {
      Core::DisplayMessage("State doesn't exist", 2000);
      return;
    }

    std::ranges::stable_sort(used, std::ranges::greater{}, &UsedSlot::time);
    LoadNow(system, SlotPath(used[static_cast<size_t>(i)].slot));
  });
}

void SaveFirstSaved(Core::System& system)
{
  Core::RunOnCPUThread(system, [&system, lock = HoldSaveTask()] {
    s_writer.WaitForCompletion();

    std::vector<UsedSlot> used = FindUsedSlots();
    std::optional<u32> slot = FirstFreeSlot(used);
    if (!slot)
    {
      // Every slot is taken: replace the oldest.
      std::ranges::stable_sort(used, std::ranges::less{}, &UsedSlot::time);
      slot = used.front().slot;
    }
    CaptureAndQueue(system, SlotPath(*slot));
  });
}

void UndoLoadState(Core::System& system)
{
  if (!LoadingAllowed(system))
    return;

  Core::RunOnCPUThread(system, [&system] {
    if (s_undo_load_buffer.empty())
    {
      PanicAlertFmtT("There is nothing to undo!");
      return;
    }

    auto& movie = system.GetMovie();
    if (!movie.IsMovieActive())
    {
      RestoreFrom(system, s_undo_load_buffer);
      return;
    }

    // During a movie the matching movie must come back too. Only the CPU
    // thread writes undo.dtm.
    const std::string undo_movie = UndoMoviePath();
    if (!File::Exists(undo_movie))
    {
      PanicAlertFmtT("No undo.dtm found, aborting undo load state to prevent movie desyncs");
      return;
    }
    RestoreFrom(system, s_undo_load_buffer);
    movie.LoadInput(undo_movie);
  });
}

void UndoSaveState(Core::System& system)
{
  LoadAs(system, StateDir() + "lastState.sav");
}

}  // namespace State
