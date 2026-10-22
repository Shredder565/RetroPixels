// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <cstddef>

#include <QString>
#include <QValidator>

// Rejects input longer than a number of Unicode code points (counted in the UTF-8 encoding).
class UTF8CodePointCountValidator : public QValidator
{
  Q_OBJECT
public:
  explicit UTF8CodePointCountValidator(std::size_t max_count, QObject* parent = nullptr);

  QValidator::State validate(QString& input, int& pos) const override;

private:
  std::size_t m_limit;
};
