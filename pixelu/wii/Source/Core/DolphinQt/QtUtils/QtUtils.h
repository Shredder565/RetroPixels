// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <string_view>

#include <QStyle>

class QDateTimeEdit;
class QLabel;
class QWidget;

namespace QtUtils
{
// Makes a date/time edit show four-digit years whatever the locale's format says.
void ShowFourDigitYear(QDateTimeEdit* widget);

// A widget with a standard style icon on the left of `label`.
QWidget* CreateIconWarning(QWidget* parent, QStyle::StandardPixmap standard_pixmap, QLabel* label);

// Resizes the widget to its size hint, but at most 90% of the available screen area, and centres
// it on its parent's window.
void AdjustSizeWithinScreen(QWidget* widget);

// Centres the widget over its parent's top-level window. Call it once the size is final.
void CenterOnParentWindow(QWidget* widget);

// A widget whose preferred size is its minimum size hint, for widgets (such as QListWidget) that
// otherwise ask for a lot of room even when empty. Don't use it with widgets whose
// minimumSizeHint itself calls sizeHint (QPushButton, for one): that would recurse forever.
template <typename Widget>
class MinimumSizeHintWidget : public Widget
{
public:
  using Widget::Widget;

  QSize sizeHint() const override { return Widget::minimumSizeHint(); }
};

// Opens the folder that contains the file, selecting the file where the platform allows it.
void ShowFileInFolder(std::string_view file_path);

}  // namespace QtUtils
