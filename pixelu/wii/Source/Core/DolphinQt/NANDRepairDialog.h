// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QDialog>

class QWidget;

namespace WiiUtils
{
struct NANDCheckResult;
}

// Shown after a NAND check found problems: explains what a repair will do, including which
// titles it has to delete, and asks whether to go ahead (accepted) or not (rejected).
class NANDRepairDialog final : public QDialog
{
  Q_OBJECT

public:
  explicit NANDRepairDialog(const WiiUtils::NANDCheckResult& result, QWidget* parent = nullptr);
};
