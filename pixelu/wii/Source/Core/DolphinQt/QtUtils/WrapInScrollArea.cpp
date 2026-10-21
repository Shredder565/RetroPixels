// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/QtUtils/WrapInScrollArea.h"

#include <QFrame>
#include <QLayout>
#include <QPalette>
#include <QScrollArea>
#include <QScrollBar>
#include <QVBoxLayout>
#include <QWidget>

namespace
{
class ContentSizedScrollArea final : public QScrollArea
{
public:
  ContentSizedScrollArea()
  {
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    setWidgetResizable(true);
    // No frame: a framed scroll area looks out of place inside dialogs on Windows.
    setFrameStyle(QFrame::NoFrame);
  }

  QSize sizeHint() const override
  {
    // Leave room for both scroll bars. Otherwise a window that is too small in one direction
    // grows a scroll bar that eats into the other direction, which then needs one too.
    const QSize bars{verticalScrollBar()->sizeHint().width(),
                     horizontalScrollBar()->sizeHint().height()};
    return widget()->sizeHint() + bars;
  }
};
}  // namespace

QWidget* GetWrappedWidget(QWidget* wrapped_widget)
{
  auto* const area = new ContentSizedScrollArea;
  area->setWidget(wrapped_widget);

  // Keep the contents transparent so the dialog's background shows through (matters on macOS).
  area->viewport()->setAutoFillBackground(false);
  wrapped_widget->setAutoFillBackground(false);

  return area;
}

void WrapInScrollArea(QWidget* parent, QLayout* wrapped_layout)
{
  auto* const contents = new QWidget;
  contents->setLayout(wrapped_layout);

  auto* const fill = new QVBoxLayout;
  fill->addWidget(GetWrappedWidget(contents));
  fill->setContentsMargins(0, 0, 0, 0);
  parent->setLayout(fill);
}
