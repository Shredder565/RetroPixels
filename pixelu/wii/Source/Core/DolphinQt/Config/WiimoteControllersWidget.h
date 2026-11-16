// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <concepts>

#include <QWidget>

class QAction;
class QCheckBox;
class QComboBox;
class QGridLayout;
class QGroupBox;
class QHBoxLayout;
class QLabel;
class QPushButton;
class QToolButton;

namespace Core
{
enum class State;
}

// The "Wii Remotes" box of the controller settings: the four Wii Remote
// slots and real-remote options.
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

  QCheckBox* m_continuous_scanning;
  QCheckBox* m_balance_board;
  QCheckBox* m_speaker_data;
  QCheckBox* m_for_ciface;
  QToolButton* m_refresh;
  QLabel* m_spinner;
};
