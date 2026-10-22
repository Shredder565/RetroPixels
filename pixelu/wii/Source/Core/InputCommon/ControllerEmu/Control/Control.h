// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <memory>
#include <string>

#include "InputCommon/ControlReference/ControlReference.h"

namespace ControllerEmu
{
// Whether the mapping UI should translate a control's name.
enum class Translatability
{
  DoNotTranslate,
  Translate
};

// One mappable control of a group: its ini name, its UI name and the reference holding the
// user's expression.
class Control
{
public:
  virtual ~Control();

  template <typename T = ControlState>
  T GetState() const
  {
    return control_ref->GetState<T>();
  }

  std::unique_ptr<ControlReference> const control_ref;
  const Translatability translate;
  const std::string name;
  const std::string ui_name;

protected:
  Control(std::unique_ptr<ControlReference> ref, Translatability translate, std::string name,
          std::string ui_name);
  // The UI name is the ini name.
  Control(std::unique_ptr<ControlReference> ref, Translatability translate, std::string name);
};

}  // namespace ControllerEmu
