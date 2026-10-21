// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/Mapping/MappingButton.h"

#include <QMouseEvent>
#include <QString>
#include <QStringList>

#include "DolphinQt/Config/Mapping/IOWindow.h"
#include "DolphinQt/Config/Mapping/MappingWidget.h"
#include "DolphinQt/Config/Mapping/MappingWindow.h"
#include "InputCommon/ControlReference/ControlReference.h"
#include "InputCommon/ControllerEmu/ControllerEmu.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"

namespace
{
// Range given back to a control when its mapping is cleared (100%).
constexpr double CLEARED_RANGE = 1.0;

// The button text: the expression with each `quoted control` reduced to its control name
// (":" in front when it names a device), blanks outside the quotes dropped, the backticks
// and line breaks removed and '&' doubled so Qt does not take it as a mnemonic. A
// expression with a syntax error is shown as written, apart from the last three steps.
QString ButtonText(ControlReference* reference)
{
  const bool parsed =
      reference->GetParseStatus() != ciface::ExpressionParser::ParseStatus::SyntaxError;

  QString expression;
  {
    const auto state_lock = ControllerEmu::EmulatedController::GetStateLock();
    expression = QString::fromStdString(reference->GetExpression());
  }

  // Splitting on '`' puts the quoted controls at the odd positions.
  const QStringList pieces = expression.split(QLatin1Char{'`'});
  QString text;
  for (qsizetype i = 0; i < pieces.size(); ++i)
  {
    QString piece = pieces[i];
    if (parsed)
    {
      if (i % 2 == 1)
      {
        ciface::ExpressionParser::ControlQualifier qualifier;
        qualifier.FromString(piece.toStdString());
        piece = QString::fromStdString(qualifier.control_name);
        if (qualifier.has_device)
          piece.prepend(QLatin1Char{':'});
      }
      else
      {
        piece.remove(QLatin1Char{' '});
      }
    }
    text += piece;
  }

  for (const char line_char : {'\t', '\n', '\r'})
    text.remove(QLatin1Char{line_char});
  text.replace(QLatin1Char{'&'}, QStringLiteral("&&"));
  return text;
}
}  // namespace

MappingButton::MappingButton(MappingWidget* widget, ControlReference* ref, ControlType type)
    : ElidedButton{ButtonText(ref)}, m_window{widget->GetParent()}, m_reference{ref}, m_type{type}
{
  if (m_reference->IsInput())
  {
    setToolTip(
        tr("Left-click to detect input.\nMiddle-click to clear.\nRight-click for more options."));
  }
  else
  {
    setToolTip(tr("Left/Right-click to configure output.\nMiddle-click to clear."));
  }

  connect(this, &MappingButton::clicked, this, &MappingButton::OnLeftClick);
  connect(widget, &MappingWidget::ConfigChanged, this, &MappingButton::ConfigChanged);
  connect(this, &MappingButton::ConfigChanged, this, [this] { setText(ButtonText(m_reference)); });
}

ControlReference* MappingButton::GetControlReference()
{
  return m_reference;
}

MappingButton::ControlType MappingButton::GetControlType() const
{
  return m_type;
}

void MappingButton::OnLeftClick()
{
  // Outputs cannot be detected, so a click edits them directly.
  if (m_reference->IsInput())
    emit m_window->QueueInputDetection(this);
  else
    OpenExpressionEditor();
}

void MappingButton::OpenExpressionEditor()
{
  emit m_window->CancelMapping();

  const auto kind = m_reference->IsInput() ? IOWindow::Type::Input : IOWindow::Type::Output;
  IOWindow editor(m_window, m_window->GetController(), m_reference, kind);
  editor.exec();

  emit ConfigChanged();
  emit m_window->Save();
}

void MappingButton::ClearMapping()
{
  m_reference->range = CLEARED_RANGE;
  m_reference->SetExpression("");
  m_window->GetController()->UpdateSingleControlReference(g_controller_interface, m_reference);

  emit m_window->Save();
  emit m_window->UnQueueInputDetection(this);
}

void MappingButton::mouseReleaseEvent(QMouseEvent* event)
{
  const Qt::MouseButton button = event->button();
  if (button == Qt::MouseButton::MiddleButton)
    ClearMapping();
  else if (button == Qt::MouseButton::RightButton)
    OpenExpressionEditor();
  else
    QPushButton::mouseReleaseEvent(event);
}
