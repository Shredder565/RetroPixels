// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/ConfigControls/ConfigText.h"

#include <QLineEdit>

ConfigText::ConfigText(const Config::Info<std::string>& setting) : ConfigText(setting, nullptr)
{
}

ConfigText::ConfigText(const Config::Info<std::string>& setting, Config::Layer* layer)
    : ConfigControl(setting.GetLocation(), layer), m_setting(setting)
{
  OnConfigChanged();
  connect(this, &QLineEdit::editingFinished, this, &ConfigText::StoreText);
}

// Sets the text programmatically and saves it, as if the user had typed it.
void ConfigText::SetTextAndUpdate(const QString& text)
{
  if (this->text() == text)
    return;
  setText(text);
  StoreText();
}

void ConfigText::StoreText()
{
  SaveValue(m_setting, text().toStdString());
}

void ConfigText::OnConfigChanged()
{
  setText(QString::fromStdString(ReadValue(m_setting)));
}
