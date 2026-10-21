// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/Mapping/IOWindow.h"

#include <array>
#include <optional>

#include <QBrush>
#include <QColor>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QItemDelegate>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QTableWidget>
#include <QTextBlock>
#include <QTimer>
#include <QVBoxLayout>

#include "DolphinQt/Config/Mapping/MappingCommon.h"
#include "DolphinQt/Config/Mapping/MappingIndicator.h"
#include "DolphinQt/Config/Mapping/MappingWindow.h"
#include "DolphinQt/QtUtils/BlockUserInputFilter.h"
#include "DolphinQt/QtUtils/ModalMessageBox.h"
#include "DolphinQt/Settings.h"

#include "InputCommon/ControlReference/ControlReference.h"
#include "InputCommon/ControlReference/ExpressionParser.h"
#include "InputCommon/ControllerEmu/ControllerEmu.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/ControllerInterface/MappingCommon.h"

namespace
{
namespace EP = ciface::ExpressionParser;

// How long input detection listens, and how long an output test runs.
constexpr auto DETECT_DURATION = std::chrono::seconds(2);
constexpr auto TEST_DURATION = std::chrono::seconds(2);

// Fixed entries at the top of the variables combo: title, separator, reset, separator.
constexpr int VARIABLE_COMBO_FIXED_ITEMS = 4;
constexpr int VARIABLE_COMBO_RESET_INDEX = 2;

// ---- Expression colouring --------------------------------------------------------------------

QTextCharFormat Colored(QColor dark_theme, Qt::GlobalColor light_theme)
{
  QTextCharFormat format;
  const QColor color = Settings::Instance().IsThemeDark() ? dark_theme : QColor(light_theme);
  format.setForeground(QBrush{color});
  return format;
}

QTextCharFormat Emphasised()
{
  QTextCharFormat format;
  format.setFontWeight(QFont::Weight::Bold);
  return format;
}

QTextCharFormat Squiggled()
{
  QTextCharFormat format;
  format.setUnderlineStyle(QTextCharFormat::WaveUnderline);
  format.setUnderlineColor(Settings::Instance().IsThemeDark() ? QColor(255, 69, 0) :
                                                                QColor(Qt::darkRed));
  return format;
}

std::optional<QTextCharFormat> FormatFor(const EP::Token& token)
{
  switch (token.type)
  {
  case EP::TOK_INVALID:
    return Squiggled();
  case EP::TOK_LPAREN:
  case EP::TOK_RPAREN:
  case EP::TOK_COMMA:
  case EP::TOK_QUESTION:
  case EP::TOK_COLON:
    return Emphasised();
  case EP::TOK_LITERAL:
    return Colored(QColor(171, 132, 219), Qt::darkMagenta);
  case EP::TOK_CONTROL:
    return Colored(QColor(0, 220, 0), Qt::darkGreen);
  case EP::TOK_BAREWORD:
    return Colored(QColor(66, 138, 255), Qt::darkBlue);
  case EP::TOK_VARIABLE:
    return Colored(QColor(226, 226, 0), Qt::darkYellow);
  case EP::TOK_COMMENT:
    return Colored(QColor(176, 176, 176), Qt::darkGray);
  default:
    break;
  }
  if (token.IsBinaryOperator())
    return Emphasised();
  return std::nullopt;
}

// ---- State read-outs -------------------------------------------------------------------------

// Draws the value as text with a red bar behind it; the part of the text over the bar is
// redrawn in the contrasting colour.
void PaintValueBar(QPainter& painter, const QRect& area, ControlState value)
{
  const QString text = QString::number(value, 'g', 4);

  QRect bar = area;
  bar.setWidth(static_cast<int>(area.width() * std::clamp(value, 0.0, 1.0)));

  // An unparented indicator is only used to look up the shared colours.
  MappingIndicator colours;

  painter.setPen(colours.GetTextColor());
  painter.drawText(area, Qt::AlignCenter, text);

  painter.fillRect(bar, colours.GetAdjustedInputColor());

  painter.setPen(colours.GetAltTextColor());
  painter.setClipping(true);
  painter.setClipRect(bar);
  painter.drawText(area, Qt::AlignCenter, text);
}
}  // namespace

// Paints the live value of each input into one column of the control table.
class InputStateDelegate : public QItemDelegate
{
public:
  explicit InputStateDelegate(IOWindow* parent, int column,
                              std::function<ControlState(int row)> state_evaluator)
      : QItemDelegate(parent), m_value_of_row(std::move(state_evaluator)), m_value_column(column)
  {
  }

  void paint(QPainter* painter, const QStyleOptionViewItem& option,
             const QModelIndex& index) const override
  {
    QItemDelegate::paint(painter, option, index);
    if (index.column() != m_value_column)
      return;

    painter->save();
    PaintValueBar(*painter, option.rect, m_value_of_row(index.row()));
    painter->restore();
  }

private:
  std::function<ControlState(int row)> m_value_of_row;
  int m_value_column;
};

// Read-only line showing parse errors, or the expression's live value once it parses.
class InputStateLineEdit : public QLineEdit
{
public:
  explicit InputStateLineEdit(std::function<ControlState()> state_evaluator)
      : m_current_value(std::move(state_evaluator))
  {
    setReadOnly(true);
  }

  void SetShouldPaintStateIndicator(bool value) { m_show_value = value; }

  void paintEvent(QPaintEvent* event) override
  {
    QLineEdit::paintEvent(event);
    if (!m_show_value)
      return;

    QPainter painter(this);
    PaintValueBar(painter, rect(), m_current_value());
  }

private:
  std::function<ControlState()> m_current_value;
  bool m_show_value = false;
};

// ---- Helper widgets --------------------------------------------------------------------------

ControlExpressionSyntaxHighlighter::ControlExpressionSyntaxHighlighter(QTextDocument* parent)
    : QObject(parent)
{
  connect(parent, &QTextDocument::contentsChanged, this, [this, parent] { Recolor(parent); });
}

void ControlExpressionSyntaxHighlighter::Recolor(QTextDocument* document)
{
  // Latin-1 keeps one byte per character, so token offsets line up with Qt's character
  // positions.
  EP::Lexer lexer(document->toPlainText().toLatin1().toStdString());
  std::vector<EP::Token> tokens;

  if (lexer.Tokenize(tokens) == EP::ParseStatus::Successful)
  {
    const auto parsed = EP::ParseTokens(tokens);
    if (parsed.status != EP::ParseStatus::Successful)
    {
      // Repeat the token the parser stopped at, marked invalid, so it gets squiggled.
      EP::Token culprit = *parsed.token;
      culprit.type = EP::TOK_INVALID;
      tokens.emplace_back(culprit);
    }
  }

  // Formats are applied per block layout so the edit history stays untouched.
  for (QTextBlock block = document->begin(); block.isValid(); block = block.next())
  {
    QTextLayout* const layout = block.layout();
    layout->clearFormats();

    const int block_start = block.position();
    const int block_end = block_start + block.length();
    QList<QTextLayout::FormatRange> ranges;

    for (const EP::Token& token : tokens)
    {
      int start = static_cast<int>(token.string_position) - block_start;
      int length = static_cast<int>(token.string_length);

      // Clip tokens that began in an earlier block.
      if (start < 0)
      {
        length += start;
        start = 0;
      }
      if (length <= 0)
        continue;
      if (start >= block_end)
        break;

      if (const auto format = FormatFor(token))
        ranges.emplace_back(
            QTextLayout::FormatRange{.start = start, .length = length, .format = *format});
    }

    layout->setFormats(ranges);
  }
}

void QComboBoxWithMouseWheelDisabled::wheelEvent(QWheelEvent*)
{
  // Swallowed on purpose.
}

// ---- IOWindow --------------------------------------------------------------------------------

IOWindow::IOWindow(MappingWindow* window, ControllerEmu::EmulatedController* controller,
                   ControlReference* ref, IOWindow::Type type)
    : QDialog(window), m_reference(ref), m_controller(controller), m_type(type),
      m_original_expression(ref->GetExpression())
{
  BuildWidgets();
  FillHelperCombos();
  SetUpControlTable();
  ArrangeWidgets();

  connect(window, &MappingWindow::Update, this, &IOWindow::Tick);
  connect(window, &MappingWindow::ConfigChanged, this, &IOWindow::SyncFromReference);
  connect(&Settings::Instance(), &Settings::ConfigChanged, this, &IOWindow::SyncFromReference);

  setWindowTitle(IsInputEditor() ? tr("Configure Input") : tr("Configure Output"));

  SyncFromReference();

  WireSelection();
  WireDetection();
  WireOutputTest();
  WireEditing();
}

void IOWindow::BuildWidgets()
{
  m_device_combo = new QComboBox();
  m_control_table = new QTableWidget();

  m_insert_button = new QPushButton(IsInputEditor() ? tr("Insert Input") : tr("Insert Output"));
  m_detect_button = new QPushButton(tr("Detect Input"), this);
  m_test_button = new QPushButton(tr("Test Output"), this);
  m_buttons = new QDialogButtonBox();
  m_clear_button = new QPushButton(tr("Clear"));
  m_multiplier = new QSpinBox();

  // Inputs show the reference's live state; outputs show the test level while a test runs.
  std::function<ControlState()> live_value;
  if (IsInputEditor())
  {
    live_value = [this] {
      const auto state_lock = m_controller->GetStateLock();
      return m_reference->GetState<ControlState>();
    };
  }
  else
  {
    live_value = [this] {
      const auto state_lock = m_controller->GetStateLock();
      return m_test_timer->isActive() * m_reference->range;
    };
  }
  m_status_line = new InputStateLineEdit(std::move(live_value));

  m_editor = new QPlainTextEdit();
  m_editor->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
  new ControlExpressionSyntaxHighlighter(m_editor->document());
}

void IOWindow::FillHelperCombos()
{
  const bool input = IsInputEditor();

  m_operator_combo = new QComboBoxWithMouseWheelDisabled(this);
  m_operator_combo->addItem(tr("Operators"));
  m_operator_combo->insertSeparator(1);
  if (input)
  {
    for (const QString& op : {tr("! Not"), tr("* Multiply"), tr("/ Divide"), tr("% Modulo"),
                              tr("+ Add"), tr("- Subtract"), tr("> Greater-than"),
                              tr("< Less-than"), tr("& And"), tr("^ Xor"), tr("? Conditional")})
    {
      m_operator_combo->addItem(op);
    }
  }
  m_operator_combo->addItem(tr("| Or"));
  m_operator_combo->addItem(tr("$ User Variable"));
  if (input)
    m_operator_combo->addItem(tr(", Comma"));

  // Function names are expression syntax and are not translated.
  static constexpr std::array function_names = {
      "if",    "timer", "toggle", "deadzone", "smooth", "hold", "tap",  "relative",
      "pulse", "sin",   "cos",    "tan",      "asin",   "acos", "atan", "atan2",
      "sqrt",  "pow",   "min",    "max",      "clamp",  "abs"};
  m_function_combo = new QComboBoxWithMouseWheelDisabled(this);
  m_function_combo->addItem(tr("Functions"));
  m_function_combo->insertSeparator(1);
  for (const char* name : function_names)
    m_function_combo->addItem(QString::fromLatin1(name));

  m_variable_combo = new QComboBoxWithMouseWheelDisabled(this);
  m_variable_combo->addItem(tr("User Variables"));
  m_variable_combo->setToolTip(tr("User defined variables usable in the control expression.\n"
                                  "You can use them to save or retrieve values between\n"
                                  "inputs and outputs of the same parent controller."));
  m_variable_combo->insertSeparator(m_variable_combo->count());
  m_variable_combo->addItem(tr("Reset Values"));
  m_variable_combo->insertSeparator(m_variable_combo->count());
}

void IOWindow::SetUpControlTable()
{
  // Inputs allow up to +/-1000%. Outputs only ever see 0 or 1, so more than 100% is useless,
  // but -100% is kept for flipping force-feedback direction.
  const int limit = IsInputEditor() ? 1000 : 100;
  m_multiplier->setMinimum(-limit);
  m_multiplier->setMaximum(limit);
  // i18n: Percentage symbol.
  m_multiplier->setSuffix(tr("%"));

  m_control_table->setTabKeyNavigation(false);

  QHeaderView* const columns = m_control_table->horizontalHeader();
  if (IsInputEditor())
  {
    // A second, fixed-width column shows each input's live value.
    m_control_table->setColumnCount(2);
    m_control_table->setColumnWidth(1, 64);
    columns->setSectionResizeMode(1, QHeaderView::Fixed);

    m_control_table->setItemDelegate(new InputStateDelegate(this, 1, [this](int row) {
      std::lock_guard guard(m_device_mutex);
      // Negative values are clamped away; values above one are still shown.
      const ControlState state = m_device->Inputs()[static_cast<std::size_t>(row)]->GetState();
      return std::max(state, 0.0);
    }));
  }
  else
  {
    m_control_table->setColumnCount(1);
  }

  columns->hide();
  columns->setSectionResizeMode(0, QHeaderView::Stretch);

  QHeaderView* const rows = m_control_table->verticalHeader();
  rows->hide();
  rows->setDefaultSectionSize(rows->minimumSectionSize());

  m_control_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  m_control_table->setSelectionBehavior(QAbstractItemView::SelectRows);
  m_control_table->setSelectionMode(QAbstractItemView::SingleSelection);
}

void IOWindow::ArrangeWidgets()
{
  auto* const page = new QVBoxLayout();
  page->addWidget(m_device_combo);

  auto* const multiplier_row = new QHBoxLayout();
  // i18n: Controller input values are multiplied by this percentage value.
  multiplier_row->addWidget(new QLabel(tr("Multiplier")));
  multiplier_row->addWidget(m_multiplier);

  // Right of the table: insert, detect/test, the helper combos and the multiplier.
  auto* const side = new QVBoxLayout();
  auto* const middle = new QHBoxLayout();
  middle->addWidget(m_control_table, 8);
  middle->addLayout(side, 1);

  side->addWidget(m_insert_button);
  QPushButton* const unused_action = IsInputEditor() ? m_test_button : m_detect_button;
  unused_action->hide();
  side->addWidget(IsInputEditor() ? m_detect_button : m_test_button);
  side->addWidget(m_variable_combo);
  side->addWidget(m_operator_combo);
  if (IsInputEditor())
    side->addWidget(m_function_combo);
  else
    m_function_combo->hide();
  side->addLayout(multiplier_row);

  page->addLayout(middle, 2);
  page->addWidget(m_editor, 1);
  page->addWidget(m_status_line);
  page->addWidget(m_buttons);

  m_buttons->addButton(m_clear_button, QDialogButtonBox::ActionRole);
  m_buttons->addButton(QDialogButtonBox::Ok);

  m_test_timer = new QTimer(this);
  m_test_timer->setSingleShot(true);

  setLayout(page);
}

void IOWindow::SyncFromReference()
{
  // Abort any detection or test in progress.
  emit DetectInputComplete();
  emit TestOutputComplete();

  const QSignalBlocker quiet(this);
  const auto state_lock = ControllerEmu::EmulatedController::GetStateLock();

  // Forced so the status line is refreshed even when the text is unchanged.
  ApplyExpression(m_reference->GetExpression(), UpdateMode::Force);

  m_editor->setPlainText(QString::fromStdString(m_reference->GetExpression()));
  m_editor->moveCursor(QTextCursor::End, QTextCursor::MoveAnchor);
  m_multiplier->setValue(static_cast<int>(m_reference->range * 100.0));

  if (m_device_qualifier.ToString().empty())
    m_device_qualifier = m_controller->GetDefaultDevice();

  FillDeviceCombo();
}

void IOWindow::Tick()
{
  m_control_table->viewport()->update();
  m_status_line->update();

  if (!m_detector)
    return;

  if (!m_detector->IsComplete())
  {
    m_detector->Update(DETECT_DURATION, {}, DETECT_DURATION);
    return;
  }

  const auto detections = m_detector->TakeResults();
  emit DetectInputComplete();
  if (detections.empty())
    return;

  // Highlight the first input that fired.
  const QString first = QString::fromStdString(detections.front().input->GetName());
  const auto matches = m_control_table->findItems(first, Qt::MatchFixedString);
  if (!matches.empty())
    m_control_table->setCurrentItem(matches.front());
}

void IOWindow::WireSelection()
{
  connect(m_insert_button, &QPushButton::clicked, this, [this] { InsertCurrentControl(); });
  connect(m_control_table, &QTableWidget::cellDoubleClicked, this,
          [this] { InsertCurrentControl(); });
  connect(&Settings::Instance(), &Settings::ReleaseDevices, this, &IOWindow::DropDeviceHandle);
  connect(&Settings::Instance(), &Settings::DevicesChanged, this, &IOWindow::FillDeviceCombo);
}

void IOWindow::WireDetection()
{
  // "Detect" waits briefly before listening, so the click itself isn't picked up.
  auto* const delay = new QTimer(this);
  delay->setSingleShot(true);

  connect(m_detect_button, &QPushButton::clicked, this, [this, delay] {
    m_detect_button->setText(tr("[ ... ]"));
    delay->start(MappingCommon::INPUT_DETECT_INITIAL_DELAY);
  });

  connect(delay, &QTimer::timeout, this, [this] {
    m_detect_button->setText(tr("[ Press Now ]"));
    m_detector = std::make_unique<ciface::Core::InputDetector>();
    const auto state_lock = m_controller->GetStateLock();
    m_detector->Start(g_controller_interface, std::array{m_device_qualifier.ToString()});
    QtUtils::InstallKeyboardBlocker(m_detect_button, this, &IOWindow::DetectInputComplete);
  });

  connect(this, &IOWindow::DetectInputComplete, this,
          [this, delay, idle_label = m_detect_button->text()] {
            delay->stop();
            m_detector.reset();
            m_detect_button->setText(idle_label);
          });
}

void IOWindow::WireOutputTest()
{
  connect(m_test_button, &QPushButton::clicked, this, [this] {
    // A second click ends a running test.
    if (m_test_timer->isActive())
    {
      emit TestOutputComplete();
      return;
    }
    m_test_button->setText(QStringLiteral("[ ... ]"));
    m_test_timer->start(TEST_DURATION);
    const auto state_lock = m_controller->GetStateLock();
    m_reference->State(1.0);
  });

  connect(m_test_timer, &QTimer::timeout, this, [this] { emit TestOutputComplete(); });

  connect(this, &IOWindow::TestOutputComplete, this, [this, idle_label = m_test_button->text()] {
    m_test_timer->stop();
    m_test_button->setText(idle_label);
    const auto state_lock = m_controller->GetStateLock();
    m_reference->State(0.0);
  });
}

void IOWindow::WireEditing()
{
  connect(m_buttons, &QDialogButtonBox::clicked, this, &IOWindow::OnButtonBoxClicked);
  connect(m_device_combo, &QComboBox::currentTextChanged, this, &IOWindow::OnDeviceSelected);
  connect(m_multiplier, &QSpinBox::valueChanged, this, &IOWindow::OnMultiplierChanged);

  connect(m_editor, &QPlainTextEdit::textChanged, this,
          [this] { ApplyExpression(m_editor->toPlainText().toStdString()); });

  connect(m_variable_combo, &QComboBox::activated, this, [this](int index) {
    if (index == 0)
      return;

    if (index == VARIABLE_COMBO_RESET_INDEX)
    {
      const auto state_lock = ControllerEmu::EmulatedController::GetStateLock();
      m_controller->ResetExpressionVariables();
    }
    else
    {
      m_editor->insertPlainText(QLatin1Char('$') + m_variable_combo->currentText());
    }
    m_variable_combo->setCurrentIndex(0);
  });

  connect(m_operator_combo, &QComboBox::activated, this, [this](int index) {
    if (index == 0)
      return;
    // Each entry starts with the operator character itself.
    m_editor->insertPlainText(m_operator_combo->currentText().left(1));
    m_operator_combo->setCurrentIndex(0);
  });

  connect(m_function_combo, &QComboBox::activated, this, [this](int index) {
    if (index == 0)
      return;
    m_editor->insertPlainText(m_function_combo->currentText() + QStringLiteral("()"));
    m_function_combo->setCurrentIndex(0);
  });

  // Closing any way other than OK restores the original expression (which also stops a test).
  connect(this, &IOWindow::finished, this, [this] { ApplyExpression(m_original_expression); });
}

void IOWindow::InsertCurrentControl()
{
  const int row = m_control_table->currentRow();
  if (row < 0)
    return;

  const std::string control = m_control_table->item(row, 0)->text().toStdString();
  const std::string expression = ciface::MappingCommon::GetExpressionForControl(
      control, m_device_qualifier, m_controller->GetDefaultDevice());
  m_editor->insertPlainText(QString::fromStdString(expression));
}

void IOWindow::OnDeviceSelected()
{
  std::string device;
  if (m_device_combo->count() > 0)
    device = m_device_combo->currentData().toString().toStdString();
  m_device_qualifier.FromString(device);
  FillControlTable();
}

void IOWindow::OnButtonBoxClicked(QAbstractButton* button)
{
  if (button == m_clear_button)
  {
    m_editor->clear();
    return;
  }

  // Anything else is OK.
  const auto state_lock = ControllerEmu::EmulatedController::GetStateLock();
  ApplyExpression(m_editor->toPlainText().toStdString());

  if (m_reference->GetParseStatus() == EP::ParseStatus::SyntaxError)
  {
    ModalMessageBox::warning(this, tr("Error"), tr("The expression contains a syntax error."));
    return;
  }

  m_original_expression = m_reference->GetExpression();
  accept();
}

void IOWindow::OnMultiplierChanged(int percent)
{
  m_reference->range = percent / 100.0;
  emit TestOutputComplete();
}

void IOWindow::DropDeviceHandle()
{
  std::lock_guard guard(m_device_mutex);
  m_device = nullptr;
}

void IOWindow::FillControlTable()
{
  std::lock_guard guard(m_device_mutex);
  m_device = g_controller_interface.FindDevice(m_device_qualifier);
  m_control_table->setRowCount(0);

  if (!m_device)
    return;

  const auto list_controls = [this](const auto& controls) {
    int row = 0;
    for (ciface::Core::Device::Control* const control : controls)
    {
      m_control_table->insertRow(row);
      if (control->IsHidden())
        m_control_table->hideRow(row);
      m_control_table->setItem(row, 0,
                               new QTableWidgetItem(QString::fromStdString(control->GetName())));
      ++row;
    }
  };

  if (m_reference->IsInput())
    list_controls(m_device->Inputs());
  else
    list_controls(m_device->Outputs());
}

void IOWindow::FillDeviceCombo()
{
  const QSignalBlocker quiet(m_device_combo);

  const std::string previous = m_device_combo->currentData().toString().toStdString();
  m_device_combo->clear();

  const auto tag = [](const QString& word) {
    return QLatin1Char{'['} + word + QStringLiteral("] ");
  };

  // Every device gets an entry (the default one tagged). The previous choice is kept even if
  // it vanished, tagged as disconnected, since a refresh may bring it straight back.
  const std::string default_name = m_controller->GetDefaultDevice().ToString();
  int default_index = -1;
  int previous_index = -1;
  for (const std::string& name : g_controller_interface.GetAllDeviceStrings())
  {
    QString label;
    if (name == default_name)
    {
      default_index = m_device_combo->count();
      label += tag(tr("default"));
    }
    if (name == previous)
      previous_index = m_device_combo->count();

    label += QString::fromStdString(name);
    m_device_combo->addItem(label, QString::fromStdString(name));
  }

  if (previous_index >= 0)
  {
    m_device_combo->setCurrentIndex(previous_index);
  }
  else if (!previous.empty())
  {
    const QString missing = QString::fromStdString(previous);
    QString label;
    if (previous == default_name)
      label += tag(tr("default"));
    label += tag(tr("disconnected"));
    label += missing;
    m_device_combo->addItem(label, missing);
    m_device_combo->setCurrentIndex(m_device_combo->count() - 1);
  }
  else if (default_index >= 0)
  {
    m_device_combo->setCurrentIndex(default_index);
  }
  else if (m_device_combo->count() > 0)
  {
    m_device_combo->setCurrentIndex(0);
  }

  // Always re-resolve: the device object may have been replaced.
  OnDeviceSelected();
}

void IOWindow::RefreshVariableItems()
{
  while (m_variable_combo->count() > VARIABLE_COMBO_FIXED_ITEMS)
    m_variable_combo->removeItem(m_variable_combo->count() - 1);

  for (const auto& variable : m_controller->GetExpressionVariables())
    m_variable_combo->addItem(QString::fromStdString(variable.first));
}

void IOWindow::ShowParseResult(const std::optional<std::string>& error,
                               ciface::ExpressionParser::ParseStatus status)
{
  QString message;
  bool parsed = false;

  if (error)
    message = QString::fromStdString(*error);
  else if (status == EP::ParseStatus::EmptyExpression)
    message = QString();
  else if (status != EP::ParseStatus::Successful)
    message = tr("Invalid Expression.");
  else
    parsed = true;

  m_status_line->SetShouldPaintStateIndicator(parsed);
  m_status_line->setText(message);
}

void IOWindow::ApplyExpression(std::string expression, UpdateMode mode)
{
  emit TestOutputComplete();

  const auto state_lock = m_controller->GetStateLock();
  if (mode == UpdateMode::Normal && expression == m_reference->GetExpression())
    return;

  const auto error = m_reference->SetExpression(std::move(expression));
  const auto status = m_reference->GetParseStatus();
  m_controller->UpdateSingleControlReference(g_controller_interface, m_reference);

  // Variables can only appear or disappear when the expression changes.
  RefreshVariableItems();

  ShowParseResult(error, status);
}
