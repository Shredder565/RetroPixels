// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <optional>

#include <QString>

#include "DolphinQt/Config/ToolTipControls/BalloonTip.h"

class QEnterEvent;
class QEvent;
class QHideEvent;
class QTimerEvent;

// Hover time in milliseconds before a control shows its balloon.
constexpr int TOOLTIP_DELAY = 300;

// Mixes a delayed BalloonTip into any QWidget subclass. The subclass says
// where the balloon's arrow should point, in its parent's coordinates.
template <class Derived>
class ToolTipWidget : public Derived
{
public:
  using Derived::Derived;

  void SetTitle(QString title) { m_title = std::move(title); }
  void SetDescription(QString description) { m_description = std::move(description); }

private:
  virtual QPoint GetToolTipPosition() const = 0;

  void enterEvent(QEnterEvent* const event) override
  {
    // Coming back from the balloon itself (or a hover already pending)
    // must not restart the countdown.
    const bool pending = m_hover_timer.has_value();
    if (!pending && !BalloonTip::IsWidgetBalloonTipActive(*this))
      m_hover_timer = this->startTimer(TOOLTIP_DELAY);
    Derived::enterEvent(event);
  }

  void leaveEvent(QEvent* const event) override
  {
    // The balloon may sit on top of part of this control; leaving onto it
    // while still inside our rectangle keeps it up (the balloon then
    // watches the cursor itself).
    const bool onto_own_balloon =
        BalloonTip::IsCursorInsideWidgetBoundingBox(*this) && BalloonTip::IsCursorOnBalloonTip();
    if (!onto_own_balloon)
      CancelHover();
    Derived::leaveEvent(event);
  }

  void hideEvent(QHideEvent* const event) override
  {
    CancelHover();
    Derived::hideEvent(event);
  }

  void timerEvent(QTimerEvent* const event) override
  {
    StopHoverTimer();
    const QPoint arrow_tip = this->parentWidget()->mapToGlobal(GetToolTipPosition());
    BalloonTip::ShowBalloon(m_title, m_description, arrow_tip, this);
    Derived::timerEvent(event);
  }

  void StopHoverTimer()
  {
    if (!m_hover_timer)
      return;
    this->killTimer(*m_hover_timer);
    m_hover_timer = std::nullopt;
  }

  void CancelHover()
  {
    StopHoverTimer();
    BalloonTip::HideBalloon();
  }

  std::optional<int> m_hover_timer;
  QString m_title;
  QString m_description;
};
