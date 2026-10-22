// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QMap>
#include <QPushButton>
#include <QString>
#include <QStringList>
#include <QWidget>

class QCompleter;
class QMenu;
class QTextEdit;

// A plain-text editor for one game INI file, with keyword completion and
// What's This help for known keys. Edits are written back as they happen.
// Without Q_OBJECT, its texts translate in the QWidget context.
class GameConfigEdit : public QWidget
{
public:
  explicit GameConfigEdit(QWidget* parent, QString path, bool read_only);

protected:
  void keyPressEvent(QKeyEvent* e) override;
  void focusInEvent(QFocusEvent* e) override;

private:
  void ReadFromDisk();
  void WriteToDisk();
  void Describe(const QString& keyword, const QString& help);

  void ShowHelpForSelection();
  void InsertCompletion(const QString& completion);
  void EditExternally();
  QString WordAtCursor() const;

  QCompleter* m_completer;
  QStringList m_keywords;
  QPushButton* m_reload_button;
  QPushButton* m_external_button;
  QTextEdit* m_text;

  const QString m_path;
  bool m_read_only;

  QMap<QString, QString> m_help;
};
