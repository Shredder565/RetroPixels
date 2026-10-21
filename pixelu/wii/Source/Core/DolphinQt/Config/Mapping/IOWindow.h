// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include <QComboBox>
#include <QDialog>
#include <QString>

#include "InputCommon/ControllerInterface/CoreDevice.h"

class ControlReference;
class MappingWindow;
class QAbstractButton;
class QDialogButtonBox;
class QHBoxLayout;
class QLineEdit;
class QTableWidget;
class QVBoxLayout;
class QWidget;
class QPlainTextEdit;
class QPushButton;
class QSlider;
class QSpinBox;
class QTextDocument;
class QTimer;

namespace ControllerEmu
{
class EmulatedController;
}

namespace ciface::ExpressionParser
{
enum class ParseStatus;
}

class InputStateLineEdit;

// Colours the tokens of a control expression as the document changes.
class ControlExpressionSyntaxHighlighter final : public QObject
{
  Q_OBJECT
public:
  explicit ControlExpressionSyntaxHighlighter(QTextDocument* parent);

private:
  void Recolor(QTextDocument* document);
};

// Combo box that ignores the mouse wheel, so scrolling the dialog can't change it.
class QComboBoxWithMouseWheelDisabled : public QComboBox
{
  Q_OBJECT
public:
  explicit QComboBoxWithMouseWheelDisabled(QWidget* parent = nullptr) : QComboBox(parent) {}

protected:
  void wheelEvent(QWheelEvent* event) override;
};

// Editor for a single control reference's expression: pick a device and control, combine
// with operators/functions/variables, set the multiplier, and detect inputs or test outputs.
class IOWindow final : public QDialog
{
  Q_OBJECT
public:
  enum class Type
  {
    Input,
    Output
  };

  explicit IOWindow(MappingWindow* window, ControllerEmu::EmulatedController* controller,
                    ControlReference* ref, Type type);

signals:
  void DetectInputComplete();
  void TestOutputComplete();

private:
  enum class UpdateMode
  {
    Normal,
    Force,
  };

  bool IsInputEditor() const { return m_type == Type::Input; }

  void BuildWidgets();
  void FillHelperCombos();
  void SetUpControlTable();
  void ArrangeWidgets();

  void WireSelection();
  void WireDetection();
  void WireOutputTest();
  void WireEditing();

  void SyncFromReference();
  void Tick();

  void OnButtonBoxClicked(QAbstractButton* button);
  void OnDeviceSelected();
  void OnMultiplierChanged(int percent);

  void InsertCurrentControl();
  void FillControlTable();
  void FillDeviceCombo();
  void DropDeviceHandle();
  void RefreshVariableItems();
  void ShowParseResult(const std::optional<std::string>& error,
                       ciface::ExpressionParser::ParseStatus status);

  void ApplyExpression(std::string expression, UpdateMode mode = UpdateMode::Normal);

  ControlReference* const m_reference;
  ControllerEmu::EmulatedController* const m_controller;
  const Type m_type;

  // Expression to restore if the dialog is closed without OK.
  std::string m_original_expression;

  ciface::Core::DeviceQualifier m_device_qualifier;
  std::shared_ptr<ciface::Core::Device> m_device;
  std::mutex m_device_mutex;

  std::unique_ptr<ciface::Core::InputDetector> m_detector;

  QComboBox* m_device_combo;
  QTableWidget* m_control_table;
  QPushButton* m_insert_button;
  QPushButton* m_detect_button;
  QPushButton* m_test_button;
  QDialogButtonBox* m_buttons;
  QPushButton* m_clear_button;
  QSpinBox* m_multiplier;
  InputStateLineEdit* m_status_line;
  QPlainTextEdit* m_editor;
  QComboBox* m_operator_combo;
  QComboBox* m_function_combo;
  QComboBox* m_variable_combo;
  QTimer* m_test_timer;
};
