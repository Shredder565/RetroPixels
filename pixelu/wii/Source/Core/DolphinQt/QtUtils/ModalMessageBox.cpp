// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/QtUtils/ModalMessageBox.h"

#include <QApplication>

namespace
{
QWidget* OwningWindow(QWidget* widget)
{
  return widget ? widget->window() : nullptr;
}

struct BoxContents
{
  QMessageBox::Icon icon;
  const QString& title;
  const QString& text;
  QMessageBox::StandardButtons buttons;
  QMessageBox::StandardButton default_button;
  const QString& details;
};

int RunBox(QWidget* parent, Qt::WindowModality modality, const BoxContents& contents)
{
  ModalMessageBox box(parent, modality);
  box.setIcon(contents.icon);
  box.setWindowTitle(contents.title);
  box.setText(contents.text);
  box.setStandardButtons(contents.buttons);
  box.setDefaultButton(contents.default_button);
  box.setDetailedText(contents.details);
  return box.exec();
}
}  // namespace

ModalMessageBox::ModalMessageBox(QWidget* parent, Qt::WindowModality modality)
    : QMessageBox(OwningWindow(parent))
{
  setWindowModality(modality);
  setWindowFlags(Qt::Sheet | Qt::WindowTitleHint | Qt::CustomizeWindowHint);

  // A box attached to a hidden window would not be seen; better to have no parent at all.
  QWidget* const owner = OwningWindow(parent);
  if (owner && !owner->isVisible())
    setParent(nullptr);
}

int ModalMessageBox::critical(QWidget* parent, const QString& title, const QString& text,
                              StandardButtons buttons, StandardButton default_button,
                              Qt::WindowModality modality, const QString& detailed_text)
{
  return RunBox(parent, modality,
                {QMessageBox::Critical, title, text, buttons, default_button, detailed_text});
}

int ModalMessageBox::information(QWidget* parent, const QString& title, const QString& text,
                                 StandardButtons buttons, StandardButton default_button,
                                 Qt::WindowModality modality, const QString& detailedText)
{
  return RunBox(parent, modality,
                {QMessageBox::Information, title, text, buttons, default_button, detailedText});
}

int ModalMessageBox::question(QWidget* parent, const QString& title, const QString& text,
                              StandardButtons buttons, StandardButton default_button,
                              Qt::WindowModality modality, const QString& detailedText)
{
  // Questions deliberately use the warning icon.
  return RunBox(parent, modality,
                {QMessageBox::Warning, title, text, buttons, default_button, detailedText});
}

int ModalMessageBox::warning(QWidget* parent, const QString& title, const QString& text,
                             StandardButtons buttons, StandardButton default_button,
                             Qt::WindowModality modality, const QString& detailedText)
{
  return RunBox(parent, modality,
                {QMessageBox::Warning, title, text, buttons, default_button, detailedText});
}
