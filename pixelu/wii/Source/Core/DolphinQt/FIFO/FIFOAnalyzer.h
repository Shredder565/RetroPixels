// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <optional>
#include <vector>

#include <QWidget>

#include "Common/CommonTypes.h"

class FifoPlayer;
class QFont;
class QGroupBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSplitter;
class QTextBrowser;
class QTreeWidget;

// Browses the loaded FIFO log: a tree of frames and their objects (draws and EFB copies), the
// commands of the selected object, a description of the selected command, and a byte search
// within the object.
class FIFOAnalyzer final : public QWidget
{
  Q_OBJECT

public:
  explicit FIFOAnalyzer(FifoPlayer& fifo_player);
  ~FIFOAnalyzer() override;

  void Update();

private:
  // Where the selected tree object lives in the log.
  struct ObjectSpan
  {
    u32 frame;
    u32 first_part;
    u32 last_part;
  };
  // The command bytes of an object, located in its frame's FIFO data.
  struct ObjectBytes
  {
    const u8* begin;
    u32 fifo_offset;
    u32 size;
  };

  void BuildLayout();
  void ConnectSignals();

  std::optional<ObjectSpan> SelectedObject() const;
  ObjectBytes BytesOf(const ObjectSpan& span) const;

  void BeginSearch();
  void FindNext();
  void FindPrevious();

  void ShowSearchResult(size_t index);

  void UpdateTree();
  void UpdateDetails();
  void UpdateDescription();

  void OnDebugFontChanged(const QFont& font);

  FifoPlayer& m_fifo_player;

  QTreeWidget* m_tree_widget;
  QListWidget* m_detail_list;
  QTextBrowser* m_entry_detail_browser;
  QSplitter* m_object_splitter;

  // Search
  QGroupBox* m_search_box;
  QLineEdit* m_search_edit;
  QPushButton* m_search_new;
  QPushButton* m_search_next;
  QPushButton* m_search_previous;
  QLabel* m_search_label;
  QSplitter* m_search_splitter;

  // A command of the selected object that contains the searched bytes.
  struct SearchResult
  {
    u32 frame;
    // Row of the object under its frame in the tree (not an object or part number).
    u32 object_row;
    // Row of the command in the detail list.
    u32 command;
  };

  // Where each command of the selected object starts, relative to the object's first byte.
  std::vector<u32> m_object_data_offsets;

  std::vector<SearchResult> m_search_results;
};
