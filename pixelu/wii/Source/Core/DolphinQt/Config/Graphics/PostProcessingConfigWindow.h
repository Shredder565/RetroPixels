// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <QDialog>

#include "Common/CommonTypes.h"
#include "VideoCommon/PostProcessing.h"

class EnhancementsWidget;
class QCheckBox;
class QDialogButtonBox;
class QGridLayout;
class QLineEdit;
class QSlider;
class QTabWidget;
class QWidget;

// Edits the [configuration] options of a post-processing shader: a check
// box per bool option and a slider per component of number options.
// Options other options depend on get a tab of their own with their
// dependants; the rest share a "General" tab.
class PostProcessingConfigWindow final : public QDialog
{
  Q_OBJECT
public:
  explicit PostProcessingConfigWindow(EnhancementsWidget* parent, const std::string& shader);
  ~PostProcessingConfigWindow() override;

private:
  using Option = VideoCommon::PostProcessingConfiguration::ConfigurationOption;

  // One option, its controls and the options that depend on it.
  struct OptionNode
  {
    const Option* option = nullptr;
    QCheckBox* checkbox = nullptr;
    std::vector<QSlider*> sliders;
    std::vector<QLineEdit*> value_fields;
    std::vector<OptionNode*> dependants;
  };

  void BuildTree();
  void BuildTabs();
  u32 AddOptionRow(OptionNode& node, QGridLayout* grid, u32 row);
  u32 AddBoolRow(OptionNode& node, QGridLayout* grid, u32 row);
  u32 AddIntegerRows(OptionNode& node, QGridLayout* grid, u32 row);
  u32 AddFloatRows(OptionNode& node, QGridLayout* grid, u32 row);
  static void SetDependantsEnabled(const OptionNode& node, bool enabled);

  void StoreBool(OptionNode& node, bool value);
  void StoreIntegers(OptionNode& node);
  void StoreFloats(OptionNode& node);

  QTabWidget* m_tabs;
  QDialogButtonBox* m_buttons;

  // Either the running presenter's configuration or m_own_config.
  VideoCommon::PostProcessingConfiguration* m_config;
  std::unique_ptr<VideoCommon::PostProcessingConfiguration> m_own_config;
  std::vector<std::unique_ptr<OptionNode>> m_nodes;
  std::vector<OptionNode*> m_roots;
};
