// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/ConfigLoaders/NetPlayConfigLoader.h"

#include <memory>
#include <string>
#include <type_traits>
#include <utility>

#include <fmt/format.h>

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"

#include "Core/Config/AchievementSettings.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/SYSCONFSettings.h"
#include "Core/Config/SessionSettings.h"
#include "Core/Config/WiimoteSettings.h"
#include "Core/HW/EXI/EXI.h"
#include "Core/HW/SI/SI.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/HW/Wiimote.h"
#include "Core/NetPlayProto.h"

#include "InputCommon/GCAdapter.h"

namespace ConfigLoaders
{
namespace
{
// Everything that affects emulation has to be identical on all NetPlay peers, so the host's
// values are forced through this layer. The groups below are applied in a fixed order.

void ForceCoreSettings(Config::Layer& layer, const NetPlay::NetSettings& host)
{
  layer.Set(Config::MAIN_CPU_THREAD, host.cpu_thread);
  layer.Set(Config::MAIN_CPU_CORE, host.cpu_core);
  layer.Set(Config::MAIN_ENABLE_CHEATS, host.enable_cheats);
#ifdef USE_RETRO_ACHIEVEMENTS
  layer.Set(Config::RA_HARDCORE_ENABLED, host.enable_hardcore);
#endif
  layer.Set(Config::MAIN_GC_LANGUAGE, host.selected_language);
  layer.Set(Config::MAIN_OVERRIDE_REGION_SETTINGS, host.override_region_settings);
  layer.Set(Config::MAIN_DSP_HLE, host.dsp_hle);
  layer.Set(Config::MAIN_OVERCLOCK_ENABLE, host.oc_enable);
  layer.Set(Config::MAIN_OVERCLOCK, host.oc_factor);
  layer.Set(Config::MAIN_VI_OVERCLOCK_ENABLE, host.vi_oc_enable);
  layer.Set(Config::MAIN_VI_OVERCLOCK, host.vi_oc_factor);

  for (const ExpansionInterface::Slot slot : ExpansionInterface::SLOTS)
    layer.Set(Config::GetInfoForEXIDevice(slot), host.exi_device[slot]);
  layer.Set(Config::MAIN_MEMORY_CARD_SIZE, host.memcard_size_override);
  layer.Set(Config::SESSION_SAVE_DATA_WRITABLE, host.savedata_write);

  layer.Set(Config::MAIN_RAM_OVERRIDE_ENABLE, host.ram_override_enable);
  layer.Set(Config::MAIN_MEM1_SIZE, host.mem1_size);
  layer.Set(Config::MAIN_MEM2_SIZE, host.mem2_size);
  layer.Set(Config::MAIN_FALLBACK_REGION, host.fallback_region);
  layer.Set(Config::MAIN_ALLOW_SD_WRITES, host.allow_sd_writes);
  layer.Set(Config::MAIN_DSP_JIT, host.dsp_enable_jit);
}

void ForceSysconfSettings(Config::Layer& layer, const NetPlay::NetSettings& host)
{
  // The host sends the SYSCONF values in the order of SYSCONF_SETTINGS, all widened to u32.
  size_t index = 0;
  for (const Config::SYSCONFSetting& setting : Config::SYSCONF_SETTINGS)
  {
    const u32 sent = host.sysconf_settings[index++];
    std::visit(
        [&layer, sent](const auto* info) {
          using Value = std::remove_cvref_t<decltype(info->GetDefaultValue())>;
          layer.Set(*info, static_cast<Value>(sent));
        },
        setting.config_info);
  }
}

void ForceDeterminismSettings(Config::Layer& layer, const NetPlay::NetSettings& host)
{
  layer.Set(Config::GFX_HACK_EFB_ACCESS_ENABLE, host.efb_access_enable);
  layer.Set(Config::GFX_HACK_BBOX_ENABLE, host.bbox_enable);
  layer.Set(Config::GFX_HACK_FORCE_PROGRESSIVE, host.force_progressive);
  layer.Set(Config::GFX_HACK_SKIP_EFB_COPY_TO_RAM, host.efb_to_texture_enable);
  layer.Set(Config::GFX_HACK_SKIP_XFB_COPY_TO_RAM, host.xfb_to_texture_enable);
  layer.Set(Config::GFX_HACK_DISABLE_COPY_TO_VRAM, host.disable_copy_to_vram);
  layer.Set(Config::GFX_HACK_IMMEDIATE_XFB, host.immediate_xfb_enable);
  layer.Set(Config::GFX_HACK_EFB_EMULATE_FORMAT_CHANGES, host.efb_emulate_format_changes);
  layer.Set(Config::GFX_SAFE_TEXTURE_CACHE_COLOR_SAMPLES, host.safe_texture_cache_color_samples);
  layer.Set(Config::GFX_PERF_QUERIES_ENABLE, host.perf_queries_enable);

  // CPU accuracy options.
  layer.Set(Config::MAIN_FLOAT_EXCEPTIONS, host.float_exceptions);
  layer.Set(Config::MAIN_DIVIDE_BY_ZERO_EXCEPTIONS, host.divide_by_zero_exceptions);
  layer.Set(Config::MAIN_FPRF, host.fprf);
  layer.Set(Config::MAIN_ACCURATE_NANS, host.accurate_nans);
  layer.Set(Config::MAIN_ACCURATE_FMADDS, host.accurate_fmadds);
  layer.Set(Config::MAIN_DISABLE_ICACHE, host.disable_icache);

  // GPU synchronisation.
  layer.Set(Config::MAIN_SYNC_ON_SKIP_IDLE, host.sync_on_skip_idle);
  layer.Set(Config::MAIN_SYNC_GPU, host.sync_gpu);
  layer.Set(Config::MAIN_SYNC_GPU_MAX_DISTANCE, host.sync_gpu_max_distance);
  layer.Set(Config::MAIN_SYNC_GPU_MIN_DISTANCE, host.sync_gpu_min_distance);
  layer.Set(Config::MAIN_SYNC_GPU_OVERCLOCK, host.sync_gpu_overclock);

  layer.Set(Config::MAIN_JIT_FOLLOW_BRANCH, host.jit_follow_branch);
  layer.Set(Config::MAIN_FAST_DISC_SPEED, host.fast_disc_speed);
  layer.Set(Config::MAIN_MMU, host.mmu);
  layer.Set(Config::MAIN_FASTMEM, host.fastmem);
  layer.Set(Config::MAIN_SKIP_IPL, host.skip_ipl);
  layer.Set(Config::SESSION_LOAD_IPL_DUMP, host.load_ipl_dump);

  layer.Set(Config::GFX_HACK_DEFER_EFB_COPIES, host.defer_efb_copies);
  layer.Set(Config::GFX_HACK_EFB_ACCESS_TILE_SIZE, host.efb_access_tile_size);
  layer.Set(Config::GFX_HACK_EFB_DEFER_INVALIDATION, host.efb_access_defer_invalidation);

  layer.Set(Config::SESSION_USE_FMA, host.use_fma);

  // Real Bluetooth adapters can't be synchronised.
  layer.Set(Config::MAIN_BLUETOOTH_PASSTHROUGH_ENABLED, false);
}

// With strict sync, the rendering options that can change emulated results are forced as well.
void ForceStrictGraphicsSettings(Config::Layer& layer, const NetPlay::NetSettings& host)
{
  layer.Set(Config::GFX_HACK_VERTEX_ROUNDING, host.vertex_rounding);
  layer.Set(Config::GFX_EFB_SCALE, host.internal_resolution);
  layer.Set(Config::GFX_HACK_COPY_EFB_SCALED, host.efb_scaled_copy);
  layer.Set(Config::GFX_FAST_DEPTH_CALC, host.fast_depth_calc);
  layer.Set(Config::GFX_ENABLE_PIXEL_LIGHTING, host.enable_pixel_lighting);
  layer.Set(Config::GFX_WIDESCREEN_HACK, host.widescreen_hack);
  layer.Set(Config::GFX_ENHANCE_FORCE_TEXTURE_FILTERING, host.force_texture_filtering);
  layer.Set(Config::GFX_ENHANCE_MAX_ANISOTROPY, host.max_anisotropy);
  layer.Set(Config::GFX_ENHANCE_FORCE_TRUE_COLOR, host.force_true_color);
  layer.Set(Config::GFX_ENHANCE_DISABLE_COPY_FILTER, host.disable_copy_filter);
  layer.Set(Config::GFX_DISABLE_FOG, host.disable_fog);
  layer.Set(Config::GFX_ENHANCE_ARBITRARY_MIPMAP_DETECTION, host.arbitrary_mipmap_detection);
  layer.Set(Config::GFX_ENHANCE_ARBITRARY_MIPMAP_DETECTION_THRESHOLD,
            host.arbitrary_mipmap_detection_threshold);
  layer.Set(Config::GFX_ENABLE_GPU_TEXTURE_DECODING, host.enable_gpu_texture_decoding);

  // Anti-aliasing output differs between GPUs, so it is switched off.
  layer.Set(Config::GFX_MSAA, 1);
  layer.Set(Config::GFX_SSAA, false);
}

// GameCube ports. The host decides which ports are used and which of them are emulated GBAs; for
// the other used ports each peer keeps its own controller kind (standard pad, adapter, ...), taken
// from its local configuration in the order of the ports assigned to the local player.
//
// Known limitation: a port that is a GBA on one side can't be driven by a real adapter on another.
void ForceGameCubePorts(Config::Layer& layer, const NetPlay::NetSettings& host)
{
  int next_local_port = 0;
  for (int port = 0; port < SerialInterface::MAX_SI_CHANNELS; ++port)
  {
    const NetPlay::PlayerId owner = host.pad_map[static_cast<size_t>(port)];
    const auto& port_info = Config::GetInfoForSIDevice(port);

    if (owner == 0)
    {
      layer.Set(port_info, SerialInterface::SIDEVICE_NONE);
      continue;
    }

    if (host.gba_config[static_cast<size_t>(port)].enabled)
    {
      layer.Set(port_info, SerialInterface::SIDEVICE_GC_GBA_EMULATED);
    }
    else
    {
      const SerialInterface::SIDevices local_kind =
          Config::Get(Config::GetInfoForSIDevice(next_local_port));
      if (SerialInterface::SIDevice_IsGCController(local_kind))
      {
        layer.Set(port_info, local_kind);
        if (local_kind == SerialInterface::SIDEVICE_WIIU_ADAPTER)
          GCAdapter::ResetDeviceType(next_local_port);
      }
      else if (local_kind != SerialInterface::SIDEVICE_AM_BASEBOARD)
      {
        layer.Set(port_info, SerialInterface::SIDEVICE_GC_CONTROLLER);
      }
      // (A Triforce baseboard keeps whatever the port is configured as.)
    }

    if (owner == host.local_player_id)
      ++next_local_port;
  }
}

void ForceWiimotes(Config::Layer& layer, const NetPlay::NetSettings& host)
{
  for (int wiimote = 0; wiimote < MAX_WIIMOTES; ++wiimote)
  {
    const bool assigned = host.wiimote_map[static_cast<size_t>(wiimote)] > 0;
    layer.Set(Config::GetInfoForWiimoteSource(wiimote),
              assigned ? WiimoteSource::Emulated : WiimoteSource::None);
  }
}

// When the host sends save data, clients use temporary NetPlay memory cards and GCI folders.
void UseNetPlaySaveData(Config::Layer& layer, const NetPlay::NetSettings& host)
{
  if (!host.is_hosting)
  {
    const std::string& gc_user = File::GetUserPath(D_GCUSER_IDX);
    const std::string folder_base = gc_user + GC_MEMCARD_NETPLAY DIR_SEP;
    layer.Set(Config::MAIN_GCI_FOLDER_A_PATH_OVERRIDE, folder_base + "Card A");
    layer.Set(Config::MAIN_GCI_FOLDER_B_PATH_OVERRIDE, folder_base + "Card B");

    for (const char card : {'A', 'B'})
    {
      const std::string raw_path =
          fmt::format("{}{}{}.{}.raw", gc_user, GC_MEMCARD_NETPLAY, card, host.save_data_region);
      layer.Set(card == 'A' ? Config::MAIN_MEMCARD_A_PATH : Config::MAIN_MEMCARD_B_PATH, raw_path);
    }
  }
  layer.Set(Config::SESSION_GCI_FOLDER_CURRENT_GAME_ONLY, true);
}

class NetPlaySettingsLayerLoader final : public Config::ConfigLayerLoader
{
public:
  explicit NetPlaySettingsLayerLoader(NetPlay::NetSettings settings)
      : ConfigLayerLoader(Config::LayerType::Netplay), m_host(std::move(settings))
  {
  }

  void Load(Config::Layer* layer) override
  {
    Config::Layer& out = *layer;
    ForceCoreSettings(out, m_host);
    ForceSysconfSettings(out, m_host);
    ForceDeterminismSettings(out, m_host);
    if (m_host.strict_settings_sync)
      ForceStrictGraphicsSettings(out, m_host);
    ForceGameCubePorts(out, m_host);
    ForceWiimotes(out, m_host);
    if (m_host.savedata_load)
      UseNetPlaySaveData(out, m_host);

#ifdef HAS_LIBMGBA
    for (size_t gba = 0; gba < m_host.gba_rom_paths.size(); ++gba)
      out.Set(Config::MAIN_GBA_ROM_PATHS[gba], m_host.gba_rom_paths[gba]);
#endif

    // Clients replace their own cheat codes with the host's when code sync is on.
    if (!m_host.is_hosting && m_host.sync_codes)
      out.Set(Config::SESSION_CODE_SYNC_OVERRIDE, true);
  }

  void Save(Config::Layer*) override
  {
    // The NetPlay layer only lives for the session and is never written anywhere.
  }

private:
  const NetPlay::NetSettings m_host;
};
}  // namespace

std::unique_ptr<Config::ConfigLayerLoader>
GenerateNetPlayConfigLoader(const NetPlay::NetSettings& settings)
{
  return std::make_unique<NetPlaySettingsLayerLoader>(settings);
}
}  // namespace ConfigLoaders
