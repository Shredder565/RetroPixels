// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QMessageBox>

// A QMessageBox that is window-modal by default and attaches to the top-level window of the
// widget it is opened for (or to nothing, if that window is hidden).
class ModalMessageBox : public QMessageBox
{
public:
  explicit ModalMessageBox(QWidget* parent, Qt::WindowModality modality = Qt::WindowModal);

  // Like QMessageBox's static helpers. Note that question() shows the warning icon.
  static int critical(QWidget* parent, const QString& title, const QString& text,
                      StandardButtons buttons = Ok, StandardButton default_button = NoButton,
                      Qt::WindowModality modality = Qt::WindowModal,
                      const QString& detailed_text = QString());
  static int information(QWidget* parent, const QString& title, const QString& text,
                         StandardButtons buttons = Ok, StandardButton default_button = NoButton,
                         Qt::WindowModality modality = Qt::WindowModal,
                         const QString& detailedText = QString());
  static int question(QWidget* parent, const QString& title, const QString& text,
                      StandardButtons buttons = Yes | No, StandardButton default_button = NoButton,
                      Qt::WindowModality modality = Qt::WindowModal,
                      const QString& detailedText = QString());
  static int warning(QWidget* parent, const QString& title, const QString& text,
                     StandardButtons buttons = Ok, StandardButton default_button = NoButton,
                     Qt::WindowModality modality = Qt::WindowModal,
                     const QString& detailedText = QString());
};
