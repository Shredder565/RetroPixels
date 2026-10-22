// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/ControllerInterface/ServerStringValidator.h"

#include <string>

#include "DolphinQt/Config/ControllerInterface/DualShockUDPSettings.h"

ServerStringValidator::ServerStringValidator(QObject* parent) : QValidator(parent)
{
}

QValidator::State ServerStringValidator::validate(QString& input, int&) const
{
  if (input.isEmpty())
    return Invalid;

  for (const char separator :
       {DualShockUDPSettings::FIELD_SEPARATOR, DualShockUDPSettings::SERVER_SEPARATOR})
  {
    if (input.contains(QChar::fromLatin1(separator)))
      return Invalid;
  }
  return Acceptable;
}
