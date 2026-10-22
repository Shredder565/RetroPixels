// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "Common/Analytics.h"
#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"

#if defined(ANDROID)
#include <functional>
#endif

// The emulator's usage statistics (only sent when the user opted in): which settings are used,
// what hardware, and which rarely used or unemulated features games touch. The report format
// itself is in Common/Analytics.h.

// Unusual things a game can do. Each is reported at most once per game start. The order is part of
// the report format (see the names in DolphinAnalytics.cpp).
enum class GameQuirk
{
  // Rare Wii DI (disc drive) commands.
  UsesDVDLowStopLaser,
  UsesDVDLowOffset,
  UsesDVDLowReadDiskBCA,
  UsesDVDLowRequestDiscStatus,
  UsesDVDLowRequestRetryNumber,
  UsesDVDLowSerMeasControl,
  // A partition open command other than the plain DVDLowOpenPartition.
  UsesDifferentPartitionCommand,
  // The DI interrupt mask commands (stubbed in the Wii SDK).
  UsesDIInterruptMaskCommand,

  // Transform unit and TEV disagreeing on the number of texture coordinates or colors.
  MismatchedGPUTexGensBetweenXFAndBP,
  MismatchedGPUColorsBetweenXFAndBP,

  // WD (local wireless) modes other than the two in common use, and unknown WD ioctls.
  UsesUncommonWDMode,
  UsesWDUnimplementedIOCtl,

  // Graphics commands of a known kind (BP/CP/XF) with an unknown register or meaning.
  UsesUnknownBPCommand,
  UsesUnknownCPCommand,
  UsesUnknownXFCommand,
  // CP commands that documentation and the emulator disagree on.
  UsesMaybeInvalidCPCommand,
  // CP performance counter commands.
  UsesCPPerfCommand,

  // Audio (AX) features that aren't emulated.
  UsesUnimplementedAXCommand,
  UsesAXInitialTimeDelay,
  UsesAXWiimoteLowPass,
  UsesAXWiimoteBiquad,

  // The XF clip disable bits (not emulated).
  SetsXFClipDisableBit0,
  SetsXFClipDisableBit1,
  SetsXFClipDisableBit2,

  // Vertex format mismatches between the command processor and the transform unit.
  MismatchedGPUColorsBetweenCPAndXF,
  MismatchedGPUNormalsBetweenCPAndXF,
  MismatchedGPUTexCoordsBetweenCPAndXF,
  MismatchedGPUMatrixIndicesBetweenCPAndXF,

  // Reading the bounding box registers back (all games set them up, few read them).
  ReadsBoundingBox,

  // Invalid vertex component formats.
  InvalidPositionComponentFormat,
  InvalidNormalComponentFormat,
  InvalidTextureCoordinateComponentFormat,
  InvalidColorComponentFormat,

  Count,
};

class DolphinAnalytics
{
public:
  // The process-wide instance, created on first use.
  static DolphinAnalytics& Instance();
  DolphinAnalytics(const DolphinAnalytics&) = delete;
  DolphinAnalytics& operator=(const DolphinAnalytics&) = delete;
  ~DolphinAnalytics();

#if defined(ANDROID)
  // How device properties are fetched from the Java side.
  static void AndroidSetGetValFunc(std::function<std::string(std::string)> function);
#endif
  // Applies the current opt-in setting (installs or removes the sending backend) and loads the
  // anonymous ID.
  void ReloadConfig();
  // Replaces the anonymous ID with a new random one and saves it.
  void GenerateNewIdentity();

  void ReportDolphinStart(std::string_view ui_type);
  // Also builds the per-game data that later reports of this game carry.
  void ReportGameStart();
  // At most once per quirk and game start.
  void ReportGameQuirk(GameQuirk quirk);

  // The data every report starts with.
  const Common::AnalyticsReportBuilder& BaseBuilder() const { return m_base_builder; }

  struct PerformanceSample
  {
    double speed_ratio;  // See SystemTimers::GetEstimatedEmulationPerformance().
    int num_prims;
    int num_draw_calls;
  };
  // Called every frame. Samples are only collected now and then (a while after the game starts,
  // then every half hour) and sent in batches. Not thread-safe.
  void ReportPerformanceInfo(PerformanceSample sample);

  template <typename T>
  void Send(T report)
  {
    std::lock_guard lock{m_send_mutex};
    m_reporter.Send(report);
  }

private:
  DolphinAnalytics();

  void MakeBaseBuilder();
  void MakePerGameBuilder();

  // A per-purpose ID derived from the anonymous ID, so reports of different kinds can't be linked.
  std::string MakeUniqueId(std::string_view data) const;

  void InitializePerformanceSampling();
  bool ShouldStartPerformanceSampling();

  // Never sent as is; see MakeUniqueId.
  std::string m_unique_id;

  // Sampling schedule: 100 frames, first after 5 minutes, then every 30 minutes, each plus up to
  // 3 minutes of random delay.
  static constexpr int NUM_PERFORMANCE_SAMPLES_PER_REPORT = 100;
  static constexpr int PERFORMANCE_SAMPLING_INITIAL_WAIT_TIME_SECS = 300;
  static constexpr int PERFORMANCE_SAMPLING_WAIT_TIME_JITTER_SECS = 180;
  static constexpr int PERFORMANCE_SAMPLING_INTERVAL_SECS = 1800;

  u64 m_next_sampling_us = 0;
  bool m_sampling = false;
  std::vector<PerformanceSample> m_samples;

  std::array<bool, static_cast<size_t>(GameQuirk::Count)> m_quirk_reported{};

  Common::AnalyticsReportBuilder m_base_builder;
  Common::AnalyticsReportBuilder m_per_game_builder;

  std::mutex m_send_mutex;
  Common::AnalyticsReporter m_reporter;
  Config::ConfigChangedCallbackID m_config_callback{};
  bool m_enabled = false;
};
