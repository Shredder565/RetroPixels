// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QValidator>

// Accepts a non-empty DSU server field that contains neither of the
// separators used to store the server list.
class ServerStringValidator : public QValidator
{
  Q_OBJECT
public:
  explicit ServerStringValidator(QObject* parent);

  State validate(QString& input, int& pos) const override;
};
