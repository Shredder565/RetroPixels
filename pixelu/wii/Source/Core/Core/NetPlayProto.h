// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <string>

#include "Common/CommonTypes.h"
#include "Common/EnumMap.h"
#include "Core/Config/SYSCONFSettings.h"
#include "Core/HW/EXI/EXI.h"
#include "Core/HW/EXI/EXI_Device.h"
#include "Core/HW/Sram.h"
#include "VideoCommon/VideoConfig.h"

namespace DiscIO
{
enum class Region;
}
namespace IOS::HLE::FS
{
class FileSystem;
}
namespace PowerPC
{
enum class CPUCore;
}

namespace NetPlay
{
// What the host knows about the GBA in one controller port.
struct GBAConfig
{
  bool enabled = false;
  bool has_rom = false;
  std::string title;
  std::array<u8, 20> hash{};
};

using PlayerId = u8;
using FrameNum = u32;
using PadIndex = s8;
using PadMappingArray = std::array<PlayerId, 4>;
using GBAConfigArray = std::array<GBAConfig, 4>;

// The settings every player runs a NetPlay session with. The host fills them from its own
// configuration and sends them in the StartGame message (see VisitStartGameSettings).
struct NetSettings
{
  bool cpu_thread = false;
  PowerPC::CPUCore cpu_core{};
  bool enable_cheats = false;
  bool enable_hardcore = false;
  int selected_language = 0;
  bool override_region_settings = false;
  bool dsp_hle = false;
  bool dsp_enable_jit = false;
  bool ram_override_enable = false;
  u32 mem1_size = 0;
  u32 mem2_size = 0;
  DiscIO::Region fallback_region{};
  bool allow_sd_writes = false;
  bool oc_enable = false;
  float oc_factor = 0;
  bool vi_oc_enable = false;
  float vi_oc_factor = 0;
  Common::EnumMap<ExpansionInterface::EXIDeviceType, ExpansionInterface::MAX_SLOT> exi_device{};
  int memcard_size_override = -1;

  std::array<u32, Config::SYSCONF_SETTINGS.size()> sysconf_settings{};

  bool efb_access_enable = false;
  bool bbox_enable = false;
  bool force_progressive = false;
  bool efb_to_texture_enable = false;
  bool xfb_to_texture_enable = false;
  bool disable_copy_to_vram = false;
  bool immediate_xfb_enable = false;
  bool efb_emulate_format_changes = false;
  int safe_texture_cache_color_samples = 0;
  bool perf_queries_enable = false;
  bool float_exceptions = false;
  bool divide_by_zero_exceptions = false;
  bool fprf = false;
  bool accurate_nans = false;
  bool accurate_fmadds = false;
  bool disable_icache = false;
  bool sync_on_skip_idle = false;
  bool sync_gpu = false;
  int sync_gpu_max_distance = 0;
  int sync_gpu_min_distance = 0;
  float sync_gpu_overclock = 0;
  bool jit_follow_branch = false;
  bool fast_disc_speed = false;
  bool mmu = false;
  bool fastmem = false;
  bool skip_ipl = false;
  bool load_ipl_dump = false;
  bool vertex_rounding = false;
  int internal_resolution = 0;
  bool efb_scaled_copy = false;
  bool fast_depth_calc = false;
  bool enable_pixel_lighting = false;
  bool widescreen_hack = false;
  TextureFilteringMode force_texture_filtering = TextureFilteringMode::Default;
  AnisotropicFilteringMode max_anisotropy = AnisotropicFilteringMode::Default;
  bool force_true_color = false;
  bool disable_copy_filter = false;
  bool disable_fog = false;
  bool arbitrary_mipmap_detection = false;
  float arbitrary_mipmap_detection_threshold = 0;
  bool enable_gpu_texture_decoding = false;
  bool defer_efb_copies = false;
  int efb_access_tile_size = 0;
  bool efb_access_defer_invalidation = false;

  bool savedata_load = false;
  bool savedata_write = false;
  bool savedata_sync_all_wii = false;

  bool strict_settings_sync = false;
  bool sync_codes = false;
  std::string save_data_region;
  bool golf_mode = false;
  bool use_fma = false;
  bool hide_remote_gbas = false;

  Sram sram;

  // Sent in their own messages, not with the rest.
  PadMappingArray pad_map{};
  GBAConfigArray gba_config{};
  PadMappingArray wiimote_map{};

  // Local only; never sent.
  bool is_hosting = false;
  PlayerId local_player_id;
  std::array<std::string, 4> gba_rom_paths{};
};

// Calls `field` on each value of a StartGame message after the game number, in wire order. The
// host and the clients both go through here so the two ends cannot drift apart. `initial_rtc` and
// `region` travel in the middle of the settings; the region is the host's save directory name.
// accurate_fmadds is not part of the message.
template <typename Field>
void VisitStartGameSettings(NetSettings& s, u64& initial_rtc, std::string& region, Field&& field)
{
  field(s.cpu_thread);
  field(s.cpu_core);
  field(s.enable_cheats);
  field(s.enable_hardcore);
  field(s.selected_language);
  field(s.override_region_settings);
  // The JIT flag goes before the HLE flag.
  field(s.dsp_enable_jit);
  field(s.dsp_hle);
  field(s.ram_override_enable);
  field(s.mem1_size);
  field(s.mem2_size);
  field(s.fallback_region);
  field(s.allow_sd_writes);
  field(s.oc_enable);
  field(s.oc_factor);
  field(s.vi_oc_enable);
  field(s.vi_oc_factor);
  // Device types go as their underlying int.
  for (const ExpansionInterface::Slot slot : ExpansionInterface::SLOTS)
    field(s.exi_device[slot]);
  field(s.memcard_size_override);
  for (u32& sysconf_value : s.sysconf_settings)
    field(sysconf_value);

  field(s.efb_access_enable);
  field(s.bbox_enable);
  field(s.force_progressive);
  field(s.efb_to_texture_enable);
  field(s.xfb_to_texture_enable);
  field(s.disable_copy_to_vram);
  field(s.immediate_xfb_enable);
  field(s.efb_emulate_format_changes);
  field(s.safe_texture_cache_color_samples);
  field(s.perf_queries_enable);
  field(s.float_exceptions);
  field(s.divide_by_zero_exceptions);
  field(s.fprf);
  field(s.accurate_nans);
  field(s.disable_icache);
  field(s.sync_on_skip_idle);
  field(s.sync_gpu);
  field(s.sync_gpu_max_distance);
  field(s.sync_gpu_min_distance);
  field(s.sync_gpu_overclock);
  field(s.jit_follow_branch);
  field(s.fast_disc_speed);
  field(s.mmu);
  field(s.fastmem);
  field(s.skip_ipl);
  field(s.load_ipl_dump);
  field(s.vertex_rounding);
  field(s.internal_resolution);
  field(s.efb_scaled_copy);
  field(s.fast_depth_calc);
  field(s.enable_pixel_lighting);
  field(s.widescreen_hack);
  field(s.force_texture_filtering);
  field(s.max_anisotropy);
  field(s.force_true_color);
  field(s.disable_copy_filter);
  field(s.disable_fog);
  field(s.arbitrary_mipmap_detection);
  field(s.arbitrary_mipmap_detection_threshold);
  field(s.enable_gpu_texture_decoding);
  field(s.defer_efb_copies);
  field(s.efb_access_tile_size);
  field(s.efb_access_defer_invalidation);

  field(s.savedata_load);
  field(s.savedata_write);
  field(s.savedata_sync_all_wii);
  field(s.strict_settings_sync);
  field(initial_rtc);
  field(region);
  field(s.sync_codes);
  field(s.golf_mode);
  field(s.use_fma);
  field(s.hide_remote_gbas);
  // The host's SRAM, byte by byte, closes the message.
  for (std::size_t offset = 0; offset < sizeof(s.sram); ++offset)
    field(s.sram[offset]);
}

struct NetTraversalConfig
{
  NetTraversalConfig() = default;
  NetTraversalConfig(bool use_traversal_, std::string traversal_host_, u16 traversal_port_,
                     u16 traversal_port_alt_ = 0)
      : use_traversal{use_traversal_}, traversal_host{std::move(traversal_host_)},
        traversal_port{traversal_port_}, traversal_port_alt{traversal_port_alt_}
  {
  }

  bool use_traversal = false;
  std::string traversal_host;
  u16 traversal_port = 0;
  u16 traversal_port_alt = 0;
};

// First byte of every message. The values are part of the protocol.
enum class MessageID : u8
{
  ConnectionSuccessful = 0,

  PlayerJoin = 0x10,
  PlayerLeave = 0x11,

  ChatMessage = 0x30,

  ChunkedDataStart = 0x40,
  ChunkedDataEnd = 0x41,
  ChunkedDataPayload = 0x42,
  ChunkedDataProgress = 0x43,
  ChunkedDataComplete = 0x44,
  ChunkedDataAbort = 0x45,

  PadData = 0x60,
  PadMapping = 0x61,
  PadBuffer = 0x62,
  PadHostData = 0x63,
  GBAConfig = 0x64,

  WiimoteData = 0x70,
  WiimoteMapping = 0x71,

  GolfRequest = 0x90,
  GolfSwitch = 0x91,
  GolfAcquire = 0x92,
  GolfRelease = 0x93,
  GolfPrepare = 0x94,

  StartGame = 0xA0,
  ChangeGame = 0xA1,
  StopGame = 0xA2,
  DisableGame = 0xA3,
  GameStatus = 0xA4,
  ClientCapabilities = 0xA5,
  HostInputAuthority = 0xA6,
  PowerButton = 0xA7,

  TimeBase = 0xB0,
  DesyncDetected = 0xB1,

  ComputeGameDigest = 0xC0,
  GameDigestProgress = 0xC1,
  GameDigestResult = 0xC2,
  GameDigestAbort = 0xC3,
  GameDigestError = 0xC4,

  Ready = 0xD0,
  NotReady = 0xD1,

  Ping = 0xE0,
  Pong = 0xE1,
  PlayerPingData = 0xE2,

  SyncSaveData = 0xF1,
  SyncCodes = 0xF2,
};

// The host's answer to a connection attempt.
enum class ConnectionError : u8
{
  NoError = 0,
  ServerFull = 1,
  GameRunning = 2,
  VersionMismatch = 3,
  NameTooLong = 4
};

// Second byte of a SyncSaveData message.
enum class SyncSaveDataID : u8
{
  Notify = 0,
  Success = 1,
  Failure = 2,
  RawData = 3,
  GCIData = 4,
  WiiData = 5,
  GBAData = 6
};

// Second byte of a SyncCodes message.
enum class SyncCodeID : u8
{
  Notify = 0,
  NotifyGecko = 1,
  NotifyAR = 2,
  GeckoData = 3,
  ARData = 4,
  Success = 5,
  Failure = 6,
};

constexpr u32 MAX_NAME_LENGTH = 30;
constexpr u32 MAX_TARGET_PAD_BUFFER_SIZE = 99;
constexpr size_t CHUNKED_DATA_UNIT_SIZE = 16384;
// Larger packets get fragmented badly on some routes (enet issue 132).
constexpr u32 MAX_ENET_MTU = 1392;

enum : u8
{
  DEFAULT_CHANNEL,
  CHUNKED_DATA_CHANNEL,
  CHANNEL_COUNT
};

// Who drives an in-game controller port, from this machine's point of view.
struct PadDetails
{
  std::string player_name{};
  bool is_local = false;
  int local_pad = 0;
  bool hide_gba = false;
};

std::string GetPlayerMappingString(PlayerId pid, const PadMappingArray& pad_map,
                                   const GBAConfigArray& gba_config,
                                   const PadMappingArray& wiimote_map);
bool IsNetPlayRunning();
void SetSIPollBatching(bool state);
void SendPowerButtonEvent();
std::string GetGBASavePath(int pad_num);
PadDetails GetPadDetails(int pad_num);
int NumLocalWiimotes();
}  // namespace NetPlay
