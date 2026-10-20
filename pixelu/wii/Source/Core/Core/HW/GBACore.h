// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#ifdef HAS_LIBMGBA

#include <array>
#include <atomic>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <mgba/core/core.h>
#include <mgba/core/interface.h>
#if !defined(_WIN32)
#define USE_PTHREADS  // mCoreSync's mutex/condition types need this outside Windows.
#endif
#include <mgba/core/sync.h>
#undef USE_PTHREADS
#include <mgba/gba/interface.h>

#include "Common/CommonTypes.h"
#include "Common/WorkQueueThread.h"

class GBAHostInterface;
class Mixer;
class PointerWrap;

namespace Core
{
class System;
}

namespace HW::GBA
{

class Core;

// mGBA driver/stream structs with a way back to the owning Core.
struct SIODriver : GBASIODriver
{
  Core* core;
};

struct AVStream : mAVStream
{
  Core* core;
  Mixer* mixer;
};

struct CoreInfo
{
  int device_number;
  bool is_gba;
  bool has_rom;
  bool has_ereader;
  u32 width;
  u32 height;
  std::string game_title;
};

// One emulated GBA (or GB) running in mGBA on its own thread, kept in step
// with the GameCube's clock through joybus syncs.
class Core final
{
public:
  explicit Core(::Core::System& system, int device_number);
  Core(const Core&) = delete;
  Core(Core&&) = delete;
  Core& operator=(const Core&) = delete;
  Core& operator=(Core&&) = delete;
  ~Core();

  bool Start(u64 gc_ticks);
  void Stop();
  void Reset();
  bool IsStarted() const;
  CoreInfo GetCoreInfo() const;

  void SetHost(std::weak_ptr<GBAHostInterface> host);
  void SetForceDisconnect(bool force_disconnect);
  void EReaderQueueCard(std::string_view card_path);

  void SyncJoybus(u64 gc_ticks, u16 keys);
  void SendJoybusCommand(u64 gc_ticks, int transfer_time, u8* buffer, u16 keys);
  int GetJoybusResponse(u8* data_out);

  // Blocks until the GBA thread has finished everything queued so far.
  void Flush();

  mCore* GetCore() { return m_mcore; }

  mAudioBuffer* GetAudioBuffer() { return m_mcore->getAudioBuffer(m_mcore); }
  std::span<const u32> GetVideoBuffer() const { return m_frame; }

  mPlatform GetPlatform() const { return m_mcore->platform(m_mcore); }
  u32 GetAudioSampleRate() const { return m_mcore->audioSampleRate(m_mcore); }

  void ImportState(std::string_view state_path);
  void ExportState(std::string_view state_path);
  void ImportSave(std::string_view save_path);
  void ExportSave(std::string_view save_path);
  void DoState(PointerWrap& p);

  static bool GetRomInfo(const char* rom_path, std::array<u8, 20>& hash, std::string& title);
  static std::string GetSavePath(std::string_view rom_path, int device_number);

private:
  // Work handed to the GBA thread.
  struct Job
  {
    bool joybus_command = false;  // otherwise a plain clock sync
    u16 keys = 0;
    u64 until_gc_ticks = 0;
  };
  void RunJob(const Job& job);
  void AdvanceTo(u64 gc_ticks);

  bool OpenBios(const char* bios_path);
  bool OpenSave(const char* save_path);

  void InstallLinkDriver();
  void InstallStream();
  void InstallCallbacks();
  void PrepareSyncEvent();
  void AttachFrameBuffer();

  const int m_device_number;

  bool m_started = false;
  std::string m_rom_path;
  std::string m_save_path;
  std::array<u8, 20> m_rom_hash{};
  std::string m_game_title;

  mCore* m_mcore{};
  mCoreSync m_sync{};
  mTimingEvent m_sync_event{};
  bool m_awaiting_sync = false;
  SIODriver m_link{};
  AVStream m_av{};
  std::vector<u32> m_frame;

  // GameCube time the GBA has caught up to, plus the sub-tick leftover.
  u64 m_gc_time = 0;
  u64 m_gc_time_frac = 0;
  u16 m_keys = 0;
  bool m_joybus_mode = false;
  bool m_unplugged = false;

  std::weak_ptr<GBAHostInterface> m_host;

  // A joybus command: written by the GC thread, then run on the GBA thread,
  // which puts the reply into m_cmd_data and clears m_cmd_busy.
  int m_cmd_transfer_time{};
  GBASIOJOYCommand m_cmd{};
  std::array<u8, 5> m_cmd_data{};
  u8 m_reply_size{};
  std::atomic_bool m_cmd_busy{};

  Common::WorkQueueThreadSP<Job> m_worker;

  ::Core::System& m_system;
};

}  // namespace HW::GBA

#endif  // HAS_LIBMGBA
