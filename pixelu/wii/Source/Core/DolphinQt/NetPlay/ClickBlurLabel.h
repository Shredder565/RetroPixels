// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QLabel>
#include <QStackedWidget>
#include <QWidget>

// A label whose text stays hidden behind a blur until it is clicked. Each further click toggles
// between the hidden and the readable form.
class ClickBlurLabel final : public QStackedWidget
{
  Q_OBJECT
public:
  explicit ClickBlurLabel(QWidget* parent = nullptr);

  void setText(const QString& text);

  QString text() const { return m_normal_label->text(); }

protected:
  void mousePressEvent(QMouseEvent* event) override;

private:
  // Produces a stand-in string with the same shape as the input (letter case, digits and
  // punctuation kept in place) so the blurred page gives nothing away.
  static QString GenerateBlurredText(const QString& text);

  QLabel* m_normal_label;
  QLabel* m_blurred_label;
};
