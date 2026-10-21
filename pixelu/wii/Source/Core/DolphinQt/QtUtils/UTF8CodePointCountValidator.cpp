// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/QtUtils/UTF8CodePointCountValidator.h"

#include "Common/StringUtil.h"

UTF8CodePointCountValidator::UTF8CodePointCountValidator(std::size_t max_count, QObject* parent)
    : QValidator(parent), m_limit(max_count)
{
}

QValidator::State UTF8CodePointCountValidator::validate(QString& input, int&) const
{
  const bool fits = StringUTF8CodePointCount(input.toStdString()) <= m_limit;
  return fits ? QValidator::Acceptable : QValidator::Invalid;
}
