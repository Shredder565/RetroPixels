// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/Graphics/PostProcessingConfigWindow.h"

#include <cmath>
#include <cstdlib>
#include <vector>

#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSlider>
#include <QString>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QWidget>

#include "DolphinQt/Config/Graphics/EnhancementsWidget.h"

#include "VideoCommon/PostProcessing.h"
#include "VideoCommon/Present.h"

namespace
{
using OptionType = VideoCommon::PostProcessingConfiguration::ConfigurationOption::OptionType;

QSlider* NewStepSlider(int steps, int position, int tick)
{
  auto* const slider = new QSlider(Qt::Orientation::Horizontal);
  slider->setMinimum(0);
  slider->setMaximum(steps);
  slider->setValue(position);
  slider->setTickInterval(tick);
  return slider;
}

QLineEdit* NewValueField(const QString& text)
{
  auto* const field = new QLineEdit(text);
  field->setEnabled(false);
  return field;
}
}  // namespace

PostProcessingConfigWindow::PostProcessingConfigWindow(EnhancementsWidget* parent,
                                                       const std::string& shader)
    : QDialog(parent)
{
  // While a game runs, edit the live configuration so changes show at once.
  VideoCommon::PostProcessing* const live =
      g_presenter ? g_presenter->GetPostProcessor() : nullptr;
  if (live != nullptr)
  {
    m_config = live->GetConfig();
  }
  else
  {
    m_own_config = std::make_unique<VideoCommon::PostProcessingConfiguration>();
    m_config = m_own_config.get();
    m_config->LoadShader(shader);
  }

  setWindowTitle(tr("Post-Processing Shader Configuration"));

  BuildTree();
  BuildTabs();
  connect(m_buttons, &QDialogButtonBox::accepted, this, &PostProcessingConfigWindow::accept);

  adjustSize();
}

PostProcessingConfigWindow::~PostProcessingConfigWindow()
{
  m_config->SaveOptionsConfiguration();
}

// Hangs every option under the option it depends on, in option name order.
void PostProcessingConfigWindow::BuildTree()
{
  std::unordered_map<std::string, OptionNode*> by_name;
  for (const auto& [name, option] : m_config->GetOptions())
  {
    auto node = std::make_unique<OptionNode>();
    node->option = &option;
    by_name[name] = node.get();
    m_nodes.push_back(std::move(node));
  }

  for (const std::unique_ptr<OptionNode>& node : m_nodes)
  {
    const std::string& depends_on = node->option->m_dependent_option;
    const auto parent = depends_on.empty() ? by_name.end() : by_name.find(depends_on);
    if (parent == by_name.end())
      m_roots.push_back(node.get());
    else
      parent->second->dependants.push_back(node.get());
  }
}

void PostProcessingConfigWindow::BuildTabs()
{
  m_tabs = new QTabWidget();
  auto* const general = new QWidget(m_tabs);
  auto* const general_grid = new QGridLayout(general);

  u32 general_row = 0;
  bool general_used = false;
  for (OptionNode* const root : m_roots)
  {
    if (root->dependants.empty())
    {
      general_used = true;
      general_row = AddOptionRow(*root, general_grid, general_row);
      continue;
    }

    // The option first, then its direct dependants.
    auto* const page = new QWidget(m_tabs);
    auto* const grid = new QGridLayout(page);
    u32 row = AddOptionRow(*root, grid, 0);
    for (OptionNode* const dependant : root->dependants)
      row = AddOptionRow(*dependant, grid, row);
    m_tabs->addTab(page, QString::fromStdString(root->option->m_gui_name));

    if (root->option->m_type == OptionType::Bool)
      SetDependantsEnabled(*root, root->checkbox->isChecked());
  }

  if (general_used)
    m_tabs->insertTab(0, general, tr("General"));

  m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok);
  auto* const column = new QVBoxLayout(this);
  column->addWidget(m_tabs);
  column->addWidget(m_buttons);
}

u32 PostProcessingConfigWindow::AddOptionRow(OptionNode& node, QGridLayout* grid, u32 row)
{
  grid->addWidget(new QLabel(QString::fromStdString(node.option->m_gui_name)),
                  static_cast<int>(row), 0);
  switch (node.option->m_type)
  {
  case OptionType::Bool:
    return AddBoolRow(node, grid, row);
  case OptionType::Float:
    return AddFloatRows(node, grid, row);
  case OptionType::Integer:
    return AddIntegerRows(node, grid, row);
  }
  std::abort();
}

u32 PostProcessingConfigWindow::AddBoolRow(OptionNode& node, QGridLayout* grid, u32 row)
{
  node.checkbox = new QCheckBox();
  node.checkbox->setChecked(node.option->m_bool_value);
  connect(node.checkbox, &QCheckBox::toggled, this,
          [this, &node](bool checked) { StoreBool(node, checked); });
  grid->addWidget(node.checkbox, static_cast<int>(row), 2);
  return row + 1;
}

// Sliders count steps from the minimum. The step count is rounded up, so
// an odd step size still reaches the maximum. Options with several
// components take one grid row per component, plus one.
u32 PostProcessingConfigWindow::AddIntegerRows(OptionNode& node, QGridLayout* grid, u32 row)
{
  const Option& option = *node.option;
  const size_t components = option.m_integer_values.size();
  for (size_t i = 0; i < components; ++i)
  {
    const s32 low = option.m_integer_min_values[i];
    const double step = static_cast<double>(option.m_integer_step_values[i]);
    const double span = option.m_integer_max_values[i] - low;
    const int steps = static_cast<int>(std::ceil(span / step));
    const int position = static_cast<int>(std::round((option.m_integer_values[i] - low) / step));

    QSlider* const slider = NewStepSlider(steps, position, static_cast<int>(span / steps));
    connect(slider, &QSlider::valueChanged, this, [this, &node](int) { StoreIntegers(node); });
    QLineEdit* const field = NewValueField(QString::number(option.m_integer_values[i]));

    grid->addWidget(slider, static_cast<int>(row), 1);
    grid->addWidget(field, static_cast<int>(row), 2);
    node.sliders.push_back(slider);
    node.value_fields.push_back(field);
    if (components > 1)
      ++row;
  }
  return row + 1;
}

u32 PostProcessingConfigWindow::AddFloatRows(OptionNode& node, QGridLayout* grid, u32 row)
{
  const Option& option = *node.option;
  const size_t components = option.m_float_values.size();
  for (size_t i = 0; i < components; ++i)
  {
    const float low = option.m_float_min_values[i];
    const float step = option.m_float_step_values[i];
    const float span = option.m_float_max_values[i] - low;
    const int steps = static_cast<int>(std::ceil(span / step));
    const int position = static_cast<int>(std::round((option.m_float_values[i] - low) / step));

    QSlider* const slider =
        NewStepSlider(steps, position, static_cast<int>(span / static_cast<float>(steps)));
    connect(slider, &QSlider::valueChanged, this, [this, &node](int) { StoreFloats(node); });
    QLineEdit* const field =
        NewValueField(QString::asprintf("%f", static_cast<double>(option.m_float_values[i])));

    grid->addWidget(slider, static_cast<int>(row), 1);
    grid->addWidget(field, static_cast<int>(row), 2);
    node.sliders.push_back(slider);
    node.value_fields.push_back(field);
    if (components > 1)
      ++row;
  }
  return row + 1;
}

// Enables or disables every option below node, at any depth. Options more
// than one level down have no controls of their own.
void PostProcessingConfigWindow::SetDependantsEnabled(const OptionNode& node, bool enabled)
{
  for (const OptionNode* const dependant : node.dependants)
  {
    if (dependant->option->m_type == OptionType::Bool)
    {
      if (dependant->checkbox != nullptr)
        dependant->checkbox->setEnabled(enabled);
    }
    else
    {
      for (QSlider* const slider : dependant->sliders)
        slider->setEnabled(enabled);
    }
    SetDependantsEnabled(*dependant, enabled);
  }
}

void PostProcessingConfigWindow::StoreBool(OptionNode& node, bool value)
{
  m_config->SetOptionb(node.option->m_option_name, value);
  SetDependantsEnabled(node, value);
}

void PostProcessingConfigWindow::StoreIntegers(OptionNode& node)
{
  const Option& option = m_config->GetOption(node.option->m_option_name);
  for (size_t i = 0; i < option.m_integer_values.size(); ++i)
  {
    const s32 value = option.m_integer_step_values[i] * node.sliders[i]->value() +
                      option.m_integer_min_values[i];
    m_config->SetOptioni(option.m_option_name, static_cast<int>(i), value);
    node.value_fields[i]->setText(QString::number(value));
  }
}

void PostProcessingConfigWindow::StoreFloats(OptionNode& node)
{
  const Option& option = m_config->GetOption(node.option->m_option_name);
  for (size_t i = 0; i < option.m_float_values.size(); ++i)
  {
    const float value =
        option.m_float_step_values[i] * static_cast<float>(node.sliders[i]->value()) +
        option.m_float_min_values[i];
    m_config->SetOptionf(option.m_option_name, static_cast<int>(i), value);
    node.value_fields[i]->setText(QString::asprintf("%f", static_cast<double>(value)));
  }
}
