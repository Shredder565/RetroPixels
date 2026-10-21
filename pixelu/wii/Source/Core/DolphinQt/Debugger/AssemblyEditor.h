// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QPlainTextEdit>
#include <QTextBlock>

class QWidget;
class QPaintEvent;
class QResizeEvent;
class QRect;
class QWheelEvent;
class GekkoSyntaxHighlight;

// One tab of the assembler: a plain text editor with a line-number gutter, Gekko syntax
// highlighting and a "#<base address>" header line in the saved file.
class AsmEditor : public QPlainTextEdit
{
  Q_OBJECT;

public:
  AsmEditor(QString file_path, int editor_num, bool dark_scheme, QWidget* parent = nullptr);
  void LineNumberAreaPaintEvent(QPaintEvent* event);
  int LineNumberAreaWidth();
  const QString& Path() const { return m_file_path; }
  const QString& FileName() const { return m_file_name; }
  const QString& EditorTitle() const { return m_show_full_path ? Path() : FileName(); }
  const QString& BaseAddress() const { return m_base; }
  void SetBaseAddress(const QString& ba);
  void SetAmbiguous(bool b) { m_show_full_path = b; }
  int EditorNum() const { return m_number; }
  bool LoadFromPath();
  bool IsDirty() const { return m_modified; }
  bool IsAmbiguous() const { return m_show_full_path; }
  bool PathsMatch(const QString& path) const;
  void Zoom(int amount);

public slots:
  bool SaveFile(const QString& save_path);

signals:
  void PathChanged();
  void DirtyChanged();
  void ZoomRequested(int amount);

protected:
  void resizeEvent(QResizeEvent* event) override;
  void paintEvent(QPaintEvent* event) override;
  bool event(QEvent* e) override;
  void keyPressEvent(QKeyEvent* event) override;
  void wheelEvent(QWheelEvent* event) override;

private:
  class Gutter;

  void FitGutter();
  void ScrollGutter(const QRect& rect, int dy);
  void RefreshCursorLine();
  void MarkModified(bool modified);
  bool ShowErrorTip(const QPoint& local, const QPoint& global);
  int SpaceWidth() const;

  QWidget* m_gutter = nullptr;
  GekkoSyntaxHighlight* m_syntax = nullptr;
  QString m_file_path;
  QString m_file_name;
  QString m_base;
  const int m_number;
  bool m_modified = false;
  QTextBlock m_cursor_block;
  bool m_show_full_path = false;
  const bool m_dark;
};
