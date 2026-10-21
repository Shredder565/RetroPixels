// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QPushButton>
#include <QWidget>

#include "Core/PowerPC/PowerPC.h"

class ConfigBool;
template <typename T>
class ConfigChoiceMap;
class ConfigFloatSlider;
class ConfigSlider;
class ConfigSliderU32;
class QCheckBox;
class QDateTimeEdit;
class QLabel;
class QRadioButton;
class QSlider;
class QVBoxLayout;

namespace Core
{
enum class State;
}

// "Advanced" settings page: CPU core options, timing, clock/VBI/memory overrides, custom RTC
// and the reset-everything button. Most controls lock while a game is running.
class AdvancedPane final : public QWidget
{
  Q_OBJECT
public:
  explicit AdvancedPane(QWidget* parent = nullptr);

private:
  void AddCpuGroup(QVBoxLayout* page);
  void AddTimingGroup(QVBoxLayout* page);
  void AddClockGroup(QVBoxLayout* page, int value_width);
  void AddVbiGroup(QVBoxLayout* page, int value_width);
  void AddMemoryGroup(QVBoxLayout* page, int value_width);
  void AddRtcGroup(QVBoxLayout* page);
  void AddResetGroup(QVBoxLayout* page);
  void HookUpConfigWriters();

  void RefreshState();
  void RefreshClockText();
  void RefreshVbiText();
  void ConfirmAndReset();

  ConfigChoiceMap<PowerPC::CPUCore>* m_cpu_core;
  ConfigBool* m_mmu;
  ConfigBool* m_pause_on_panic;
  ConfigBool* m_write_back_cache;

  ConfigBool* m_clock_override;
  ConfigFloatSlider* m_clock_slider;
  QLabel* m_clock_value;

  ConfigBool* m_vbi_override;
  ConfigFloatSlider* m_vbi_slider;
  QLabel* m_vbi_value;

  ConfigBool* m_memory_override;
  ConfigSliderU32* m_mem1_slider;
  QLabel* m_mem1_value;
  ConfigSliderU32* m_mem2_slider;
  QLabel* m_mem2_value;

  ConfigBool* m_custom_rtc;
  QDateTimeEdit* m_rtc_value;

  QPushButton* m_reset_all;
};
