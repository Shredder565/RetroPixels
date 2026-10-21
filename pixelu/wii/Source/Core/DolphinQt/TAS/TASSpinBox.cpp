// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/TAS/TASSpinBox.h"

#include <optional>

#include "DolphinQt/QtUtils/QueueOnObject.h"

namespace
{
std::optional<int> ParseWholeNumber(const QString& text)
{
  bool parsed = false;
  const int number = text.toInt(&parsed);
  if (!parsed)
    return std::nullopt;
  return number;
}
}  // namespace

TASSpinBox::TASSpinBox(QWidget* parent) : QSpinBox(parent)
{
  connect(this, &QSpinBox::valueChanged, this, &TASSpinBox::OnUIValueChanged);
}

int TASSpinBox::GetValue() const
{
  return m_state.GetValue();
}

void TASSpinBox::OnControllerValueChanged(int new_value)
{
  if (!m_state.OnControllerValueChanged(new_value))
    return;
  QueueOnObject(this, &TASSpinBox::ApplyControllerValueChange);
}

void TASSpinBox::OnUIValueChanged(int new_value)
{
  m_state.OnUIValueChanged(new_value);
}

void TASSpinBox::ApplyControllerValueChange()
{
  setValue(m_state.ApplyControllerValueChange());
}

QValidator::State TASSpinBox::validate(QString& input, int& pos) const
{
  const QValidator::State verdict = QSpinBox::validate(input, pos);
  // An out-of-range number is still worth keeping while the user types; fixup() clamps it.
  if (verdict == QValidator::Invalid && ParseWholeNumber(input))
    return QValidator::Intermediate;
  return verdict;
}

void TASSpinBox::fixup(QString& input) const
{
  const std::optional<int> typed = ParseWholeNumber(input);
  if (!typed)
  {
    QSpinBox::fixup(input);
    return;
  }

  if (*typed > maximum())
    input = QString::number(maximum());
  else if (*typed < minimum())
    input = QString::number(minimum());
}
