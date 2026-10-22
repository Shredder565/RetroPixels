// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <cstddef>
#include <string>

#include "InputCommon/ControllerEmu/ControlGroup/ControlGroup.h"
#include "InputCommon/ControllerInterface/CoreDevice.h"

namespace ControllerEmu
{
// Raw IR camera objects passed through from a real Wii Remote: position and size of each of
// the four tracked objects.
class IRPassthrough : public ControlGroup
{
public:
  IRPassthrough(std::string name, std::string ui_name);

  ControlState GetObjectPositionX(size_t object_index) const;
  ControlState GetObjectPositionY(size_t object_index) const;
  ControlState GetObjectSize(size_t object_index) const;

  bool AreInputsBound() const;
};
}  // namespace ControllerEmu
