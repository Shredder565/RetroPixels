// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Debugger/AssemblerWidget.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QFont>
#include <QFontDatabase>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QShortcut>
#include <QStyle>
#include <QTabWidget>
#include <QTextEdit>
#include <QToolBar>
#include <QVBoxLayout>

#include <algorithm>
#include <filesystem>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>

#include <fmt/format.h>

#include "Common/Assert.h"
#include "Common/FileUtil.h"

#include "Core/Core.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

#include "DolphinQt/Debugger/AssemblyEditor.h"
#include "DolphinQt/QtUtils/DolphinFileDialog.h"
#include "DolphinQt/QtUtils/ModalMessageBox.h"
#include "DolphinQt/Resources.h"
#include "DolphinQt/Settings.h"

namespace
{
namespace GA = Common::GekkoAssembler;

// Output pane width, in '0' characters.
constexpr int OUTPUT_COLUMNS = 18;
// Editors opened from a file have no "New File (n)" number.
constexpr int NO_NUMBER = -1;

// The order of the entries in the output format box.
enum class CodeFormat
{
  Raw,
  ActionReplay,
  Gecko04,
  GeckoC0,
  GeckoC2,
};

// --- Error log -----------------------------------------------------------------------------

QString EscapedPiece(std::string_view line, size_t from, size_t count)
{
  from = std::min(from, line.size());
  return QString::fromStdString(std::string(line.substr(from, count))).toHtmlEscaped();
}

void ReportError(QTextEdit* log, const GA::AssemblerError& error)
{
  log->append(
      QObject::tr("<span style=\"color: red; font-weight: bold\">Error</span> on line %1 col %2")
          .arg(error.line + 1)
          .arg(error.col + 1));

  const std::string_view line = error.error_line;
  log->append(QStringLiteral("<span style=\"font-family:'monospace';font-size:16px\">"
                             "<pre>%1<u><span style=\"color:red;font-weight:bold\">%2</span></u>%3"
                             "</pre></span>")
                  .arg(EscapedPiece(line, 0, error.col))
                  .arg(EscapedPiece(line, error.col, error.len))
                  .arg(EscapedPiece(line, error.col + error.len, std::string_view::npos)));

  log->append(
      QStringLiteral("<span>%1</span>").arg(QString::fromStdString(error.message).toHtmlEscaped()));
}

// --- Code output ---------------------------------------------------------------------------

using Text = std::string;

template <typename... Args>
void Put(Text& out, fmt::format_string<Args...> format, Args&&... args)
{
  fmt::format_to(std::back_inserter(out), format, std::forward<Args>(args)...);
}

// Hex bytes, four to a group and eight to a line. With word padding, a partial word is filled with
// zero bytes and followed by a separator; without it, a final line break is added unless the
// block ends on the seventh byte of a line.
void PutHexRows(Text& out, const std::vector<u8>& bytes, bool pad_to_word)
{
  size_t count = 0;
  const auto separator = [](size_t n) -> std::optional<char> {
    if (n % 8 == 7)
      return '\n';
    if (n % 4 == 3)
      return ' ';
    return std::nullopt;
  };

  for (const u8 byte : bytes)
  {
    Put(out, "{:02x}", byte);
    if (const auto sep = separator(count))
      out += *sep;
    ++count;
  }

  if (!pad_to_word)
  {
    if (count % 8 != 7)
      out += '\n';
    return;
  }

  if (count % 4 == 0)
    return;
  while (count % 4 != 0)
  {
    out += "00";
    ++count;
  }
  out += (count % 8 == 0) ? '\n' : ' ';
}

void PutRaw(Text& out, const GA::CodeBlock& block)
{
  Put(out, "# Block {:08x}\n", block.block_address);
  PutHexRows(out, block.instructions, false);
}

// Action Replay RAM writes: 32-bit writes for whole words, then 8-bit writes for the rest.
void PutActionReplay(Text& out, const GA::CodeBlock& block)
{
  const std::vector<u8>& b = block.instructions;
  size_t at = 0;
  for (; at + 4 <= b.size(); at += 4)
  {
    const u32 code = ((block.block_address + static_cast<u32>(at)) & 0x1ffffff) | 0x04000000;
    Put(out, "{:08x} {:02x}{:02x}{:02x}{:02x}\n", code, b[at], b[at + 1], b[at + 2], b[at + 3]);
  }
  for (; at < b.size(); ++at)
  {
    const u32 code = (block.block_address + static_cast<u32>(at)) & 0x1ffffff;
    Put(out, "{:08x} 000000{:02x}\n", code, b[at]);
  }
}

// Gecko C0/C2 bodies are lines of 8 bytes; the trailing word(s) either share the last line or
// need one more line of their own.
struct GeckoBody
{
  u32 lines;
  bool tail_on_own_line;
};

GeckoBody MeasureGeckoBody(size_t byte_count)
{
  const size_t rest = byte_count % 8;
  const bool own_line = rest == 0 || rest > 4;
  const u32 lines = 1 + static_cast<u32>((byte_count - 1) / 8) + (own_line ? 1 : 0);
  return {lines, own_line};
}

void PutGeckoExecute(Text& out, const GA::CodeBlock& block)
{
  const GeckoBody body = MeasureGeckoBody(block.instructions.size());
  Put(out, "c0000000 {:08x}\n", body.lines);
  PutHexRows(out, block.instructions, true);
  // blr, padded to a full line when needed.
  out += body.tail_on_own_line ? "4e800020 00000000\n" : "4e800020\n";
}

void PutGeckoInsert(Text& out, const GA::CodeBlock& block)
{
  const GeckoBody body = MeasureGeckoBody(block.instructions.size());
  const u32 target = (block.block_address & 0x1ffffff) | 0x02000000;
  Put(out, "c{:07x} {:08x}\n", target, body.lines);
  PutHexRows(out, block.instructions, true);
  // The handler appends the branch back; we only supply the nop/terminator word(s).
  out += body.tail_on_own_line ? "60000000 00000000\n" : "00000000\n";
}

Text FormatBlocks(const std::vector<GA::CodeBlock>& blocks, CodeFormat format)
{
  Text out;
  for (const GA::CodeBlock& block : blocks)
  {
    if (block.instructions.empty())
      continue;

    switch (format)
    {
    case CodeFormat::Raw:
      PutRaw(out, block);
      break;
    case CodeFormat::ActionReplay:
    case CodeFormat::Gecko04:
      PutActionReplay(out, block);
      break;
    case CodeFormat::GeckoC0:
      PutGeckoExecute(out, block);
      break;
    case CodeFormat::GeckoC2:
      PutGeckoInsert(out, block);
      break;
    }
  }
  return out;
}

CodeFormat FormatFromIndex(int index)
{
  switch (index)
  {
  case 1:
    return CodeFormat::ActionReplay;
  case 2:
    return CodeFormat::Gecko04;
  case 3:
    return CodeFormat::GeckoC0;
  case 4:
    return CodeFormat::GeckoC2;
  default:
    return CodeFormat::Raw;
  }
}

bool LooksDark()
{
  const QPalette palette;
  return palette.color(QPalette::WindowText).value() > palette.color(QPalette::Window).value();
}
}  // namespace

AssemblerWidget::AssemblerWidget(QWidget* parent)
    : QDockWidget(parent), m_system(Core::System::GetInstance()), m_dark(LooksDark())
{
  setWindowTitle(tr("Assembler"));
  setObjectName(QStringLiteral("assemblerwidget"));

  auto& settings = Settings::Instance();
  setHidden(!settings.IsAssemblerVisible() || !settings.IsDebugModeEnabled());
  setVisible(true);

  BuildLayout();

  QSettings& stored = Settings::GetQSettings();
  restoreGeometry(stored.value(QStringLiteral("assemblerwidget/geometry")).toByteArray());
  setFloating(stored.value(QStringLiteral("assemblerwidget/floating")).toBool());

  connect(&settings, &Settings::AssemblerVisibilityChanged, this,
          [this](bool visible) { setHidden(!visible); });
  connect(&settings, &Settings::DebugModeToggled, this, [this](bool enabled) {
    setHidden(!enabled || !Settings::Instance().IsAssemblerVisible());
  });
  connect(&settings, &Settings::EmulationStateChanged, this, [this](Core::State state) {
    m_inject_action->setEnabled(state == Core::State::Running || state == Core::State::Paused);
  });
  connect(&settings, &Settings::ThemeChanged, this, &AssemblerWidget::RefreshIcons);
  connect(m_tabs, &QTabWidget::tabCloseRequested, this, &AssemblerWidget::RequestCloseTab);

  AddShortcuts();

  connect(m_output, &QPlainTextEdit::updateRequest, this, &AssemblerWidget::FitOutputWidth);
  connect(m_copy_button, &QPushButton::released, this,
          [this] { QApplication::clipboard()->setText(m_output->toPlainText()); });
  connect(m_base_field, &QLineEdit::textChanged, this, [this] {
    if (AsmEditor* editor = CurrentEditor())
      editor->SetBaseAddress(m_base_field->text());
  });
  connect(m_tabs, &QTabWidget::currentChanged, this, [this](int index) {
    if (index == -1)
      m_base_field->clear();
    else
      m_base_field->setText(EditorAt(index)->BaseAddress());
  });

  RefreshIcons();
}

AssemblerWidget::~AssemblerWidget()
{
  QSettings& stored = Settings::GetQSettings();
  stored.setValue(QStringLiteral("assemblerwidget/geometry"), saveGeometry());
  stored.setValue(QStringLiteral("assemblerwidget/floating"), isFloating());
}

void AssemblerWidget::closeEvent(QCloseEvent*)
{
  Settings::Instance().SetAssemblerVisible(false);
}

void AssemblerWidget::AddShortcuts()
{
  // Ctrl+S only saves while the editor itself has focus.
  auto* save = new QShortcut(QKeySequence::Save, this);
  connect(save, &QShortcut::activated, this, [this] {
    if (m_tabs->currentIndex() != -1 && m_tabs->currentWidget()->hasFocus())
      SaveCurrent();
  });

  const auto zoom_key = [this](const QKeySequence& keys, auto handler) {
    auto* shortcut = new QShortcut(keys, this);
    shortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(shortcut, &QShortcut::activated, this, handler);
  };
  const auto zoom_in = [this] {
    if (m_tabs->currentIndex() != -1)
      ZoomEverything(2);
  };
  const auto zoom_out = [this] {
    if (m_tabs->currentIndex() != -1)
      ZoomEverything(-2);
  };

  zoom_key(QKeySequence::ZoomIn, zoom_in);
  zoom_key(QKeySequence::ZoomOut, zoom_out);
  zoom_key(QKeySequence(Qt::CTRL | Qt::Key_Equal), zoom_in);
  zoom_key(QKeySequence(Qt::CTRL | Qt::Key_Underscore), zoom_out);
  zoom_key(QKeySequence(Qt::CTRL | Qt::Key_0), [this] {
    if (m_tabs->currentIndex() != -1)
      ZoomEverything(-m_zoom_total);
  });
}

void AssemblerWidget::BuildLayout()
{
  m_tabs = new QTabWidget;
  m_bar = new QToolBar;
  m_format_choice = new QComboBox;
  m_output = new QPlainTextEdit;
  m_messages = new QTextEdit;
  m_base_field = new QLineEdit;
  m_copy_button = new QPushButton;

  m_tabs->setTabsClosable(true);

  m_bar->setContentsMargins(0, 0, 0, 0);
  m_bar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  m_open_action = m_bar->addAction(tr("Open"), this, &AssemblerWidget::OpenFiles);
  m_new_action = m_bar->addAction(tr("New"), this, [this] { AddEditor(); });
  m_assemble_action = m_bar->addAction(tr("Assemble"), this, [this] {
    Blocks discarded;
    AssembleCurrent(&discarded);
  });
  m_inject_action = m_bar->addAction(tr("Inject"), this, &AssemblerWidget::InjectCurrent);
  m_save_action = m_bar->addAction(tr("Save"), this, &AssemblerWidget::SaveCurrent);
  for (QAction* action : {m_inject_action, m_save_action, m_assemble_action})
    action->setEnabled(false);

  // Output and log panes share a grey background that suits the theme.
  QPalette pane = m_output->palette();
  pane.setColor(QPalette::Base,
                m_dark ? QColor::fromRgb(76, 76, 76) : QColor::fromRgb(180, 180, 180));
  m_output->setPalette(pane);
  m_messages->setPalette(pane);

  QFont code_font(QFontDatabase::systemFont(QFontDatabase::FixedFont).family());
  code_font.setPointSize(12);
  QFont log_font(QFontDatabase::systemFont(QFontDatabase::GeneralFont).family());
  log_font.setPointSize(12);
  const QFontMetrics code_metrics(code_font);

  m_output->setFont(code_font);
  m_messages->setFont(log_font);
  m_output->setReadOnly(true);
  m_messages->setReadOnly(true);

  const int output_width = code_metrics.horizontalAdvance(QLatin1Char('0')) * OUTPUT_COLUMNS;
  m_messages->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  // Three log lines plus one line of the source excerpt.
  m_messages->setFixedHeight(code_metrics.height() * 4);
  m_output->setFixedWidth(output_width);

  for (const QString& name :
       {tr("Raw"), tr("AR Code"), tr("Gecko (04)"), tr("Gecko (C0)"), tr("Gecko (C2)")})
  {
    m_format_choice->addItem(name);
  }

  auto* base_row = new QHBoxLayout;
  base_row->addWidget(new QLabel(tr("Base Address")));
  base_row->addWidget(m_base_field);

  auto* output_controls = new QHBoxLayout;
  output_controls->addWidget(m_format_choice);
  output_controls->addWidget(m_copy_button);

  auto* base_box = new QWidget();
  base_box->setLayout(base_row);
  base_row->setContentsMargins(0, 0, 0, 0);

  auto* controls_box = new QWidget();
  controls_box->setFixedWidth(output_width);
  controls_box->setLayout(output_controls);
  output_controls->setContentsMargins(0, 0, 0, 0);

  auto* grid = new QGridLayout;
  grid->setSpacing(0);
  grid->setContentsMargins(5, 0, 5, 5);
  grid->addWidget(m_bar, 0, 0, 1, 2);

  auto* input_group = new QGroupBox(tr("Input"));
  auto* input_column = new QVBoxLayout;
  input_group->setLayout(input_column);
  input_column->addWidget(m_tabs);
  input_column->addWidget(base_box);
  grid->addWidget(input_group, 1, 0, 1, 1);

  auto* output_group = new QGroupBox(tr("Output"));
  auto* output_grid = new QGridLayout;
  output_group->setLayout(output_grid);
  output_grid->addWidget(m_output, 0, 0);
  output_grid->addWidget(controls_box, 1, 0);
  grid->addWidget(output_group, 1, 1, 1, 1);
  output_group->setSizePolicy(QSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding));

  auto* log_group = new QGroupBox(tr("Error Log"));
  auto* log_row = new QHBoxLayout;
  log_group->setLayout(log_row);
  log_row->addWidget(m_messages);
  grid->addWidget(log_group, 2, 0, 1, 2);
  log_group->setSizePolicy(QSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed));

  auto* body = new QWidget;
  body->setLayout(grid);
  setWidget(body);
}

void AssemblerWidget::FitOutputWidth()
{
  // Leave room for the scroll bar while it is shown.
  int width = m_output->fontMetrics().horizontalAdvance(QLatin1Char('0')) * OUTPUT_COLUMNS;
  if (m_output->verticalScrollBar()->isVisible())
    width += m_output->style()->pixelMetric(QStyle::PM_ScrollBarExtent);
  m_output->setFixedWidth(width);
}

void AssemblerWidget::RefreshIcons()
{
  m_new_action->setIcon(Resources::GetThemeIcon("assembler_new"));
  m_open_action->setIcon(Resources::GetThemeIcon("assembler_openasm"));
  m_save_action->setIcon(Resources::GetThemeIcon("assembler_save"));
  m_assemble_action->setIcon(Resources::GetThemeIcon("assembler_assemble"));
  m_inject_action->setIcon(Resources::GetThemeIcon("assembler_inject"));
  m_copy_button->setIcon(Resources::GetThemeIcon("assembler_clipboard"));
}

AsmEditor* AssemblerWidget::EditorAt(int index) const
{
  return qobject_cast<AsmEditor*>(m_tabs->widget(index));
}

AsmEditor* AssemblerWidget::CurrentEditor() const
{
  const int index = m_tabs->currentIndex();
  return index == -1 ? nullptr : EditorAt(index);
}

int AssemblerWidget::IndexOf(const AsmEditor* editor) const
{
  const int count = m_tabs->count();
  int index = 0;
  while (index < count && m_tabs->widget(index) != editor)
    ++index;
  return index;
}

bool AssemblerWidget::ApplicationCloseRequest()
{
  int unsaved = 0;
  for (int i = 0; i < m_tabs->count(); ++i)
    unsaved += EditorAt(i)->IsDirty() ? 1 : 0;
  if (unsaved == 0)
    return true;

  const int answer = ModalMessageBox::question(
      this, tr("Unsaved Changes"),
      tr("You have %1 unsaved assembly tabs open\n\n"
         "Do you want to save all and exit?")
          .arg(unsaved),
      QMessageBox::YesToAll | QMessageBox::NoToAll | QMessageBox::Cancel, QMessageBox::Cancel);

  if (answer == QMessageBox::Cancel)
    return false;
  if (answer == QMessageBox::YesToAll)
  {
    for (int i = 0; i < m_tabs->count(); ++i)
    {
      AsmEditor* editor = EditorAt(i);
      if (editor->IsDirty() && !SaveEditor(editor))
        return false;
    }
  }
  return true;
}

bool AssemblerWidget::AssembleCurrent(Blocks* blocks)
{
  AsmEditor* editor = CurrentEditor();
  if (!editor)
    return false;

  m_messages->clear();
  m_output->clear();
  const CodeFormat format = FormatFromIndex(m_format_choice->currentIndex());

  bool base_ok = false;
  u32 base = m_base_field->text().toUInt(&base_ok, 16);
  if (!base_ok)
  {
    base = 0;
    m_messages->append(
        tr("<span style=\"color:#ffcc00\">Warning</span> invalid base address, defaulting to 0"));
  }

  auto outcome = GA::Assemble(editor->toPlainText().toStdString(), base);
  if (GA::IsFailure(outcome))
  {
    // The error replaces the base address warning.
    m_messages->clear();
    blocks->clear();
    ReportError(m_messages, GA::GetFailure(outcome));
    return false;
  }

  Blocks& assembled = GA::GetT(outcome);
  m_output->appendPlainText(QString::fromStdString(FormatBlocks(assembled, format)));
  m_output->moveCursor(QTextCursor::Start);
  m_output->ensureCursorVisible();

  *blocks = std::move(assembled);
  return true;
}

void AssemblerWidget::InjectCurrent()
{
  // Hold the CPU for the whole assemble-and-patch sequence.
  const Core::CPUThreadGuard guard(m_system);

  Blocks blocks;
  AssembleCurrent(&blocks);

  auto& debug = m_system.GetPowerPC().GetDebugInterface();
  for (const GA::CodeBlock& block : blocks)
  {
    if (PowerPC::MMU::HostIsRAMAddress(guard, block.block_address) && !block.instructions.empty())
      debug.SetPatch(guard, block.block_address, block.instructions);
  }
}

void AssemblerWidget::OpenFiles()
{
  const QString start_dir = QString::fromStdString(File::GetUserPath(D_ASM_ROOT_IDX));
  const QStringList chosen = DolphinFileDialog::getOpenFileNames(
      this, tr("Select a File"), start_dir,
      QStringLiteral("%1 (*.s *.S *.asm);;%2 (*)")
          .arg(tr("All Assembly files"))
          .arg(tr("All Files")));
  if (chosen.isEmpty())
    return;

  // Files that are already open are not opened twice. Only the last chosen file, if it was
  // already open, is brought to the front (a newly opened file already is).
  std::optional<int> already_open;
  for (const QString& path : chosen)
  {
    already_open.reset();
    for (int i = 0; i < m_tabs->count() && !already_open; ++i)
    {
      if (EditorAt(i)->PathsMatch(path))
        already_open = i;
    }
    if (!already_open)
      AddEditor(path);
  }

  if (already_open)
    m_tabs->setCurrentIndex(*already_open);
}

void AssemblerWidget::SaveCurrent()
{
  if (AsmEditor* editor = CurrentEditor())
    SaveEditor(editor);
}

bool AssemblerWidget::SaveEditor(AsmEditor* editor)
{
  QString target = editor->Path();
  if (target.isEmpty())
  {
    const QString asm_filter = QStringLiteral("%1 (*.S)").arg(tr("Assembly File"));
    const QString any_filter = QStringLiteral("%1 (*)").arg(tr("All Files"));

    QString used_filter;
    target = DolphinFileDialog::getSaveFileName(
        this, tr("Save File To"), QString::fromStdString(File::GetUserPath(D_ASM_ROOT_IDX)),
        QStringLiteral("%1;;%2").arg(asm_filter).arg(any_filter), &used_filter);
    if (target.isEmpty())
      return false;

    // With the assembly filter chosen, a name without an extension gets ".S".
    if (used_filter == asm_filter &&
        !std::filesystem::path(target.toStdString()).has_extension())
    {
      target.append(QStringLiteral(".S"));
    }
  }

  // A failed write is not reported; the tab simply stays marked as unsaved.
  editor->SaveFile(target);
  return true;
}

void AssemblerWidget::AddEditor(const QString& path)
{
  const bool untitled = path.isEmpty();
  auto* editor = new AsmEditor(path, untitled ? TakeUntitledNumber() : NO_NUMBER, m_dark);
  if (!untitled && !editor->LoadFromPath())
  {
    ModalMessageBox::warning(this, tr("Failed to open file"),
                             tr("Failed to read the contents of file:\n%1").arg(path));
    delete editor;
    return;
  }

  const int index = m_tabs->addTab(editor, QString());
  connect(editor, &AsmEditor::PathChanged, this, [this, editor] {
    ResolveDuplicateTitles(editor);
    RetitleTab(editor);
  });
  connect(editor, &AsmEditor::DirtyChanged, this, [this, editor] { RetitleTab(editor); });
  connect(editor, &AsmEditor::ZoomRequested, this, &AssemblerWidget::ZoomEverything);
  editor->Zoom(m_zoom_total);

  ResolveDuplicateTitles(editor);
  m_tabs->setTabText(index, TitleFor(editor, true));

  m_save_action->setEnabled(true);
  m_assemble_action->setEnabled(true);

  m_tabs->setCurrentIndex(index);
}

void AssemblerWidget::RequestCloseTab(int index)
{
  ASSERT(index < m_tabs->count());
  AsmEditor* editor = EditorAt(index);

  if (editor->IsDirty())
  {
    const int answer = ModalMessageBox::question(
        this, tr("Unsaved Changes"),
        tr("There are unsaved changes in \"%1\".\n\n"
           "Do you want to save before closing?")
            .arg(TitleFor(editor, false)),
        QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel, QMessageBox::Cancel);

    if (answer == QMessageBox::Cancel)
      return;
    if (answer == QMessageBox::Yes && editor->IsDirty() && !SaveEditor(editor))
      return;
  }

  RemoveTab(index, editor);
}

void AssemblerWidget::RemoveTab(int index, AsmEditor* editor)
{
  ReturnUntitledNumber(editor->EditorNum());
  m_tabs->removeTab(index);
  editor->deleteLater();

  ResolveDuplicateTitles(nullptr);

  if (m_tabs->count() == 0)
  {
    m_save_action->setEnabled(false);
    m_assemble_action->setEnabled(false);
  }
}

QString AssemblerWidget::TitleFor(const AsmEditor* editor, bool mark_unsaved) const
{
  ASSERT(editor != nullptr);

  QString title;
  if (!editor->Path().isEmpty())
    title = editor->EditorTitle();
  else if (editor->EditorNum() == 0)
    title = tr("New File");
  else
    title = tr("New File (%1)").arg(editor->EditorNum() + 1);

  if (mark_unsaved && editor->IsDirty())
  {
    // i18n: the "*" appended to an editor tab title marks a file with unsaved edits
    title = tr("%1 *").arg(title);
  }
  return title;
}

void AssemblerWidget::RetitleTab(AsmEditor* editor)
{
  const int index = IndexOf(editor);
  ASSERT(index < m_tabs->count());
  m_tabs->setTabText(index, TitleFor(editor, true));
}

void AssemblerWidget::ResolveDuplicateTitles(AsmEditor* added)
{
  // Tabs whose file names clash show their full path instead. First re-check every tab that
  // currently shows a full path: its partners are marked too, and it goes back to the short
  // name once nothing clashes any more.
  const int count = m_tabs->count();
  for (int i = 0; i < count; ++i)
  {
    AsmEditor* tab = EditorAt(i);
    if (!tab->IsAmbiguous())
      continue;

    bool clashes = false;
    for (int j = 0; j < count; ++j)
    {
      AsmEditor* other = EditorAt(j);
      if (j == i || other->FileName() != tab->FileName())
        continue;
      clashes = true;
      if (!other->IsAmbiguous())
      {
        other->SetAmbiguous(true);
        RetitleTab(other);
      }
    }

    if (!clashes)
    {
      tab->SetAmbiguous(false);
      RetitleTab(tab);
    }
  }

  if (!added)
    return;

  // Then mark the new tab and everything it clashes with.
  bool added_clashes = false;
  for (int i = 0; i < count; ++i)
  {
    AsmEditor* other = EditorAt(i);
    if (other == added || other->FileName() != added->FileName())
      continue;
    other->SetAmbiguous(true);
    RetitleTab(other);
    added_clashes = true;
  }
  if (added_clashes)
  {
    added->SetAmbiguous(true);
    RetitleTab(added);
  }
}

int AssemblerWidget::TakeUntitledNumber()
{
  // Reuse the lowest number given back by a closed tab before handing out a new one.
  const auto lowest = std::ranges::min_element(m_returned_numbers);
  if (lowest == m_returned_numbers.end())
    return m_next_untitled++;

  const int number = *lowest;
  m_returned_numbers.erase(lowest);
  return number;
}

void AssemblerWidget::ReturnUntitledNumber(int number)
{
  if (number != NO_NUMBER)
    m_returned_numbers.push_back(number);
}

void AssemblerWidget::ZoomEverything(int steps)
{
  if (steps == 0)
    return;
  m_zoom_total += steps;
  for (int i = 0; i < m_tabs->count(); ++i)
    EditorAt(i)->Zoom(steps);
}
