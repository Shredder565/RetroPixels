// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <concepts>
#include <span>

#include <QWidget>

#include "Common/WorkQueueThread.h"
#include "Core/USBUtils.h"

class QAction;
class QCheckBox;
class QComboBox;
class QGridLayout;
class QGroupBox;
class QHBoxLayout;
class QLabel;
class QPushButton;
class QRadioButton;
class QToolButton;

namespace Core
{
enum class State;
}

// The "Wii Remotes" box of the controller settings: Bluetooth passthrough
// or emulation, the four Wii Remote slots and real-remote options.
class WiimoteControllersWidget final : public QWidget
{
  Q_OBJECT
public:
  explicit WiimoteControllersWidget(QWidget* parent);
  ~WiimoteControllersWidget() override;

private:
  static constexpr size_t SLOTS = 4;

  void BuildLayout();
  void WireUp();
  void ShowConfig(Core::State state);
  void StoreConfig();
  void StoreAndReload();
  void RefreshAdapterControls(Core::State state);

  void ScanForAdapters();
  void OnAdaptersScanned(std::span<const USBUtils::DeviceInfo> devices);
  void OnAdapterPicked(int row);
  void SyncPassthroughRemotes();
  void ResetPassthroughPairings();
  void RefreshRealRemotes();
  void ConfigureSlot(size_t slot);

#if defined(_WIN32)
  void RunWithSpinner(std::invocable<> auto task);
  void PairHostRemotes();
  void ForgetHostRemotes();
#endif

  QGroupBox* m_group;
  QGridLayout* m_grid;
  std::array<QLabel*, SLOTS> m_slot_labels;
  std::array<QComboBox*, SLOTS> m_slot_sources;
  std::array<QPushButton*, SLOTS> m_slot_configure;
  std::array<QLabel*, 2> m_passthrough_captions;

  Common::AsyncWorkThreadSP m_scan_thread;
  bool m_scanning = false;

  QRadioButton* m_emulate_bt;
  QRadioButton* m_passthrough_bt;
  QLabel* m_adapter_caption;
  QComboBox* m_adapters;
  QPushButton* m_rescan_adapters;
  QPushButton* m_sync;
  QPushButton* m_reset;
  QCheckBox* m_continuous_scanning;
  QCheckBox* m_balance_board;
  QCheckBox* m_speaker_data;
  QCheckBox* m_for_ciface;
  QToolButton* m_refresh;
  QLabel* m_spinner;
};
