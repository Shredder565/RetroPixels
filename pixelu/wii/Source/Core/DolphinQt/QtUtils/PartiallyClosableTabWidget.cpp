// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/QtUtils/PartiallyClosableTabWidget.h"

#include <QStyle>
#include <QTabBar>

PartiallyClosableTabWidget::PartiallyClosableTabWidget(QWidget* parent) : QTabWidget(parent)
{
  setTabsClosable(true);
}

void PartiallyClosableTabWidget::setTabUnclosable(int index)
{
  // The close button sits on whichever side the style puts it; remove the button on that side.
  const int side = style()->styleHint(QStyle::SH_TabBar_CloseButtonPosition, nullptr, this);
  tabBar()->setTabButton(index, static_cast<QTabBar::ButtonPosition>(side), nullptr);
}
