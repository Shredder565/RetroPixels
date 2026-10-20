// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/Config/MainSettings.h"

#include <sstream>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include "AudioCommon/AudioCommon.h"
#include "Common/Assert.h"
#include "Common/CommonPaths.h"
#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/EnumMap.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/MathUtil.h"
#include "Common/StringUtil.h"
#include "Common/Version.h"
#include "Core/AchievementManager.h"
#include "Core/Config/DefaultLocale.h"
#include "Core/Core.h"
#include "Core/HW/EXI/EXI.h"
#include "Core/HW/EXI/EXI_Device.h"
#include "Core/HW/GCMemcard/GCMemcard.h"
#include "Core/HW/HSP/HSP_Device.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/IOS/Network/Socket.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"
#include "Core/USBUtils.h"
#include "DiscIO/Enums.h"
#include "VideoCommon/VideoBackendBase.h"

namespace Config
{
namespace
{
// Every setting in this file lives in the Main system (Dolphin.ini); only the section and key
// differ.
Location MainKey(std::string_view section, std::string_view key)
{
  return Location{System::Main, std::string(section), std::string(key)};
}

// The two-entry tables used by the memory card slot lookups below, indexed by slot A/B.
using SlotPathTable =
    Common::EnumMap<const Info<std::string>*, ExpansionInterface::MAX_MEMCARD_SLOT>;

const Info<std::string>& PickForMemcardSlot(const SlotPathTable& table,
                                            ExpansionInterface::Slot slot)
{
  ASSERT(ExpansionInterface::IsMemcardSlot(slot));
  return *table[slot];
}

// The CPU thread (dual core) is off by default; precise frame timing is on.
constexpr bool CPU_THREAD_BY_DEFAULT = false;
constexpr bool PRECISION_FRAME_TIMING_BY_DEFAULT = true;

// 2000-01-01 00:00:00 UTC in seconds since the Unix epoch (30 years, 7 of them leap years).
constexpr u32 RTC_DEFAULT_SECONDS = (30 * 365 + 7) * 24 * 60 * 60;
}  // namespace

// [Core]

const Info<bool> MAIN_SKIP_IPL{MainKey("Core", "SkipIPL"), true};
const Info<PowerPC::CPUCore> MAIN_CPU_CORE{MainKey("Core", "CPUCore"), PowerPC::DefaultCPUCore()};
const Info<bool> MAIN_JIT_FOLLOW_BRANCH{MainKey("Core", "JITFollowBranch"), true};
const Info<bool> MAIN_FASTMEM{MainKey("Core", "Fastmem"), true};
const Info<bool> MAIN_PAGE_TABLE_FASTMEM{MainKey("Core", "PageTableFastmem"), true};
const Info<bool> MAIN_FASTMEM_ARENA{MainKey("Core", "FastmemArena"), true};
const Info<bool> MAIN_LARGE_ENTRY_POINTS_MAP{MainKey("Core", "LargeEntryPointsMap"), true};
const Info<bool> MAIN_ACCURATE_CPU_CACHE{MainKey("Core", "AccurateCPUCache"), false};
const Info<bool> MAIN_DSP_HLE{MainKey("Core", "DSPHLE"), true};
const Info<int> MAIN_MAX_FALLBACK{MainKey("Core", "MaxFallback"), 100};
const Info<int> MAIN_TIMING_VARIANCE{MainKey("Core", "TimingVariance"), 40};
const Info<bool> MAIN_CORRECT_TIME_DRIFT{MainKey("Core", "CorrectTimeDrift"), false};
const Info<bool> MAIN_RUSH_FRAME_PRESENTATION{MainKey("Core", "RushFramePresentation"), false};
const Info<bool> MAIN_SMOOTH_EARLY_PRESENTATION{MainKey("Core", "SmoothEarlyPresentation"), false};
const Info<bool> MAIN_CPU_THREAD{MainKey("Core", "CPUThread"), CPU_THREAD_BY_DEFAULT};
const Info<bool> MAIN_LOAD_GAME_INTO_MEMORY{MainKey("Core", "LoadGameIntoMemory"), false};
const Info<bool> MAIN_SYNC_ON_SKIP_IDLE{MainKey("Core", "SyncOnSkipIdle"), true};
const Info<std::string> MAIN_DEFAULT_ISO{MainKey("Core", "DefaultISO"), ""};
const Info<bool> MAIN_ENABLE_CHEATS{MainKey("Core", "EnableCheats"), false};
const Info<int> MAIN_GC_LANGUAGE{MainKey("Core", "SelectedLanguage"), 0};
const Info<bool> MAIN_OVERRIDE_REGION_SETTINGS{MainKey("Core", "OverrideRegionSettings"), false};
const Info<bool> MAIN_DPL2_DECODER{MainKey("Core", "DPL2Decoder"), false};
const Info<AudioCommon::DPL2Quality> MAIN_DPL2_QUALITY{MainKey("Core", "DPL2Quality"),
                                                       AudioCommon::GetDefaultDPL2Quality()};
const Info<int> MAIN_AUDIO_LATENCY{MainKey("Core", "AudioLatency"), 20};
const Info<int> MAIN_AUDIO_BUFFER_SIZE{MainKey("Core", "AudioBufferSize"), 80};
const Info<bool> MAIN_AUDIO_FILL_GAPS{MainKey("Core", "AudioFillGaps"), true};
const Info<bool> MAIN_AUDIO_PRESERVE_PITCH{MainKey("Core", "AudioPreservePitch"), false};
const Info<std::string> MAIN_MEMCARD_A_PATH{MainKey("Core", "MemcardAPath"), ""};
const Info<std::string> MAIN_MEMCARD_B_PATH{MainKey("Core", "MemcardBPath"), ""};
const Info<std::string> MAIN_AGP_CART_A_PATH{MainKey("Core", "AgpCartAPath"), ""};
const Info<std::string> MAIN_AGP_CART_B_PATH{MainKey("Core", "AgpCartBPath"), ""};
const Info<std::string> MAIN_GCI_FOLDER_A_PATH{MainKey("Core", "GCIFolderAPath"), ""};
const Info<std::string> MAIN_GCI_FOLDER_B_PATH{MainKey("Core", "GCIFolderBPath"), ""};
const Info<std::string> MAIN_GCI_FOLDER_A_PATH_OVERRIDE{MainKey("Core", "GCIFolderAPathOverride"),
                                                        ""};
const Info<std::string> MAIN_GCI_FOLDER_B_PATH_OVERRIDE{MainKey("Core", "GCIFolderBPathOverride"),
                                                        ""};
const Info<int> MAIN_MEMORY_CARD_SIZE{MainKey("Core", "MemoryCardSize"), -1};
const Info<ExpansionInterface::EXIDeviceType> MAIN_SLOT_A{
    MainKey("Core", "SlotA"), ExpansionInterface::EXIDeviceType::MemoryCardFolder};
const Info<ExpansionInterface::EXIDeviceType> MAIN_SLOT_B{MainKey("Core", "SlotB"),
                                                          ExpansionInterface::EXIDeviceType::None};
const Info<ExpansionInterface::EXIDeviceType> MAIN_SERIAL_PORT_1{
    MainKey("Core", "SerialPort1"), ExpansionInterface::EXIDeviceType::None};
const Info<ExpansionInterface::EXIDeviceType> MAIN_SERIAL_PORT_2{
    MainKey("Core", "SerialPort2"), ExpansionInterface::EXIDeviceType::None};

const Info<std::string>& GetInfoForMemcardPath(ExpansionInterface::Slot slot)
{
  static constexpr SlotPathTable table{&MAIN_MEMCARD_A_PATH, &MAIN_MEMCARD_B_PATH};
  return PickForMemcardSlot(table, slot);
}

const Info<std::string>& GetInfoForAGPCartPath(ExpansionInterface::Slot slot)
{
  static constexpr SlotPathTable table{&MAIN_AGP_CART_A_PATH, &MAIN_AGP_CART_B_PATH};
  return PickForMemcardSlot(table, slot);
}

const Info<std::string>& GetInfoForGCIPath(ExpansionInterface::Slot slot)
{
  static constexpr SlotPathTable table{&MAIN_GCI_FOLDER_A_PATH, &MAIN_GCI_FOLDER_B_PATH};
  return PickForMemcardSlot(table, slot);
}

const Info<std::string>& GetInfoForGCIPathOverride(ExpansionInterface::Slot slot)
{
  static constexpr SlotPathTable table{&MAIN_GCI_FOLDER_A_PATH_OVERRIDE,
                                       &MAIN_GCI_FOLDER_B_PATH_OVERRIDE};
  return PickForMemcardSlot(table, slot);
}

const Info<ExpansionInterface::EXIDeviceType>& GetInfoForEXIDevice(ExpansionInterface::Slot slot)
{
  // Slot A, slot B, then the two serial ports, in ExpansionInterface::Slot order.
  static constexpr Common::EnumMap<const Info<ExpansionInterface::EXIDeviceType>*,
                                   ExpansionInterface::MAX_SLOT>
      table{&MAIN_SLOT_A, &MAIN_SLOT_B, &MAIN_SERIAL_PORT_1, &MAIN_SERIAL_PORT_2};
  return *table[slot];
}

// The per-port settings below are built on first use, so their names can be generated.
const Info<SerialInterface::SIDevices>& GetInfoForSIDevice(int channel)
{
  static const auto table = [] {
    using SerialInterface::SIDevices;
    const auto make = [](int port, SIDevices fallback) {
      return Info<SIDevices>{MainKey("Core", fmt::format("SIDevice{}", port)), fallback};
    };
    // Port 1 has a standard controller plugged in out of the box; the rest are empty.
    return std::array<const Info<SIDevices>, 4>{
        make(0, SerialInterface::SIDEVICE_GC_CONTROLLER), make(1, SerialInterface::SIDEVICE_NONE),
        make(2, SerialInterface::SIDEVICE_NONE), make(3, SerialInterface::SIDEVICE_NONE)};
  }();
  return table[static_cast<size_t>(channel)];
}

const Info<bool>& GetInfoForAdapterRumble(int channel)
{
  static const auto table = [] {
    const auto make = [](int port) {
      return Info<bool>{MainKey("Core", fmt::format("AdapterRumble{}", port)), true};
    };
    return std::array<const Info<bool>, 4>{make(0), make(1), make(2), make(3)};
  }();
  return table[static_cast<size_t>(channel)];
}

const Info<bool>& GetInfoForSimulateKonga(int channel)
{
  static const auto table = [] {
    const auto make = [](int port) {
      return Info<bool>{MainKey("Core", fmt::format("SimulateKonga{}", port)), false};
    };
    return std::array<const Info<bool>, 4>{make(0), make(1), make(2), make(3)};
  }();
  return table[static_cast<size_t>(channel)];
}

// Network adapter (BBA) and modem backends.
const Info<std::string> MAIN_BBA_MAC{MainKey("Core", "BBA_MAC"), ""};
const Info<std::string> MAIN_BBA_XLINK_IP{MainKey("Core", "BBA_XLINK_IP"), "127.0.0.1"};
const Info<bool> MAIN_BBA_XLINK_CHAT_OSD{MainKey("Core", "BBA_XLINK_CHAT_OSD"), true};
// The built-in BBA's default DNS server is the Schthack PSO server (https://schtserv.com/).
const Info<std::string> MAIN_BBA_BUILTIN_DNS{MainKey("Core", "BBA_BUILTIN_DNS"), "3.18.217.27"};
const Info<std::string> MAIN_BBA_TAPSERVER_DESTINATION{MainKey("Core", "BBA_TAPSERVER_DESTINATION"),
                                                       "/tmp/dolphin-tap"};
const Info<std::string> MAIN_MODEM_TAPSERVER_DESTINATION{
    MainKey("Core", "MODEM_TAPSERVER_DESTINATION"), "/tmp/dolphin-modem-tap"};
const Info<std::string> MAIN_BBA_BUILTIN_IP{MainKey("Core", "BBA_BUILTIN_IP"), ""};

// Wii peripherals and CPU/GPU emulation options.
const Info<bool> MAIN_WII_SD_CARD{MainKey("Core", "WiiSDCard"), true};
const Info<bool> MAIN_WII_SD_CARD_ENABLE_FOLDER_SYNC{MainKey("Core", "WiiSDCardEnableFolderSync"),
                                                     false};
const Info<u64> MAIN_WII_SD_CARD_FILESIZE{MainKey("Core", "WiiSDCardFilesize"), 0};
const Info<bool> MAIN_WII_KEYBOARD{MainKey("Core", "WiiKeyboard"), false};
const Info<bool> MAIN_WIIMOTE_CONTINUOUS_SCANNING{MainKey("Core", "WiimoteContinuousScanning"),
                                                  false};
const Info<std::string> MAIN_WIIMOTE_AUTO_CONNECT_ADDRESSES{
    MainKey("Core", "WiimoteAutoConnectAddresses"), ""};
const Info<bool> MAIN_WIIMOTE_ENABLE_SPEAKER{MainKey("Core", "WiimoteEnableSpeaker"), false};
const Info<bool> MAIN_CONNECT_WIIMOTES_FOR_CONTROLLER_INTERFACE{
    MainKey("Core", "WiimoteControllerInterface"), false};
const Info<bool> MAIN_MMU{MainKey("Core", "MMU"), false};
const Info<bool> MAIN_PAUSE_ON_PANIC{MainKey("Core", "PauseOnPanic"), false};
const Info<int> MAIN_BB_DUMP_PORT{MainKey("Core", "BBDumpPort"), -1};
const Info<bool> MAIN_SYNC_GPU{MainKey("Core", "SyncGPU"), false};
const Info<int> MAIN_SYNC_GPU_MAX_DISTANCE{MainKey("Core", "SyncGpuMaxDistance"), 200000};
const Info<int> MAIN_SYNC_GPU_MIN_DISTANCE{MainKey("Core", "SyncGpuMinDistance"), -200000};
const Info<float> MAIN_SYNC_GPU_OVERCLOCK{MainKey("Core", "SyncGpuOverclock"), 1.0f};
const Info<bool> MAIN_FAST_DISC_SPEED{MainKey("Core", "FastDiscSpeed"), false};
const Info<bool> MAIN_LOW_DCBZ_HACK{MainKey("Core", "LowDCBZHack"), false};
const Info<bool> MAIN_FLOAT_EXCEPTIONS{MainKey("Core", "FloatExceptions"), false};
const Info<bool> MAIN_DIVIDE_BY_ZERO_EXCEPTIONS{MainKey("Core", "DivByZeroExceptions"), false};
const Info<bool> MAIN_FPRF{MainKey("Core", "FPRF"), false};
const Info<bool> MAIN_ACCURATE_NANS{MainKey("Core", "AccurateNaNs"), false};
const Info<bool> MAIN_ACCURATE_FMADDS{MainKey("Core", "AccurateFmadds"), true};
const Info<bool> MAIN_DISABLE_ICACHE{MainKey("Core", "DisableICache"), false};
const Info<float> MAIN_EMULATION_SPEED{MainKey("Core", "EmulationSpeed"), 1.0f};
const Info<bool> MAIN_PRECISION_FRAME_TIMING{MainKey("Core", "PrecisionFrameTiming"),
                                             PRECISION_FRAME_TIMING_BY_DEFAULT};
const Info<float> MAIN_OVERCLOCK{MainKey("Core", "Overclock"), 1.0f};
const Info<bool> MAIN_OVERCLOCK_ENABLE{MainKey("Core", "OverclockEnable"), false};
const Info<float> MAIN_VI_OVERCLOCK{MainKey("Core", "VIOverclock"), 1.0f};
const Info<bool> MAIN_VI_OVERCLOCK_ENABLE{MainKey("Core", "VIOverclockEnable"), false};
const Info<bool> MAIN_RAM_OVERRIDE_ENABLE{MainKey("Core", "RAMOverrideEnable"), false};
const Info<u32> MAIN_MEM1_SIZE{MainKey("Core", "MEM1Size"), Memory::MEM1_SIZE_RETAIL};
const Info<u32> MAIN_MEM2_SIZE{MainKey("Core", "MEM2Size"), Memory::MEM2_SIZE_RETAIL};
const Info<std::string> MAIN_GFX_BACKEND{MainKey("Core", "GFXBackend"),
                                         VideoBackendBase::GetDefaultBackendConfigName()};
const Info<HSP::HSPDeviceType> MAIN_HSP_DEVICE{MainKey("Core", "HSPDevice"),
                                               HSP::HSPDeviceType::None};
const Info<u32> MAIN_ARAM_EXPANSION_SIZE{MainKey("Core", "ARAMExpansionSize"), 0x400000};
const Info<std::string> MAIN_GPU_DETERMINISM_MODE{MainKey("Core", "GPUDeterminismMode"), "auto"};
const Info<s32> MAIN_OVERRIDE_BOOT_IOS{MainKey("Core", "OverrideBootIOS"), -1};

GPUDeterminismMode GetGPUDeterminismMode()
{
  const std::string mode = Config::Get(Config::MAIN_GPU_DETERMINISM_MODE);
  static constexpr std::pair<std::string_view, GPUDeterminismMode> known_modes[] = {
      {"auto", GPUDeterminismMode::Auto},
      {"none", GPUDeterminismMode::Disabled},
      {"fake-completion", GPUDeterminismMode::FakeCompletion},
  };
  for (const auto& [name, value] : known_modes)
  {
    if (mode == name)
      return value;
  }
  NOTICE_LOG_FMT(CORE, "Unknown GPU determinism mode {}", mode);
  return GPUDeterminismMode::Auto;
}

const Info<std::string> MAIN_PERF_MAP_DIR{MainKey("Core", "PerfMapDir"), ""};
const Info<bool> MAIN_CUSTOM_RTC_ENABLE{MainKey("Core", "EnableCustomRTC"), false};
const Info<u32> MAIN_CUSTOM_RTC_VALUE{MainKey("Core", "CustomRTCValue"), RTC_DEFAULT_SECONDS};
const Info<DiscIO::Region> MAIN_FALLBACK_REGION{MainKey("Core", "FallbackRegion"),
                                                GetDefaultRegion()};
const Info<bool> MAIN_AUTO_DISC_CHANGE{MainKey("Core", "AutoDiscChange"), false};
const Info<bool> MAIN_ALLOW_SD_WRITES{MainKey("Core", "WiiSDCardAllowWrites"), true};
const Info<bool> MAIN_ENABLE_SAVESTATES{MainKey("Core", "EnableSaveStates"), false};
const Info<bool> MAIN_REAL_WII_REMOTE_REPEAT_REPORTS{MainKey("Core", "RealWiiRemoteRepeatReports"),
                                                     true};
const Info<bool> MAIN_WII_WIILINK_ENABLE{MainKey("Core", "EnableWiiLink"), false};
// An empty URL means the built-in default shop server.
const Info<std::string> MAIN_WII_NUS_SHOP_URL{MainKey("Core", "WiiNusShopUrl"), ""};

// [Display]

const Info<std::string> MAIN_FULLSCREEN_DISPLAY_RES{MainKey("Display", "FullscreenDisplayRes"),
                                                    "Auto"};
const Info<bool> MAIN_FULLSCREEN{MainKey("Display", "Fullscreen"), false};
const Info<bool> MAIN_RENDER_TO_MAIN{MainKey("Display", "RenderToMain"), false};
const Info<int> MAIN_RENDER_WINDOW_XPOS{MainKey("Display", "RenderWindowXPos"), -1};
const Info<int> MAIN_RENDER_WINDOW_YPOS{MainKey("Display", "RenderWindowYPos"), -1};
const Info<int> MAIN_RENDER_WINDOW_WIDTH{MainKey("Display", "RenderWindowWidth"), 640};
const Info<int> MAIN_RENDER_WINDOW_HEIGHT{MainKey("Display", "RenderWindowHeight"), 480};
const Info<bool> MAIN_RENDER_WINDOW_AUTOSIZE{MainKey("Display", "RenderWindowAutoSize"), false};
const Info<bool> MAIN_KEEP_WINDOW_ON_TOP{MainKey("Display", "KeepWindowOnTop"), false};
const Info<bool> MAIN_DISABLE_SCREENSAVER{MainKey("Display", "DisableScreenSaver"), true};

// [DSP]

const Info<bool> MAIN_DSP_THREAD{MainKey("DSP", "DSPThread"), false};
const Info<bool> MAIN_DSP_CAPTURE_LOG{MainKey("DSP", "CaptureLog"), false};
const Info<bool> MAIN_DSP_JIT{MainKey("DSP", "EnableJIT"), true};
const Info<bool> MAIN_DUMP_AUDIO{MainKey("DSP", "DumpAudio"), false};
const Info<bool> MAIN_DUMP_AUDIO_SILENT{MainKey("DSP", "DumpAudioSilent"), false};
const Info<bool> MAIN_DUMP_UCODE{MainKey("DSP", "DumpUCode"), false};
const Info<std::string> MAIN_AUDIO_BACKEND{MainKey("DSP", "Backend"),
                                           AudioCommon::GetDefaultSoundBackend()};
const Info<int> MAIN_AUDIO_VOLUME{MainKey("DSP", "Volume"), 100};
const Info<bool> MAIN_AUDIO_MUTED{MainKey("DSP", "Muted"), false};
const Info<bool> MAIN_AUDIO_MUTE_ON_DISABLED_SPEED_LIMIT{MainKey("DSP", "MuteOnDisabledSpeedLimit"),
                                                         false};
#ifdef _WIN32
const Info<std::string> MAIN_WASAPI_DEVICE{MainKey("DSP", "WASAPIDevice"), "default"};
#endif

bool ShouldUseDPL2Decoder()
{
  // The DPL2 decoder only applies to LLE audio.
  if (Get(MAIN_DSP_HLE))
    return false;
  return Get(MAIN_DPL2_DECODER);
}

// [General]

const Info<std::string> MAIN_DUMP_PATH{MainKey("General", "DumpPath"), ""};
const Info<std::string> MAIN_LOAD_PATH{MainKey("General", "LoadPath"), ""};
const Info<std::string> MAIN_RESOURCEPACK_PATH{MainKey("General", "ResourcePackPath"), ""};
const Info<std::string> MAIN_FS_PATH{MainKey("General", "NANDRootPath"), ""};
const Info<std::string> MAIN_WII_SD_CARD_IMAGE_PATH{MainKey("General", "WiiSDCardPath"), ""};
const Info<std::string> MAIN_WII_SD_CARD_SYNC_FOLDER_PATH{MainKey("General", "WiiSDCardSyncFolder"),
                                                          ""};
const Info<std::string> MAIN_WFS_PATH{MainKey("General", "WFSPath"), ""};
const Info<bool> MAIN_SHOW_LAG{MainKey("General", "ShowLag"), false};
const Info<bool> MAIN_SHOW_FRAME_COUNT{MainKey("General", "ShowFrameCount"), false};
const Info<std::string> MAIN_WIRELESS_MAC{MainKey("General", "WirelessMac"), ""};
const Info<std::string> MAIN_GDB_SOCKET{MainKey("General", "GDBSocket"), ""};
const Info<int> MAIN_GDB_PORT{MainKey("General", "GDBPort"), -1};
const Info<int> MAIN_ISO_PATH_COUNT{MainKey("General", "ISOPaths"), 0};
const Info<std::string> MAIN_SKYLANDERS_PATH{MainKey("General", "SkylandersCollectionPath"), ""};
const Info<bool> MAIN_TIME_TRACKING{MainKey("General", "EnablePlayTimeTracking"), true};

// The game list folders are stored as ISOPaths=<count> plus ISOPath0, ISOPath1, ...
static Info<std::string> IsoPathSlot(size_t index)
{
  return Info<std::string>{MainKey("General", fmt::format("ISOPath{}", index)), ""};
}

std::vector<std::string> GetIsoPaths()
{
  const auto stored = MathUtil::SaturatingCast<size_t>(Config::Get(Config::MAIN_ISO_PATH_COUNT));
  std::vector<std::string> folders;
  folders.reserve(stored);
  for (size_t index = 0; index != stored; ++index)
  {
    std::string folder = Config::Get(IsoPathSlot(index));
    if (folder.empty())
      continue;
    folders.push_back(std::move(folder));
  }
  return folders;
}

void SetIsoPaths(std::span<const std::string> paths)
{
  const auto previous_count =
      MathUtil::SaturatingCast<size_t>(Config::Get(Config::MAIN_ISO_PATH_COUNT));

  // Empty entries are dropped and the rest are packed into consecutive slots.
  size_t written = 0;
  for (const std::string& folder : paths)
  {
    if (!folder.empty())
      Config::SetBase(IsoPathSlot(written++), folder);
  }

  // Slots that used to hold a folder are blanked (there is no way to erase a key yet).
  for (size_t index = written; index < previous_count; ++index)
    Config::SetBase(IsoPathSlot(index), "");

  Config::SetBase(Config::MAIN_ISO_PATH_COUNT, MathUtil::SaturatingCast<int>(written));
}

// [GBA]

#ifdef HAS_LIBMGBA
const Info<std::string> MAIN_GBA_BIOS_PATH{MainKey("GBA", "BIOS"), ""};
// One ROM per GBA slot, plus the Game Boy Player ROM at GBPLAYER_GBA_INDEX.
const std::array<Info<std::string>, 5> MAIN_GBA_ROM_PATHS{
    Info<std::string>{MainKey("GBA", "Rom1"), ""},
    Info<std::string>{MainKey("GBA", "Rom2"), ""},
    Info<std::string>{MainKey("GBA", "Rom3"), ""},
    Info<std::string>{MainKey("GBA", "Rom4"), ""},
    Info<std::string>{MainKey("GBA", "GBPlayerRom"), ""},
};
const Info<std::string> MAIN_GBA_SAVES_PATH{MainKey("GBA", "SavesPath"), ""};
const Info<bool> MAIN_GBA_SAVES_IN_ROM_PATH{MainKey("GBA", "SavesInRomPath"), false};
#endif

// [Network]

const Info<bool> MAIN_NETWORK_SSL_DUMP_READ{MainKey("Network", "SSLDumpRead"), false};
const Info<bool> MAIN_NETWORK_SSL_DUMP_WRITE{MainKey("Network", "SSLDumpWrite"), false};
const Info<bool> MAIN_NETWORK_SSL_VERIFY_CERTIFICATES{MainKey("Network", "SSLVerifyCertificates"),
                                                      true};
const Info<bool> MAIN_NETWORK_SSL_DUMP_ROOT_CA{MainKey("Network", "SSLDumpRootCA"), false};
const Info<bool> MAIN_NETWORK_SSL_DUMP_PEER_CERT{MainKey("Network", "SSLDumpPeerCert"), false};
const Info<bool> MAIN_NETWORK_DUMP_BBA{MainKey("Network", "DumpBBA"), false};
const Info<bool> MAIN_NETWORK_DUMP_AS_PCAP{MainKey("Network", "DumpAsPCAP"), false};
// 190 s: at least the three minutes RFC 1122 (4.2.3.5) asks for before giving up on a TCP
// connection, rounded up to what a real console was measured to wait.
const Info<int> MAIN_NETWORK_TIMEOUT{MainKey("Network", "NetworkTimeout"), 190};

// [Interface] (the OSD font size has always lived in [Settings])

const Info<bool> MAIN_USE_HIGH_CONTRAST_TOOLTIPS{MainKey("Interface", "UseHighContrastTooltips"),
                                                 true};
const Info<bool> MAIN_USE_PANIC_HANDLERS{MainKey("Interface", "UsePanicHandlers"), true};
const Info<bool> MAIN_ABORT_ON_PANIC_ALERT{MainKey("Interface", "AbortOnPanicAlert"), false};
const Info<bool> MAIN_OSD_MESSAGES{MainKey("Interface", "OnScreenDisplayMessages"), true};
const Info<int> MAIN_OSD_FONT_SIZE{MainKey("Settings", "OSDFontSize"), 13};
const Info<bool> MAIN_SKIP_NKIT_WARNING{MainKey("Interface", "SkipNKitWarning"), false};
const Info<bool> MAIN_CONFIRM_ON_STOP{MainKey("Interface", "ConfirmStop"), true};
const Info<ShowCursor> MAIN_SHOW_CURSOR{MainKey("Interface", "CursorVisibility"),
                                        ShowCursor::OnMovement};
const Info<bool> MAIN_LOCK_CURSOR{MainKey("Interface", "LockCursor"), false};
const Info<std::string> MAIN_INTERFACE_LANGUAGE{MainKey("Interface", "LanguageCode"), ""};
const Info<bool> MAIN_SHOW_ACTIVE_TITLE{MainKey("Interface", "ShowActiveTitle"), true};
const Info<bool> MAIN_USE_BUILT_IN_TITLE_DATABASE{MainKey("Interface", "UseBuiltinTitleDatabase"),
                                                  true};
const Info<std::string> MAIN_THEME_NAME{MainKey("Interface", "ThemeName"), DEFAULT_THEME_DIR};
const Info<bool> MAIN_PAUSE_ON_FOCUS_LOST{MainKey("Interface", "PauseOnFocusLost"), false};
const Info<bool> MAIN_ENABLE_DEBUGGING{MainKey("Interface", "DebugModeEnabled"), false};

// [Analytics]

const Info<std::string> MAIN_ANALYTICS_ID{MainKey("Analytics", "ID"), ""};
const Info<bool> MAIN_ANALYTICS_ENABLED{MainKey("Analytics", "Enabled"), false};
const Info<bool> MAIN_ANALYTICS_PERMISSION_ASKED{MainKey("Analytics", "PermissionAsked"), false};

// [GameList]

const Info<bool> MAIN_GAMELIST_LIST_DRIVES{MainKey("GameList", "ListDrives"), false};
const Info<bool> MAIN_GAMELIST_LIST_WAD{MainKey("GameList", "ListWad"), true};
const Info<bool> MAIN_GAMELIST_LIST_ELF_DOL{MainKey("GameList", "ListElfDol"), true};
const Info<bool> MAIN_GAMELIST_LIST_WII{MainKey("GameList", "ListWii"), true};
const Info<bool> MAIN_GAMELIST_LIST_GC{MainKey("GameList", "ListGC"), true};
const Info<bool> MAIN_GAMELIST_LIST_TRI{MainKey("GameList", "ListTriforce"), true};
const Info<bool> MAIN_GAMELIST_LIST_JPN{MainKey("GameList", "ListJap"), true};
const Info<bool> MAIN_GAMELIST_LIST_PAL{MainKey("GameList", "ListPal"), true};
const Info<bool> MAIN_GAMELIST_LIST_USA{MainKey("GameList", "ListUsa"), true};
const Info<bool> MAIN_GAMELIST_LIST_AUSTRALIA{MainKey("GameList", "ListAustralia"), true};
const Info<bool> MAIN_GAMELIST_LIST_FRANCE{MainKey("GameList", "ListFrance"), true};
const Info<bool> MAIN_GAMELIST_LIST_GERMANY{MainKey("GameList", "ListGermany"), true};
const Info<bool> MAIN_GAMELIST_LIST_ITALY{MainKey("GameList", "ListItaly"), true};
const Info<bool> MAIN_GAMELIST_LIST_KOREA{MainKey("GameList", "ListKorea"), true};
const Info<bool> MAIN_GAMELIST_LIST_NETHERLANDS{MainKey("GameList", "ListNetherlands"), true};
const Info<bool> MAIN_GAMELIST_LIST_RUSSIA{MainKey("GameList", "ListRussia"), true};
const Info<bool> MAIN_GAMELIST_LIST_SPAIN{MainKey("GameList", "ListSpain"), true};
const Info<bool> MAIN_GAMELIST_LIST_TAIWAN{MainKey("GameList", "ListTaiwan"), true};
const Info<bool> MAIN_GAMELIST_LIST_WORLD{MainKey("GameList", "ListWorld"), true};
const Info<bool> MAIN_GAMELIST_LIST_UNKNOWN{MainKey("GameList", "ListUnknown"), true};
const Info<int> MAIN_GAMELIST_LIST_SORT{MainKey("GameList", "ListSort"), 3};
const Info<int> MAIN_GAMELIST_LIST_SORT_SECONDARY{MainKey("GameList", "ListSortSecondary"), 0};
const Info<bool> MAIN_GAMELIST_COLUMN_PLATFORM{MainKey("GameList", "ColumnPlatform"), true};
const Info<bool> MAIN_GAMELIST_COLUMN_DESCRIPTION{MainKey("GameList", "ColumnDescription"), false};
const Info<bool> MAIN_GAMELIST_COLUMN_BANNER{MainKey("GameList", "ColumnBanner"), true};
const Info<bool> MAIN_GAMELIST_COLUMN_TITLE{MainKey("GameList", "ColumnTitle"), true};
const Info<bool> MAIN_GAMELIST_COLUMN_MAKER{MainKey("GameList", "ColumnNotes"), true};
const Info<bool> MAIN_GAMELIST_COLUMN_FILE_NAME{MainKey("GameList", "ColumnFileName"), false};
const Info<bool> MAIN_GAMELIST_COLUMN_FILE_PATH{MainKey("GameList", "ColumnFilePath"), false};
const Info<bool> MAIN_GAMELIST_COLUMN_GAME_ID{MainKey("GameList", "ColumnID"), false};
const Info<bool> MAIN_GAMELIST_COLUMN_REGION{MainKey("GameList", "ColumnRegion"), true};
const Info<bool> MAIN_GAMELIST_COLUMN_FILE_SIZE{MainKey("GameList", "ColumnSize"), true};
const Info<bool> MAIN_GAMELIST_COLUMN_FILE_FORMAT{MainKey("GameList", "ColumnFileFormat"), false};
const Info<bool> MAIN_GAMELIST_COLUMN_BLOCK_SIZE{MainKey("GameList", "ColumnBlockSize"), false};
const Info<bool> MAIN_GAMELIST_COLUMN_COMPRESSION{MainKey("GameList", "ColumnCompression"), false};
const Info<bool> MAIN_GAMELIST_COLUMN_TIME_PLAYED{MainKey("GameList", "ColumnTimePlayed"), true};
const Info<bool> MAIN_GAMELIST_COLUMN_TAGS{MainKey("GameList", "ColumnTags"), false};

// [FifoPlayer]

const Info<bool> MAIN_FIFOPLAYER_LOOP_REPLAY{MainKey("FifoPlayer", "LoopReplay"), true};
const Info<bool> MAIN_FIFOPLAYER_EARLY_MEMORY_UPDATES{MainKey("FifoPlayer", "EarlyMemoryUpdates"),
                                                      false};

// [AutoUpdate]

const Info<std::string> MAIN_AUTOUPDATE_UPDATE_TRACK{MainKey("AutoUpdate", "UpdateTrack"),
                                                     Common::GetScmUpdateTrackStr()};
const Info<std::string> MAIN_AUTOUPDATE_HASH_OVERRIDE{MainKey("AutoUpdate", "HashOverride"), ""};

// [Movie]

const Info<bool> MAIN_MOVIE_PAUSE_MOVIE{MainKey("Movie", "PauseMovie"), false};
const Info<std::string> MAIN_MOVIE_MOVIE_AUTHOR{MainKey("Movie", "Author"), ""};
const Info<bool> MAIN_MOVIE_DUMP_FRAMES{MainKey("Movie", "DumpFrames"), false};
const Info<bool> MAIN_MOVIE_DUMP_FRAMES_SILENT{MainKey("Movie", "DumpFramesSilent"), false};
const Info<bool> MAIN_MOVIE_SHOW_INPUT_DISPLAY{MainKey("Movie", "ShowInputDisplay"), false};
const Info<bool> MAIN_MOVIE_SHOW_RTC{MainKey("Movie", "ShowRTC"), false};
const Info<bool> MAIN_MOVIE_SHOW_RERECORD{MainKey("Movie", "ShowRerecord"), false};
const Info<bool> MAIN_MOVIE_SHOW_OSD{MainKey("Movie", "ShowMovieWindow"), false};

// [Input]

const Info<bool> MAIN_INPUT_BACKGROUND_INPUT{MainKey("Input", "BackgroundInput"), false};

// [SDL_Hints]
// These start out empty on purpose: the SDL backend writes its own defaults into the INI so that
// they show up in the SDL hints dialog.

const Info<std::string> MAIN_SDL_HINT_JOYSTICK_ENHANCED_REPORTS{
    MainKey("SDL_Hints", "SDL_JOYSTICK_ENHANCED_REPORTS"), ""};
const Info<std::string> MAIN_SDL_HINT_JOYSTICK_WGI{MainKey("SDL_Hints", "SDL_JOYSTICK_WGI"), ""};
const Info<std::string> MAIN_SDL_HINT_JOYSTICK_HIDAPI_PS5_PLAYER_LED{
    MainKey("SDL_Hints", "SDL_JOYSTICK_HIDAPI_PS5_PLAYER_LED"), ""};
const Info<std::string> MAIN_SDL_HINT_JOYSTICK_DIRECTINPUT{
    MainKey("SDL_Hints", "SDL_JOYSTICK_DIRECTINPUT"), ""};
const Info<std::string> MAIN_SDL_HINT_JOYSTICK_HIDAPI_COMBINE_JOY_CONS{
    MainKey("SDL_Hints", "SDL_JOYSTICK_HIDAPI_COMBINE_JOY_CONS"), ""};
const Info<std::string> MAIN_SDL_HINT_JOYSTICK_HIDAPI_VERTICAL_JOY_CONS{
    MainKey("SDL_Hints", "SDL_JOYSTICK_HIDAPI_VERTICAL_JOY_CONS"), ""};

// [Debug]

const Info<bool> MAIN_DEBUG_JIT_OFF{MainKey("Debug", "JitOff"), false};
const Info<bool> MAIN_DEBUG_JIT_LOAD_STORE_OFF{MainKey("Debug", "JitLoadStoreOff"), false};
const Info<bool> MAIN_DEBUG_JIT_LOAD_STORE_LXZ_OFF{MainKey("Debug", "JitLoadStorelXzOff"), false};
const Info<bool> MAIN_DEBUG_JIT_LOAD_STORE_LWZ_OFF{MainKey("Debug", "JitLoadStorelwzOff"), false};
const Info<bool> MAIN_DEBUG_JIT_LOAD_STORE_LBZX_OFF{MainKey("Debug", "JitLoadStorelbzxOff"), false};
const Info<bool> MAIN_DEBUG_JIT_LOAD_STORE_FLOATING_OFF{MainKey("Debug", "JitLoadStoreFloatingOff"),
                                                        false};
const Info<bool> MAIN_DEBUG_JIT_LOAD_STORE_PAIRED_OFF{MainKey("Debug", "JitLoadStorePairedOff"),
                                                      false};
const Info<bool> MAIN_DEBUG_JIT_FLOATING_POINT_OFF{MainKey("Debug", "JitFloatingPointOff"), false};
const Info<bool> MAIN_DEBUG_JIT_INTEGER_OFF{MainKey("Debug", "JitIntegerOff"), false};
const Info<bool> MAIN_DEBUG_JIT_PAIRED_OFF{MainKey("Debug", "JitPairedOff"), false};
const Info<bool> MAIN_DEBUG_JIT_SYSTEM_REGISTERS_OFF{MainKey("Debug", "JitSystemRegistersOff"),
                                                     false};
const Info<bool> MAIN_DEBUG_JIT_BRANCH_OFF{MainKey("Debug", "JitBranchOff"), false};
const Info<bool> MAIN_DEBUG_JIT_REGISTER_CACHE_OFF{MainKey("Debug", "JitRegisterCacheOff"), false};
const Info<bool> MAIN_DEBUG_JIT_ENABLE_PROFILING{MainKey("Debug", "JitEnableProfiling"), false};

// [BluetoothPassthrough]

const Info<bool> MAIN_BLUETOOTH_PASSTHROUGH_ENABLED{MainKey("BluetoothPassthrough", "Enabled"),
                                                    false};
const Info<int> MAIN_BLUETOOTH_PASSTHROUGH_VID{MainKey("BluetoothPassthrough", "VID"), -1};
const Info<int> MAIN_BLUETOOTH_PASSTHROUGH_PID{MainKey("BluetoothPassthrough", "PID"), -1};
const Info<std::string> MAIN_BLUETOOTH_PASSTHROUGH_LINK_KEYS{
    MainKey("BluetoothPassthrough", "LinkKeys"), ""};

// [USBPassthrough]

const Info<bool> MAIN_USB_PASSTHROUGH_DISGUISE_PLAYSTATION_AS_WII{
    MainKey("USBPassthrough", "DisguisePlayStationAsWii"), true};
const Info<std::string> MAIN_USB_PASSTHROUGH_DEVICES{MainKey("USBPassthrough", "Devices"), ""};

// The device list is stored as a comma-separated list of "VID:PID" entries.
std::set<USBUtils::DeviceInfo> GetUSBDeviceWhitelist()
{
  std::set<USBUtils::DeviceInfo> devices;
  const std::string stored = Config::Get(Config::MAIN_USB_PASSTHROUGH_DEVICES);
  for (const std::string& entry : SplitString(stored, ','))
  {
    if (const auto device = USBUtils::DeviceInfo::FromString(entry))
      devices.insert(*device);
  }
  return devices;
}

void SetUSBDeviceWhitelist(const std::set<USBUtils::DeviceInfo>& devices)
{
  std::string joined;
  bool first = true;
  for (const USBUtils::DeviceInfo& device : devices)
  {
    if (!first)
      joined += ',';
    joined += device.ToString();
    first = false;
  }
  Config::SetBase(Config::MAIN_USB_PASSTHROUGH_DEVICES, joined);
}

// [EmulatedUSBDevices]

const Info<bool> MAIN_EMULATE_SKYLANDER_PORTAL{
    MainKey("EmulatedUSBDevices", "EmulateSkylanderPortal"), false};
const Info<bool> MAIN_EMULATE_INFINITY_BASE{MainKey("EmulatedUSBDevices", "EmulateInfinityBase"),
                                            false};
const Info<bool> MAIN_EMULATE_WII_SPEAK{MainKey("EmulatedUSBDevices", "EmulateWiiSpeak"), false};
const Info<std::string> MAIN_WII_SPEAK_MICROPHONE{
    MainKey("EmulatedUSBDevices", "WiiSpeakMicrophone"), ""};
const Info<bool> MAIN_WII_SPEAK_MUTED{MainKey("EmulatedUSBDevices", "WiiSpeakMuted"), true};
const Info<s16> MAIN_WII_SPEAK_VOLUME_MODIFIER{
    MainKey("EmulatedUSBDevices", "WiiSpeakVolumeModifier"), 0};

namespace
{
template <typename T>
std::array<Info<T>, EMULATED_LOGITECH_MIC_COUNT> LogitechMicSettings(std::string_view suffix,
                                                                     T fallback)
{
  const auto make = [&](int number) {
    return Info<T>{MainKey("EmulatedUSBDevices", fmt::format("LogitechMic{}{}", number, suffix)),
                   fallback};
  };
  return {make(1), make(2), make(3), make(4)};
}
}  // namespace

const std::array<Info<bool>, EMULATED_LOGITECH_MIC_COUNT> MAIN_EMULATE_LOGITECH_MIC{
    Info<bool>{MainKey("EmulatedUSBDevices", "EmulateLogitechMic1"), false},
    Info<bool>{MainKey("EmulatedUSBDevices", "EmulateLogitechMic2"), false},
    Info<bool>{MainKey("EmulatedUSBDevices", "EmulateLogitechMic3"), false},
    Info<bool>{MainKey("EmulatedUSBDevices", "EmulateLogitechMic4"), false}};
const std::array<Info<std::string>, EMULATED_LOGITECH_MIC_COUNT> MAIN_LOGITECH_MIC_MICROPHONE =
    LogitechMicSettings<std::string>("Microphone", "");
const std::array<Info<bool>, EMULATED_LOGITECH_MIC_COUNT> MAIN_LOGITECH_MIC_MUTED =
    LogitechMicSettings<bool>("Muted", false);
const std::array<Info<s16>, EMULATED_LOGITECH_MIC_COUNT> MAIN_LOGITECH_MIC_VOLUME_MODIFIER =
    LogitechMicSettings<s16>("VolumeModifier", 0);

// Triforce networking: arcade cabinets talk to fixed LAN addresses, which are redirected to the
// local machine. Each entry is "<from>=<to> <description>", and the entries are comma-separated.
static std::string DefaultTriforceRedirections()
{
  static constexpr std::string_view redirections[] = {
      // Mario Kart Arcade GP 1 and 2: four linked cabinets on one PC, each seat with its own
      // static ports...
      "192.168.29.150:5000-5008=127.0.0.1:5000-5008 MarioKart Seat #1 Static",
      "192.168.29.151:5000-5008=127.0.0.1:5010-5018 MarioKart Seat #2 Static",
      "192.168.29.152:5000-5008=127.0.0.1:5020-5028 MarioKart Seat #3 Static",
      "192.168.29.153:5000-5008=127.0.0.1:5030-5038 MarioKart Seat #4 Static",
      // ...and its own ephemeral port range, so incoming connections can be told apart...
      "192.168.29.150=127.0.0.1:50000-50999 MarioKart Seat #1 Ephemeral",
      "192.168.29.151=127.0.0.1:51000-51999 MarioKart Seat #2 Ephemeral",
      "192.168.29.152=127.0.0.1:52000-52999 MarioKart Seat #3 Ephemeral",
      "192.168.29.153=127.0.0.1:53000-53999 MarioKart Seat #4 Ephemeral",
      // ...and the namcam2 cameras.
      "192.168.29.104-107=127.0.0.1 MarioKart namcam2",
      // CyCraft Connect.
      "192.168.11.0/24=127.0.0.1 CyCraft",
      // The Key of Avalon (listed for reference; it needs more than this to work).
      "192.168.13.0/24=127.0.0.1 The Key of Avalon",
      // Sega ALL.Net.
      "192.168.150.0/24=127.0.0.1 ALL.Net",
  };
  return fmt::to_string(fmt::join(redirections, ","));
}

const Info<std::string> MAIN_TRIFORCE_IP_REDIRECTIONS{MainKey("Core", "TriforceIPRedirections"),
                                                      DefaultTriforceRedirections()};

// Wii Remote speaker audio routing (also stored in [Core]).

const Info<bool> MAIN_WIIMOTE_AUDIO_ROUTING_ENABLED{MainKey("Core", "WiimoteAudioRoutingEnabled"),
                                                    false};

namespace
{
template <typename T>
std::array<Info<T>, WIIMOTE_SPEAKER_COUNT> WiimoteAudioSettings(std::string_view suffix, T fallback)
{
  const auto make = [&](int number) {
    return Info<T>{MainKey("Core", fmt::format("Wiimote{}AudioOutput{}", number, suffix)),
                   fallback};
  };
  return {make(1), make(2), make(3), make(4)};
}
}  // namespace

const std::array<Info<bool>, WIIMOTE_SPEAKER_COUNT> MAIN_WIIMOTE_AUDIO_OUTPUT_ENABLED =
    WiimoteAudioSettings<bool>("Enabled", false);
const std::array<Info<std::string>, WIIMOTE_SPEAKER_COUNT> MAIN_WIIMOTE_AUDIO_OUTPUT_DEVICE =
    WiimoteAudioSettings<std::string>("Device", "");

// GameCube region helpers.

DiscIO::Region ToGameCubeRegion(DiscIO::Region region)
{
  // There never was a Korean GameCube; Korean consoles were sold as NTSC-J units. Memory card
  // and IPL paths therefore treat an NTSC-K Wii as NTSC-J.
  return region == DiscIO::Region::NTSC_K ? DiscIO::Region::NTSC_J : region;
}

const char* GetDirectoryForRegion(DiscIO::Region region, RegionDirectoryStyle style)
{
  if (region == DiscIO::Region::Unknown)
    region = ToGameCubeRegion(Config::Get(Config::MAIN_FALLBACK_REGION));

  const char* const japan = style == RegionDirectoryStyle::Legacy ? JAP_DIR : JPN_DIR;
  switch (region)
  {
  case DiscIO::Region::NTSC_U:
    return USA_DIR;
  case DiscIO::Region::PAL:
    return EUR_DIR;
  case DiscIO::Region::DEV:
    return DEV_DIR;
  case DiscIO::Region::NTSC_J:
    return japan;
  case DiscIO::Region::NTSC_K:
    ASSERT_MSG(BOOT, false, "NTSC-K is not a valid GameCube region");
    return japan;
  default:
    ASSERT_MSG(BOOT, false, "Default case should not be reached");
    return EUR_DIR;
  }
}

std::string GetBootROMPath(const std::string& region_directory)
{
  // A user-supplied IPL in User/GC/<region>/ wins over the one in Sys/GC/<region>/.
  const std::string user_copy =
      File::GetUserPath(D_GCUSER_IDX) + DIR_SEP + region_directory + DIR_SEP GC_IPL;
  if (File::Exists(user_copy))
    return user_copy;
  return File::GetSysDirectory() + GC_SYS_DIR + DIR_SEP + region_directory + DIR_SEP GC_IPL;
}

namespace
{
struct RegionTag
{
  std::string_view tag;
  DiscIO::Region region;
};

// Strips a trailing region tag (one of `tags`) from `text`, returning the region it named.
std::optional<DiscIO::Region> StripRegionTag(std::string_view& text,
                                             std::span<const RegionTag> tags)
{
  for (const RegionTag& candidate : tags)
  {
    if (text.ends_with(candidate.tag))
    {
      text.remove_suffix(candidate.tag.size());
      return candidate.region;
    }
  }
  return std::nullopt;
}

DiscIO::Region ChooseRegion(std::optional<DiscIO::Region> requested,
                            std::optional<DiscIO::Region> from_path)
{
  if (requested)
    return *requested;
  if (from_path)
    return *from_path;
  return Config::Get(Config::MAIN_FALLBACK_REGION);
}
}  // namespace

std::string GetMemcardPath(ExpansionInterface::Slot slot, std::optional<DiscIO::Region> region,
                           u16 size_mb)
{
  return GetMemcardPath(Config::Get(GetInfoForMemcardPath(slot)), slot, region, size_mb);
}

std::string GetMemcardPath(std::string configured_filename, ExpansionInterface::Slot slot,
                           std::optional<DiscIO::Region> region, u16 size_mb)
{
  // Cards smaller than the 2043-block size carry their block count in the name: "x.USA.59.raw".
  std::string size_suffix;
  if (size_mb < Memcard::MBIT_SIZE_MEMORY_CARD_2043)
    size_suffix = fmt::format(".{}", Memcard::MbitToFreeBlocks(size_mb));

  if (configured_filename.empty())
  {
    const char* const region_dir =
        GetDirectoryForRegion(ToGameCubeRegion(ChooseRegion(region, std::nullopt)));
    const char* const card_name = slot == ExpansionInterface::Slot::A ? GC_MEMCARDA : GC_MEMCARDB;
    return fmt::format("{}{}.{}{}.raw", File::GetUserPath(D_GCUSER_IDX), card_name, region_dir,
                       size_suffix);
  }

  // A configured path looks like "/dir/name.<REGION>.raw". The region tag (if any) is replaced by
  // the region being booted; without one, a tag is inserted in front of the extension.
  std::string folder, stem, extension;
  UnifyPathSeparators(configured_filename);
  SplitPath(configured_filename, &folder, &stem, &extension);

  static constexpr RegionTag tags[] = {
      {"." USA_DIR, DiscIO::Region::NTSC_U},
      {"." JAP_DIR, DiscIO::Region::NTSC_J},
      {"." EUR_DIR, DiscIO::Region::PAL},
      {"." DEV_DIR, DiscIO::Region::DEV},
  };
  std::string_view bare_stem = stem;
  const std::optional<DiscIO::Region> tagged = StripRegionTag(bare_stem, tags);

  const DiscIO::Region chosen = ChooseRegion(region, tagged);
  return fmt::format("{}{}.{}{}{}", folder, bare_stem,
                     GetDirectoryForRegion(ToGameCubeRegion(chosen)), size_suffix, extension);
}

bool IsDefaultMemcardPathConfigured(ExpansionInterface::Slot slot)
{
  return Config::Get(GetInfoForMemcardPath(slot)).empty();
}

std::string GetGCIFolderPath(ExpansionInterface::Slot slot, std::optional<DiscIO::Region> region)
{
  return GetGCIFolderPath(Config::Get(GetInfoForGCIPath(slot)), slot, region);
}

std::string GetGCIFolderPath(std::string configured_folder, ExpansionInterface::Slot slot,
                             std::optional<DiscIO::Region> region)
{
  if (configured_folder.empty())
  {
    const char* const region_dir =
        GetDirectoryForRegion(ToGameCubeRegion(ChooseRegion(region, std::nullopt)));
    return fmt::format("{}{}/Card {}", File::GetUserPath(D_GCUSER_IDX), region_dir,
                       slot == ExpansionInterface::Slot::A ? 'A' : 'B');
  }

  // A configured folder looks like "/dir/<REGION>" (modern region names). The region folder is
  // replaced by the region being booted, or appended if there is none.
  UnifyPathSeparators(configured_folder);
  while (configured_folder.ends_with('/'))
    configured_folder.pop_back();

  static constexpr RegionTag tags[] = {
      {"/" USA_DIR, DiscIO::Region::NTSC_U},
      {"/" JPN_DIR, DiscIO::Region::NTSC_J},
      {"/" EUR_DIR, DiscIO::Region::PAL},
      {"/" DEV_DIR, DiscIO::Region::DEV},
  };
  std::string_view parent = configured_folder;
  const std::optional<DiscIO::Region> tagged = StripRegionTag(parent, tags);

  const DiscIO::Region chosen = ChooseRegion(region, tagged);
  return fmt::format("{}/{}", parent,
                     GetDirectoryForRegion(ToGameCubeRegion(chosen), RegionDirectoryStyle::Modern));
}

bool IsDefaultGCIFolderPathConfigured(ExpansionInterface::Slot slot)
{
  return Config::Get(GetInfoForGCIPath(slot)).empty();
}

bool AreCheatsEnabled()
{
  return Config::Get(::Config::MAIN_ENABLE_CHEATS);
}

bool IsDebuggingEnabled()
{
  // The debugger stays off while achievements hardcore mode is active.
  if (!Config::Get(::Config::MAIN_ENABLE_DEBUGGING))
    return false;
  return !AchievementManager::GetInstance().IsHardcoreModeActive();
}

}  // namespace Config
