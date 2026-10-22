// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#ifdef HAS_LIBMGBA

#include "Core/HW/GBACore.h"

#include <mgba-util/vfs.h>
#include <mgba/core/log.h>
#include <mgba/core/timing.h>
#include <mgba/internal/gb/gb.h>
#include <mgba/internal/gba/gba.h>

#include <mz.h>
#include <mz_strm.h>
#include <mz_zip.h>
#include <mz_zip_rw.h>

#include "AudioCommon/AudioCommon.h"

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/Crypto/SHA1.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Common/MinizipUtil.h"
#include "Common/ScopeGuard.h"

#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/HW/SystemTimers.h"
#include "Core/Host.h"
#include "Core/NetPlayProto.h"
#include "Core/System.h"

#ifdef ANDROID
#include "jni/AndroidCommon/AndroidCommon.h"
#endif

namespace HW::GBA
{
namespace
{
// mGBA's own logging is dropped.
mLogger s_quiet_logger = {[](mLogger*, int, mLogLevel, const char*, va_list) {}, nullptr};

// Samples (not frames) pulled from mGBA per read.
constexpr std::size_t MIXER_CHUNK = 512;

// Where a GB picture sits inside the GBA-sized frame the Game Boy Player
// shows: centred, 40 pixels in and 8 lines down.
constexpr std::size_t GB_FRAME_OFFSET = 8 * GBA_VIDEO_HORIZONTAL_PIXELS + 40;

// mGBA misreports the clock of some GB models, so use the known rates.
u32 ClockRate(mCore* core)
{
  if (core->platform(core) == mPLATFORM_GBA)
    return GBA_ARM7TDMI_FREQUENCY;
  return CGB_SM83_FREQUENCY;
}

bool IsRom(VFile* file)
{
  return mCoreIsCompatible(file) != mPLATFORM_NONE;
}

// Copies the whole of `source` into a new in-memory file.
VFile* CopyToMemory(VFile* source)
{
  const auto length = static_cast<std::size_t>(source->size(source));
  std::vector<u8> bytes(length);
  source->seek(source, 0, SEEK_SET);
  source->read(source, bytes.data(), length);
  source->close(source);
  return VFileMemChunk(bytes.data(), length);
}

// Archive formats mGBA understands itself (7z and friends).
VFile* RomFromArchive(const char* path)
{
  VDir* const dir = VDirOpenArchive(path);
  if (dir == nullptr)
    return nullptr;

  VFile* result = nullptr;
  if (VFile* const entry = VDirFindFirst(dir, IsRom))
    result = CopyToMemory(entry);
  dir->close(dir);
  return result;
}

// Zip files, read through minizip: the first entry mGBA accepts.
VFile* RomFromZip(const char* path)
{
  void* reader = mz_zip_reader_create();
  if (reader == nullptr)
    return nullptr;
  Common::ScopeGuard release{[&] { mz_zip_reader_delete(&reader); }};

  if (mz_zip_reader_open_file(reader, path) != MZ_OK)
    return nullptr;

  for (;;)
  {
    mz_zip_file* entry = nullptr;
    if (mz_zip_reader_entry_get_info(reader, &entry) == MZ_OK && entry->uncompressed_size != 0)
    {
      const auto length = static_cast<std::size_t>(entry->uncompressed_size);
      std::vector<u8> bytes(length);
      if (Common::ReadFileFromZip(reader, &bytes))
      {
        VFile* const candidate = VFileMemChunk(bytes.data(), length);
        if (IsRom(candidate))
        {
          candidate->seek(candidate, 0, SEEK_SET);
          return candidate;
        }
        candidate->close(candidate);
      }
    }
    if (mz_zip_reader_goto_next_entry(reader) == MZ_END_OF_LIST)
      return nullptr;
  }
}

VFile* OpenForReading(const char* path)
{
#ifdef ANDROID
  if (IsPathAndroidContent(path))
    return VFileFromFD(OpenAndroidContent(path, "r"));
#endif
  return VFileOpen(path, O_RDONLY);
}

// Tries an archive, then a zip, then the file itself. Returns a file mGBA can
// load, rewound, or null.
VFile* OpenRom(const char* path)
{
  VFile* rom = RomFromArchive(path);
  if (rom == nullptr)
    rom = RomFromZip(path);
  if (rom == nullptr)
    rom = OpenForReading(path);
  if (rom == nullptr)
    return nullptr;

  if (!IsRom(rom))
  {
    rom->close(rom);
    return nullptr;
  }
  rom->seek(rom, 0, SEEK_SET);
  return rom;
}

std::array<u8, 20> HashRom(VFile* rom)
{
  const auto length = static_cast<std::size_t>(rom->size(rom));
  void* const mapped = rom->map(rom, length, MAP_READ);
  const auto digest = Common::SHA1::CalculateDigest(static_cast<const u8*>(mapped), length);
  rom->unmap(rom, mapped, length);
  return digest;
}

// mGBA settings applied to every core, in this order.
struct CoreSetting
{
  const char* key;
  const char* text;  // null: use `number`
  int number;
};
// Our GB defaults: original DMG hardware, colour models for GBC games.
constexpr CoreSetting CORE_SETTINGS[] = {
    {"idleOptimization", "detect", 0},
    {"useBios", nullptr, 0},
    {"skipBios", nullptr, 0},
    {"gb.model", "DMG", 0},
    {"sgb.model", "DMG", 0},
    {"cgb.model", "CGB", 0},
    {"cgb.hybridModel", "CGB", 0},
    {"cgb.sgbModel", "CGB", 0},
    {"gb.colors", nullptr, GB_COLORS_CGB},
};

void DrainToMixer(mAudioBuffer* source, Mixer* mixer, std::size_t device)
{
  std::array<s16, MIXER_CHUNK> chunk;
  const std::size_t frames_per_read = chunk.size() / source->channels;
  for (;;)
  {
    const std::size_t frames = mAudioBufferRead(source, chunk.data(), frames_per_read);
    if (frames == 0)
      return;
    mixer->PushGBASamples(device, chunk.data(), frames);
  }
}

Core& OwnerOf(GBASIODriver* driver)
{
  return *static_cast<SIODriver*>(driver)->core;
}

AVStream& StreamOf(mAVStream* stream)
{
  return *static_cast<AVStream*>(stream);
}
}  // namespace

Core::Core(::Core::System& system, int device_number)
    : m_device_number(device_number), m_system(system)
{
  mLogSetDefaultLogger(&s_quiet_logger);

  MutexInit(&m_sync.videoFrameMutex);
  ConditionInit(&m_sync.videoFrameAvailableCond);
  ConditionInit(&m_sync.videoFrameRequiredCond);
  ConditionInit(&m_sync.audioRequiredCond);
  MutexInit(&m_sync.audioBufferMutex);
}

Core::~Core()
{
  Stop();

  MutexDeinit(&m_sync.videoFrameMutex);
  ConditionDeinit(&m_sync.videoFrameAvailableCond);
  ConditionDeinit(&m_sync.videoFrameRequiredCond);
  ConditionDeinit(&m_sync.audioRequiredCond);
  MutexDeinit(&m_sync.audioBufferMutex);
}

bool Core::Start(u64 gc_ticks)
{
  if (m_started)
    return false;

  // Any early return below leaves the core stopped.
  Common::ScopeGuard undo{[this] { Stop(); }};

  VFile* rom = nullptr;
  Common::ScopeGuard close_rom{[&rom] {
    if (rom != nullptr)
      rom->close(rom);
  }};

  const int gba_number = m_device_number + 1;
  m_rom_path = Config::Get(Config::MAIN_GBA_ROM_PATHS[static_cast<std::size_t>(m_device_number)]);
  if (!m_rom_path.empty())
  {
    rom = OpenRom(m_rom_path.c_str());
    if (rom == nullptr)
    {
      PanicAlertFmtT("Error: GBA{0} failed to open the ROM in {1}", gba_number, m_rom_path);
      return false;
    }
    m_rom_hash = HashRom(rom);
  }

  m_mcore = rom != nullptr ? mCoreFindVF(rom) : mCoreCreate(mPLATFORM_GBA);
  if (m_mcore == nullptr)
  {
    PanicAlertFmtT("Error: GBA{0} failed to create core", gba_number);
    return false;
  }
  m_mcore->init(m_mcore);

  mCoreInitConfig(m_mcore, "dolphin");
  for (const CoreSetting& setting : CORE_SETTINGS)
  {
    if (setting.text != nullptr)
      mCoreConfigSetValue(&m_mcore->config, setting.key, setting.text);
    else
      mCoreConfigSetIntValue(&m_mcore->config, setting.key, setting.number);
  }

  // Without a BIOS file mGBA uses its built-in replacement.
  if (m_mcore->platform(m_mcore) == mPLATFORM_GBA)
    OpenBios(File::GetUserPath(F_GBABIOS_IDX).c_str());

  if (rom != nullptr)
  {
    if (!m_mcore->loadROM(m_mcore, rom))
    {
      PanicAlertFmtT("Error: GBA{0} failed to load the ROM in {1}", gba_number, m_rom_path);
      return false;
    }
    close_rom.Dismiss();  // the core owns it now

    mGameInfo game;
    m_mcore->getGameInfo(m_mcore, &game);
    m_game_title = game.title;

    if (NetPlay::IsNetPlayRunning())
      m_save_path = NetPlay::GetGBASavePath(m_device_number);
    else
      m_save_path = GetSavePath(m_rom_path, m_device_number);
    if (!m_save_path.empty() && !OpenSave(m_save_path.c_str()))
      return false;
  }

  m_gc_time = gc_ticks;
  m_gc_time_frac = 0;
  m_keys = 0;

  // The Game Boy Player instance has no link cable and feeds its own sound
  // path.
  if (m_device_number != Config::GBPLAYER_GBA_INDEX)
  {
    InstallLinkDriver();
    InstallStream();
  }

  AttachFrameBuffer();
  m_mcore->setAudioBufferSize(m_mcore, MIXER_CHUNK);
  InstallCallbacks();
  PrepareSyncEvent();
  m_mcore->setSync(m_mcore, &m_sync);

  m_mcore->reset(m_mcore);
  m_started = true;
  undo.Dismiss();

  // reset() may have changed the picture size; this also tells the host.
  AttachFrameBuffer();

  m_worker.Reset(fmt::format("GBA{}", gba_number), [this](Job job) { RunJob(job); });
  return true;
}

void Core::Stop()
{
  m_worker.Shutdown();

  if (m_mcore != nullptr)
  {
    mCoreConfigDeinit(&m_mcore->config);
    m_mcore->deinit(m_mcore);
    m_mcore = nullptr;
  }
  m_started = false;
  m_rom_path.clear();
  m_save_path.clear();
  m_rom_hash.fill(0);
  m_game_title.clear();
}

void Core::Reset()
{
  Flush();
  if (m_started)
    m_mcore->reset(m_mcore);
}

bool Core::IsStarted() const
{
  return m_started;
}

CoreInfo Core::GetCoreInfo() const
{
  CoreInfo info{};
  info.device_number = m_device_number;
  info.width = GBA_VIDEO_HORIZONTAL_PIXELS;
  info.height = GBA_VIDEO_VERTICAL_PIXELS;
  if (!m_started)
    return info;

  info.is_gba = m_mcore->platform(m_mcore) == mPLATFORM_GBA;
  info.has_rom = !m_rom_path.empty();
  if (info.is_gba)
  {
    const auto* const board = static_cast<const ::GBA*>(m_mcore->board);
    info.has_ereader = (board->memory.hw.devices & HW_EREADER) != 0;
  }
  m_mcore->currentVideoSize(m_mcore, &info.width, &info.height);
  info.game_title = m_game_title;
  return info;
}

void Core::SetHost(std::weak_ptr<GBAHostInterface> host)
{
  m_host = std::move(host);
}

void Core::SetForceDisconnect(bool force_disconnect)
{
  m_unplugged = force_disconnect;
}

void Core::EReaderQueueCard(std::string_view card_path)
{
  Flush();
  if (!GetCoreInfo().has_ereader)
    return;

  File::IOFile card(std::string(card_path), "rb");
  std::vector<u8> dots(card.GetSize());
  card.ReadBytes(dots.data(), dots.size());
  GBACartEReaderQueueCard(static_cast<::GBA*>(m_mcore->board), dots.data(), dots.size());
}

bool Core::OpenBios(const char* bios_path)
{
  VFile* const bios = OpenForReading(bios_path);
  if (bios == nullptr)
  {
    ERROR_LOG_FMT(CORE, "GBA{0} failed to open the BIOS in {1}", m_device_number + 1, bios_path);
    return false;
  }
  if (m_mcore->loadBIOS(m_mcore, bios, 0))
    return true;

  ERROR_LOG_FMT(CORE, "GBA{0} failed to load the BIOS in {1}", m_device_number + 1, bios_path);
  bios->close(bios);
  return false;
}

bool Core::OpenSave(const char* save_path)
{
  VFile* const save = VFileOpen(save_path, O_CREAT | O_RDWR);
  if (save == nullptr)
  {
    PanicAlertFmtT("Error: GBA{0} failed to open the save in {1}", m_device_number + 1, save_path);
    return false;
  }
  if (m_mcore->loadSave(m_mcore, save))
    return true;

  PanicAlertFmtT("Error: GBA{0} failed to load the save in {1}", m_device_number + 1, save_path);
  save->close(save);
  return false;
}

void Core::InstallLinkDriver()
{
  if (m_mcore->platform(m_mcore) != mPLATFORM_GBA)
    return;

  // Only the joybus mode is offered; the link is "on" while the game has
  // selected it.
  m_link.core = this;
  m_link.init = [](GBASIODriver*) { return true; };
  m_link.deinit = [](GBASIODriver* driver) { OwnerOf(driver).m_joybus_mode = false; };
  m_link.reset = [](GBASIODriver* driver) { OwnerOf(driver).m_joybus_mode = false; };
  m_link.setMode = [](GBASIODriver* driver, GBASIOMode mode) {
    OwnerOf(driver).m_joybus_mode = (mode == GBA_SIO_JOYBUS);
  };
  m_link.handlesMode = [](GBASIODriver*, GBASIOMode mode) { return mode == GBA_SIO_JOYBUS; };
  m_link.connectedDevices = [](GBASIODriver*) { return 1; };

  GBASIOSetDriver(&static_cast<::GBA*>(m_mcore->board)->sio, &m_link);
}

void Core::AttachFrameBuffer()
{
  if (m_device_number != Config::GBPLAYER_GBA_INDEX)
  {
    u32 width;
    u32 height;
    m_mcore->currentVideoSize(m_mcore, &width, &height);
    m_frame.resize(std::size_t{width} * height);
    m_mcore->setVideoBuffer(m_mcore, m_frame.data(), width);
  }
  else
  {
    // The Game Boy Player always reads a full GBA frame. Start from black so
    // a GB game does not show leftovers of a GBA one around its picture.
    m_frame.assign(std::size_t{GBA_VIDEO_HORIZONTAL_PIXELS} * GBA_VIDEO_VERTICAL_PIXELS, 0);
    const std::size_t offset = GetPlatform() == mPLATFORM_GBA ? 0 : GB_FRAME_OFFSET;
    m_mcore->setVideoBuffer(m_mcore, m_frame.data() + offset, GBA_VIDEO_HORIZONTAL_PIXELS);
  }

  if (const auto host = m_host.lock())
    host->GameChanged();
}

void Core::InstallCallbacks()
{
  mCoreCallbacks hooks{};
  hooks.context = this;
  hooks.keysRead = [](void* context) {
    Core& self = *static_cast<Core*>(context);
    self.m_mcore->setKeys(self.m_mcore, self.m_keys);
  };
  hooks.videoFrameEnded = [](void* context) {
    Core& self = *static_cast<Core*>(context);
    if (const auto host = self.m_host.lock())
      host->FrameEnded(self.m_frame);
  };
  m_mcore->addCoreCallbacks(m_mcore, &hooks);
}

void Core::InstallStream()
{
  m_av = AVStream{};
  m_av.core = this;
  m_av.mixer = m_system.GetSoundStream()->GetMixer();

  m_av.videoDimensionsChanged = [](mAVStream* stream, unsigned, unsigned) {
    StreamOf(stream).core->AttachFrameBuffer();
  };
  m_av.audioRateChanged = [](mAVStream* stream, unsigned rate) {
    AVStream& av = StreamOf(stream);
    const auto device = static_cast<std::size_t>(av.core->m_device_number);
    // Samples made at the old rate go out first.
    DrainToMixer(av.core->GetAudioBuffer(), av.mixer, device);
    av.mixer->SetGBAInputSampleRate(device, rate);
  };
  m_av.postAudioBuffer = [](mAVStream* stream, mAudioBuffer* buffer) {
    AVStream& av = StreamOf(stream);
    DrainToMixer(buffer, av.mixer, static_cast<std::size_t>(av.core->m_device_number));
  };

  m_mcore->setAVStream(m_mcore, &m_av);
}

void Core::PrepareSyncEvent()
{
  // Scheduled in mGBA's timeline to mark where a run should stop.
  m_sync_event.context = this;
  m_sync_event.name = "Dolphin Sync";
  m_sync_event.callback = [](mTiming*, void* context, u32) {
    static_cast<Core*>(context)->m_awaiting_sync = false;
  };
  m_sync_event.priority = 0x80;
}

void Core::SyncJoybus(u64 gc_ticks, u16 keys)
{
  Job job;
  job.keys = keys;
  job.until_gc_ticks = gc_ticks;
  m_worker.Push(job);
}

void Core::SendJoybusCommand(u64 gc_ticks, int transfer_time, u8* buffer, u16 keys)
{
  if (!m_started)
    return;

  m_cmd_transfer_time = transfer_time;
  m_cmd = static_cast<GBASIOJOYCommand>(buffer[0]);
  std::copy(buffer + 1, buffer + 1 + m_cmd_data.size(), m_cmd_data.begin());
  m_cmd_busy.store(true, std::memory_order_relaxed);

  Job job;
  job.joybus_command = true;
  job.keys = keys;
  job.until_gc_ticks = gc_ticks;
  m_worker.Push(job);
}

int Core::GetJoybusResponse(u8* data_out)
{
  m_cmd_busy.wait(true, std::memory_order_acquire);
  std::copy_n(m_cmd_data.begin(), m_reply_size, data_out);
  return m_reply_size;
}

void Core::Flush()
{
  m_worker.WaitForCompletion();
}

void Core::RunJob(const Job& job)
{
  m_keys = job.keys;
  AdvanceTo(job.until_gc_ticks);
  if (!job.joybus_command)
    return;

  // An unplugged or non-joybus GBA gives no reply.
  const bool connected = m_joybus_mode && !m_unplugged;
  m_reply_size =
      connected ? static_cast<u8>(GBASIOJOYSendCommand(&m_link, m_cmd, m_cmd_data.data())) : 0;

  m_cmd_busy.store(false, std::memory_order_release);
  m_cmd_busy.notify_one();

  // Let the GBA run on while the reply is still on the wire.
  AdvanceTo(m_gc_time + static_cast<u64>(m_cmd_transfer_time));
}

void Core::AdvanceTo(u64 gc_ticks)
{
  if (static_cast<s64>(gc_ticks - m_gc_time) <= 0)
    return;

  const u64 gc_rate = m_system.GetSystemTimers().GetTicksPerSecond();
  const u32 gba_rate = ClockRate(m_mcore);

  // Run mGBA until the sync event fires...
  const u64 gba_cycles = (gc_ticks - m_gc_time) * gba_rate / gc_rate;
  mTimingSchedule(m_mcore->timing, &m_sync_event, static_cast<s32>(gba_cycles));
  m_awaiting_sync = true;
  const s32 started_at = mTimingCurrentTime(m_mcore->timing);
  while (m_awaiting_sync)
    m_mcore->runLoop(m_mcore);
  const s32 stopped_at = mTimingCurrentTime(m_mcore->timing);

  // ...then move our clock by what it actually ran, keeping the remainder.
  const u64 scaled = static_cast<u64>(stopped_at - started_at) * gc_rate + m_gc_time_frac;
  m_gc_time += scaled / gba_rate;
  m_gc_time_frac = scaled % gba_rate;
}

void Core::ImportState(std::string_view state_path)
{
  Flush();
  if (!m_started)
    return;

  std::vector<u8> snapshot(m_mcore->stateSize(m_mcore));
  File::IOFile file(std::string(state_path), "rb");
  if (file.GetSize() != snapshot.size())
    return;
  file.ReadBytes(snapshot.data(), snapshot.size());
  m_mcore->loadState(m_mcore, snapshot.data());
}

void Core::ExportState(std::string_view state_path)
{
  Flush();
  if (!m_started)
    return;

  std::vector<u8> snapshot(m_mcore->stateSize(m_mcore));
  m_mcore->saveState(m_mcore, snapshot.data());
  File::IOFile(std::string(state_path), "wb").WriteBytes(snapshot.data(), snapshot.size());
}

void Core::ImportSave(std::string_view save_path)
{
  Flush();
  if (!m_started)
    return;

  File::IOFile file(std::string(save_path), "rb");
  std::vector<u8> contents(file.GetSize());
  if (!file.ReadBytes(contents.data(), contents.size()))
    return;
  m_mcore->savedataRestore(m_mcore, contents.data(), contents.size(), true);
  m_mcore->reset(m_mcore);
}

void Core::ExportSave(std::string_view save_path)
{
  Flush();
  if (!m_started)
    return;

  File::IOFile file(std::string(save_path), "wb");
  void* contents = nullptr;
  const std::size_t length = m_mcore->savedataClone(m_mcore, &contents);
  if (contents == nullptr)
    return;
  file.WriteBytes(contents, length);
  free(contents);
}

void Core::DoState(PointerWrap& p)
{
  Flush();

  bool running = m_started;
  p.Do(running);
  if (!running)
  {
    Stop();
    return;
  }

  // Identify the cartridge so a state is never loaded onto another game.
  bool had_rom = !m_rom_path.empty();
  p.Do(had_rom);
  const std::array<u8, 20> hash_before = m_rom_hash;
  p.Do(m_rom_hash);
  const std::string title_before = m_game_title;
  p.Do(m_game_title);

  if (p.IsReadMode())
  {
    const bool have_rom = !m_rom_path.empty();
    const bool same_game =
        had_rom == have_rom &&
        (!had_rom || (hash_before == m_rom_hash && title_before == m_game_title));
    if (!same_game)
    {
      ::Core::DisplayMessage(
          fmt::format("Incompatible ROM state in GBA{}. Aborting load state.", m_device_number + 1),
          3000);
      p.SetVerifyMode();
      return;
    }
  }

  // Loading into a stopped GBA boots it first.
  Start(0);

  p.Do(m_frame);
  p.Do(m_gc_time);
  p.Do(m_gc_time_frac);
  p.Do(m_keys);
  p.Do(m_joybus_mode);
  p.Do(m_reply_size);
  p.Do(m_cmd_data);

  // mGBA's own snapshot, as a sized blob.
  const std::size_t snapshot_size = m_mcore->stateSize(m_mcore);
  std::vector<u8> snapshot(snapshot_size);
  if (p.IsWriteMode() || p.IsVerifyMode())
    m_mcore->saveState(m_mcore, snapshot.data());

  p.Do(snapshot);

  if (p.IsReadMode() && snapshot.size() == m_mcore->stateSize(m_mcore))
  {
    m_mcore->loadState(m_mcore, snapshot.data());
    if (const auto host = m_host.lock())
      host->FrameEnded(m_frame);
  }
}

bool Core::GetRomInfo(const char* rom_path, std::array<u8, 20>& hash, std::string& title)
{
  VFile* const rom = OpenRom(rom_path);
  if (rom == nullptr)
    return false;

  hash = HashRom(rom);

  mCore* const probe = mCoreFindVF(rom);
  if (probe == nullptr)
  {
    rom->close(rom);
    return false;
  }
  probe->init(probe);
  if (!probe->loadROM(probe, rom))
  {
    rom->close(rom);
    return false;
  }

  mGameInfo game;
  probe->getGameInfo(probe, &game);
  title = game.title;

  // Deinit also releases the ROM file the core took over.
  probe->deinit(probe);
  return true;
}

std::string Core::GetSavePath(std::string_view rom_path, int device_number)
{
  // "<rom without extension>-<n>.sav", next to the ROM or in the saves
  // folder.
  const std::string_view stem = rom_path.substr(0, rom_path.find_last_of('.'));
  std::string path = fmt::format("{}-{}.sav", stem, device_number + 1);

  if (Config::Get(Config::MAIN_GBA_SAVES_IN_ROM_PATH))
    return path;

  const std::size_t name_start = path.find_last_of("\\/") + 1;
  return File::GetUserPath(D_GBASAVES_IDX) + path.substr(name_start);
}
}  // namespace HW::GBA
#endif  // HAS_LIBMGBA
