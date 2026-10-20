// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/ConfigLoaders/MovieConfigLoader.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>

#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"

#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/SYSCONFSettings.h"
#include "Core/Config/SessionSettings.h"
#include "Core/Config/WiimoteSettings.h"
#include "Core/HW/SI/SI.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/HW/Wiimote.h"
#include "Core/Movie.h"

namespace PowerPC
{
enum class CPUCore;
}

namespace ConfigLoaders
{
namespace
{
// DTM controller masks: bits 0-3 are the GameCube ports, bits 4-7 the Wii Remotes.
constexpr int FIRST_WIIMOTE_BIT = 4;

constexpr bool HasBit(u8 mask, int bit)
{
  return ((mask >> bit) & 1) != 0;
}

constexpr u8 Bit(int bit)
{
  return static_cast<u8>(1u << bit);
}

// The device a movie expects on a GameCube port. Plain controllers keep the locally configured
// GameCube controller kind when there is one.
SerialInterface::SIDevices DeviceForMoviePort(const Movie::DTMHeader& dtm, int port)
{
  if (HasBit(dtm.GBAControllers, port))
    return SerialInterface::SIDEVICE_GC_GBA_EMULATED;
  if (!HasBit(dtm.controllers, port))
    return SerialInterface::SIDEVICE_NONE;

  const SerialInterface::SIDevices configured = Config::Get(Config::GetInfoForSIDevice(port));
  if (SerialInterface::SIDevice_IsGCController(configured))
    return configured;
  return HasBit(dtm.bongos, port) ? SerialInterface::SIDEVICE_GC_TARUKONGA :
                                    SerialInterface::SIDEVICE_GC_CONTROLLER;
}

void ApplyMovieHeader(Config::Layer& layer, const Movie::DTMHeader& dtm)
{
  // Core.
  layer.Set(Config::MAIN_CPU_THREAD, dtm.bDualCore);
  layer.Set(Config::MAIN_DSP_HLE, dtm.bDSPHLE);
  layer.Set(Config::MAIN_FAST_DISC_SPEED, dtm.bFastDiscSpeed);
  layer.Set(Config::MAIN_CPU_CORE, static_cast<PowerPC::CPUCore>(dtm.CPUCore));
  layer.Set(Config::MAIN_SYNC_GPU, dtm.bSyncGPU);
  // The backend name is a fixed-size field that isn't NUL-terminated when it is full.
  const char* const backend = dtm.videoBackend.data();
  layer.Set(Config::MAIN_GFX_BACKEND,
            std::string(backend, strnlen(backend, dtm.videoBackend.size())));

  // System settings. The language goes to SYSCONF on Wii and to the IPL setting on GameCube.
  layer.Set(Config::SYSCONF_PROGRESSIVE_SCAN, dtm.bProgressive);
  layer.Set(Config::SYSCONF_PAL60, dtm.bPAL60);
  if (!dtm.bWii)
    layer.Set(Config::MAIN_GC_LANGUAGE, static_cast<int>(dtm.language));
  else
    layer.Set(Config::SYSCONF_LANGUAGE, static_cast<u32>(dtm.language));
  layer.Set(Config::SYSCONF_WIDESCREEN, dtm.bWidescreen);
  layer.Set(Config::SYSCONF_COUNTRY, static_cast<u32>(dtm.countryCode));

  // Graphics hacks that change emulated behaviour.
  layer.Set(Config::GFX_HACK_EFB_ACCESS_ENABLE, dtm.bEFBAccessEnable);
  layer.Set(Config::GFX_HACK_SKIP_EFB_COPY_TO_RAM, dtm.bSkipEFBCopyToRam);
  layer.Set(Config::GFX_HACK_EFB_EMULATE_FORMAT_CHANGES, dtm.bEFBEmulateFormatChanges);
  layer.Set(Config::GFX_HACK_IMMEDIATE_XFB, dtm.bImmediateXFB);
  layer.Set(Config::GFX_HACK_SKIP_XFB_COPY_TO_RAM, dtm.bSkipXFBCopyToRam);

  layer.Set(Config::SESSION_USE_FMA, dtm.bUseFMA);
  layer.Set(Config::MAIN_JIT_FOLLOW_BRANCH, dtm.bFollowBranch);

  // Controllers.
  for (int port = 0; port < SerialInterface::MAX_SI_CHANNELS; ++port)
    layer.Set(Config::GetInfoForSIDevice(port), DeviceForMoviePort(dtm, port));

  for (int wiimote = 0; wiimote < MAX_WIIMOTES; ++wiimote)
  {
    const bool connected = HasBit(dtm.controllers, FIRST_WIIMOTE_BIT + wiimote);
    layer.Set(Config::GetInfoForWiimoteSource(wiimote),
              connected ? WiimoteSource::Emulated : WiimoteSource::None);
  }
}
}  // namespace

void SaveToDTM(Movie::DTMHeader* dtm)
{
  Movie::DTMHeader& header = *dtm;

  header.bDualCore = Config::Get(Config::MAIN_CPU_THREAD);
  header.bDSPHLE = Config::Get(Config::MAIN_DSP_HLE);
  header.bFastDiscSpeed = Config::Get(Config::MAIN_FAST_DISC_SPEED);
  header.CPUCore = static_cast<u8>(Config::Get(Config::MAIN_CPU_CORE));
  header.bSyncGPU = Config::Get(Config::MAIN_SYNC_GPU);
  const std::string backend = Config::Get(Config::MAIN_GFX_BACKEND);

  header.bProgressive = Config::Get(Config::SYSCONF_PROGRESSIVE_SCAN);
  header.bPAL60 = Config::Get(Config::SYSCONF_PAL60);
  header.language = header.bWii ? static_cast<u8>(Config::Get(Config::SYSCONF_LANGUAGE)) :
                                  static_cast<u8>(Config::Get(Config::MAIN_GC_LANGUAGE));
  header.bWidescreen = Config::Get(Config::SYSCONF_WIDESCREEN);
  header.countryCode = static_cast<u8>(Config::Get(Config::SYSCONF_COUNTRY));

  header.bEFBAccessEnable = Config::Get(Config::GFX_HACK_EFB_ACCESS_ENABLE);
  header.bSkipEFBCopyToRam = Config::Get(Config::GFX_HACK_SKIP_EFB_COPY_TO_RAM);
  header.bEFBEmulateFormatChanges = Config::Get(Config::GFX_HACK_EFB_EMULATE_FORMAT_CHANGES);
  header.bImmediateXFB = Config::Get(Config::GFX_HACK_IMMEDIATE_XFB);
  header.bSkipXFBCopyToRam = Config::Get(Config::GFX_HACK_SKIP_XFB_COPY_TO_RAM);

  header.bUseFMA = Config::Get(Config::SESSION_USE_FMA);
  header.bFollowBranch = Config::Get(Config::MAIN_JIT_FOLLOW_BRANCH);

  u8 plugged = 0;
  u8 gba = 0;
  u8 bongos = 0;
  for (int port = 0; port < SerialInterface::MAX_SI_CHANNELS; ++port)
  {
    const SerialInterface::SIDevices device = Config::Get(Config::GetInfoForSIDevice(port));
    if (device == SerialInterface::SIDEVICE_NONE)
      continue;
    plugged |= Bit(port);
    if (device == SerialInterface::SIDEVICE_GC_GBA_EMULATED)
      gba |= Bit(port);
    else if (device == SerialInterface::SIDEVICE_GC_TARUKONGA)
      bongos |= Bit(port);
  }
  // Wii Remotes are only recorded for Wii titles.
  for (int wiimote = 0; header.bWii && wiimote < MAX_WIIMOTES; ++wiimote)
  {
    if (Config::Get(Config::GetInfoForWiimoteSource(wiimote)) != WiimoteSource::None)
      plugged |= Bit(FIRST_WIIMOTE_BIT + wiimote);
  }
  header.controllers = plugged;
  header.GBAControllers = gba;
  header.bongos = bongos;

  // Fields for options that no longer exist get the values matching current behaviour.
  header.bSkipIdle = true;
  header.bEFBCopyEnable = true;
  header.bEFBCopyCacheEnable = false;

  // Same layout as strncpy: the name up to its first NUL, cut to the field size, then zeros.
  std::ranges::fill(header.videoBackend, '\0');
  const size_t name_length = std::min(std::strlen(backend.c_str()), header.videoBackend.size());
  std::memcpy(header.videoBackend.data(), backend.c_str(), name_length);
}

// Only the subset of settings stored in the DTM header is applied; the rest of the configuration
// isn't recorded in movies.
void MovieConfigLayerLoader::Load(Config::Layer* config_layer)
{
  ApplyMovieHeader(*config_layer, *m_header);
}

void MovieConfigLayerLoader::Save(Config::Layer*)
{
  // Movies are read-only as far as the config system is concerned.
}

std::unique_ptr<Config::ConfigLayerLoader> GenerateMovieConfigLoader(Movie::DTMHeader* header)
{
  return std::make_unique<MovieConfigLayerLoader>(header);
}
}  // namespace ConfigLoaders
