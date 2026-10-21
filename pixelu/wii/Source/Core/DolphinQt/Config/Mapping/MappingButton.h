// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "DolphinQt/QtUtils/ElidedButton.h"

class ControlReference;
class MappingWidget;
class MappingWindow;
class QMouseEvent;

// The button of one control on a mapping page. It shows a short form of the control's
// expression. Left click detects an input (or opens the editor of an output), middle click
// clears the mapping and right click opens the expression editor.
class MappingButton : public ElidedButton
{
  Q_OBJECT
public:
  enum class ControlType
  {
    NormalInput,
    ModifierInput,
    Output,
  };

  MappingButton(MappingWidget* widget, ControlReference* ref, ControlType type);

  ControlReference* GetControlReference();
  ControlType GetControlType() const;

signals:
  // Re-reads the expression into the button text.
  void ConfigChanged();
  // Emitted when this input has been mapped and the next one should be detected.
  void QueueNextButtonMapping();

private:
  void mouseReleaseEvent(QMouseEvent* event) override;

  void OnLeftClick();
  void OpenExpressionEditor();
  void ClearMapping();

  MappingWindow* const m_window;
  ControlReference* const m_reference;
  const ControlType m_type;
};
