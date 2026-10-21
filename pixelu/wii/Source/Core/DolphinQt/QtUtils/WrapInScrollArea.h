// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

class QLayout;
class QWidget;

// Returns a frameless scroll area holding `wrapped_widget`. The scroll area asks for the size of
// the widget it holds (plus room for scroll bars).
QWidget* GetWrappedWidget(QWidget* wrapped_widget);

// Puts `wrapped_layout` into such a scroll area and makes that fill `parent`.
void WrapInScrollArea(QWidget* parent, QLayout* wrapped_layout);
