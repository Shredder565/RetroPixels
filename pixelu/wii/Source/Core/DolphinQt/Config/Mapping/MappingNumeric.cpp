// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/Mapping/MappingNumeric.h"

#include <limits>

#include <QSignalBlocker>

#include "DolphinQt/Config/Mapping/MappingWidget.h"

namespace
{
// Appended to editors whose setting is driven by an input expression.
QString ExpressionMarker()
{
  return QString::fromUtf8("🎮");
}
}  // namespace

MappingDouble::MappingDouble(MappingWidget* parent, ControllerEmu::NumericSetting<double>* setting)
    : QDoubleSpinBox(parent), m_setting(*setting)
{
  setDecimals(2);

  const char* const description = m_setting.GetUIDescription();
  if (description != nullptr)
    setToolTip(tr(description));

  connect(this, &QDoubleSpinBox::valueChanged, this, [this, parent](double new_value) {
    m_setting.SetValue(new_value);
    ShowSetting();
    parent->SaveSettings();
  });
  connect(parent, &MappingWidget::ConfigChanged, this, &MappingDouble::ShowSetting);
  connect(parent, &MappingWidget::Update, this, &MappingDouble::FollowLiveValue);
}

void MappingDouble::fixup(QString& input) const
{
  input = QString::number(m_setting.GetDefaultValue());
}

void MappingDouble::ShowSetting()
{
  const QSignalBlocker no_feedback(this);

  QString suffix;
  if (const char* const unit = m_setting.GetUISuffix(); unit != nullptr)
    suffix = QLatin1Char{' '} + tr(unit);

  const bool plain_value = m_setting.IsSimpleValue();
  if (plain_value)
  {
    setRange(m_setting.GetMinValue(), m_setting.GetMaxValue());
  }
  else
  {
    // An expression can produce any value.
    const double unbounded = std::numeric_limits<double>::infinity();
    setRange(-unbounded, unbounded);
    suffix += QLatin1Char{' '} + ExpressionMarker();
  }
  setButtonSymbols(plain_value ? ButtonSymbols::UpDownArrows : ButtonSymbols::NoButtons);
  setSuffix(suffix);
  setValue(m_setting.GetValue());
}

void MappingDouble::FollowLiveValue()
{
  // Plain values do not change by themselves; and do not disturb the user while typing.
  if (m_setting.IsSimpleValue() || hasFocus())
    return;

  const QSignalBlocker no_feedback(this);
  setValue(m_setting.GetValue());
}

MappingBool::MappingBool(MappingWidget* parent, ControllerEmu::NumericSetting<bool>* setting)
    : QCheckBox(parent), m_setting(*setting)
{
  const char* const description = m_setting.GetUIDescription();
  if (description != nullptr)
    setToolTip(tr(description));

  const auto on_user_toggle = [this, parent](auto state) {
    m_setting.SetValue(state != 0);
    ShowSetting();
    parent->SaveSettings();
  };
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
  connect(this, &QCheckBox::checkStateChanged, this, on_user_toggle);
#else
  connect(this, &QCheckBox::stateChanged, this, on_user_toggle);
#endif
  connect(parent, &MappingWidget::ConfigChanged, this, &MappingBool::ShowSetting);
  connect(parent, &MappingWidget::Update, this, &MappingBool::FollowLiveValue);

  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
}

void MappingBool::ShowSetting()
{
  const QSignalBlocker no_feedback(this);
  setText(m_setting.IsSimpleValue() ? QString{} : ExpressionMarker());
  setChecked(m_setting.GetValue());
}

void MappingBool::FollowLiveValue()
{
  if (m_setting.IsSimpleValue())
    return;

  const QSignalBlocker no_feedback(this);
  setChecked(m_setting.GetValue());
}
