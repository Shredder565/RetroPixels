// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/ConfigControls/ConfigUserPath.h"

#include <QLineEdit>
#include <QString>

#include "Common/FileUtil.h"

#include "DolphinQt/Config/ConfigControls/ConfigText.h"
#include "DolphinQt/QtUtils/ModalMessageBox.h"

ConfigUserPath::ConfigUserPath(const unsigned int dir_index,
                               const Config::Info<std::string>& setting)
    : ConfigUserPath(dir_index, setting, nullptr)
{
}

ConfigUserPath::ConfigUserPath(const unsigned int dir_index,
                               const Config::Info<std::string>& setting, Config::Layer* layer)
    : ConfigText(setting, layer), m_dir_index(dir_index)
{
  ShowEffectivePath();
  // Runs after ConfigText's own editingFinished handler.
  connect(this, &QLineEdit::editingFinished, this, &ConfigUserPath::ApplyPath);
}

// The setting only seeds File's user path at start-up; empty means "keep the
// path File already has", so that path is what the field shows.
void ConfigUserPath::ShowEffectivePath()
{
  const std::string configured = ReadValue(m_setting);
  const std::string& in_use = File::GetUserPath(m_dir_index);
  setText(QString::fromStdString(configured.empty() ? in_use : configured));
}

void ConfigUserPath::ApplyPath()
{
  const QString entered = text().trimmed();
  if (!entered.isEmpty())
  {
    const std::string path = entered.toStdString();
    File::SetUserPath(m_dir_index, path);
    SaveValue(m_setting, path);
    return;
  }

  ModalMessageBox::warning(this, tr("Empty Value"),
                           tr("This field cannot be left empty. Please enter a value."));
  ShowEffectivePath();
}

void ConfigUserPath::OnConfigChanged()
{
  ShowEffectivePath();
}
