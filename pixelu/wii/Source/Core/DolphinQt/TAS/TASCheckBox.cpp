// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/TAS/TASCheckBox.h"

#include <QMouseEvent>

#include "Core/Movie.h"
#include "Core/System.h"
#include "DolphinQt/QtUtils/QueueOnObject.h"
#include "DolphinQt/TAS/TASInputWindow.h"

namespace
{
u64 MovieFrame()
{
  return Core::System::GetInstance().GetMovie().GetCurrentFrame();
}
}  // namespace

TASCheckBox::TASCheckBox(const QString& text, TASInputWindow* parent)
    : QCheckBox(text, parent), m_window(parent)
{
  setTristate(true);

#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
  connect(this, &QCheckBox::checkStateChanged, this, &TASCheckBox::OnUIValueChanged);
#else
  connect(this, &QCheckBox::stateChanged, this, &TASCheckBox::OnUIValueChanged);
#endif
}

bool TASCheckBox::GetValue() const
{
  const auto shown = static_cast<Qt::CheckState>(m_state.GetValue());
  if (shown != Qt::PartiallyChecked)
    return shown != Qt::Unchecked;

  // Turbo: held for the first m_turbo_down_frames of every cycle.
  const u64 since_origin = MovieFrame() - static_cast<u64>(m_turbo_origin_frame);
  const u64 cycle_position = since_origin % static_cast<u64>(m_turbo_cycle_frames);
  return static_cast<int>(cycle_position) < m_turbo_down_frames;
}

void TASCheckBox::OnControllerValueChanged(bool new_value)
{
  const int as_state = new_value ? Qt::Checked : Qt::Unchecked;
  if (!m_state.OnControllerValueChanged(as_state))
    return;
  QueueOnObject(this, &TASCheckBox::ApplyControllerValueChange);
}

void TASCheckBox::mousePressEvent(QMouseEvent* event)
{
  if (event->button() == Qt::RightButton)
  {
    if (checkState() == Qt::PartiallyChecked)
    {
      setCheckState(Qt::Unchecked);
      return;
    }

    m_turbo_origin_frame = static_cast<int>(MovieFrame());
    m_turbo_down_frames = m_window->GetTurboPressFrames();
    m_turbo_cycle_frames = m_turbo_down_frames + m_window->GetTurboReleaseFrames();
    setCheckState(Qt::PartiallyChecked);
    return;
  }

  setChecked(!isChecked());
}

#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
void TASCheckBox::OnUIValueChanged(Qt::CheckState new_value)
#else
void TASCheckBox::OnUIValueChanged(int new_value)
#endif
{
  m_state.OnUIValueChanged(new_value);
}

void TASCheckBox::ApplyControllerValueChange()
{
  // The new state came from the controller, so don't echo it back as a user change.
  const QSignalBlocker quiet(this);
  setCheckState(static_cast<Qt::CheckState>(m_state.ApplyControllerValueChange()));
}
