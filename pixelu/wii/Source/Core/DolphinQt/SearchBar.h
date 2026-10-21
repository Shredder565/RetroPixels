// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QWidget>

class QLineEdit;
class QPushButton;

// The game list's search field. Hidden until Show() is called; Escape or Close hides it again and
// clears the filter.
class SearchBar : public QWidget
{
  Q_OBJECT
public:
  explicit SearchBar(QWidget* parent = nullptr);

  void Show();
  void Hide();

signals:
  void Search(const QString& search);

private:
  bool eventFilter(QObject* object, QEvent* event) final;

  QLineEdit* m_query;
  QPushButton* m_close;
};
