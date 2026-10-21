// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QToolButton>
#include <QWidget>

#include <chrono>
#include <deque>

#include "Core/HW/WiimoteEmu/Dynamics.h"
#include "InputCommon/ControllerEmu/ControlGroup/IRPassthrough.h"
#include "InputCommon/ControllerEmu/StickGate.h"

namespace ControllerEmu
{
class Control;
class ControlGroup;
class Cursor;
class Force;
class MixedTriggers;
}  // namespace ControllerEmu

namespace ciface::MappingCommon
{
class ReshapableInputMapper;
class CalibrationBuilder;
}  // namespace ciface::MappingCommon

class QPainter;
class QPaintEvent;
class QTimer;

class CalibrationWidget;
class MappingWidget;

// Base of every live input preview. Subclasses advance their own state in Update() and
// paint in Draw(); both run under the emulated controller state lock on each repaint.
// The colour getters are public so helpers (and IOWindow) can share the palette.
class MappingIndicator : public QWidget
{
public:
  QPen GetBBoxPen() const;
  QBrush GetBBoxBrush() const;
  QColor GetRawInputColor() const;
  QPen GetInputShapePen() const;
  QColor GetCenterColor() const;
  QColor GetAdjustedInputColor() const;
  QColor GetDeadZoneColor() const;
  QPen GetDeadZonePen() const;
  QBrush GetDeadZoneBrush(QPainter&) const;
  QColor GetTextColor() const;
  QColor GetAltTextColor() const;
  void AdjustGateColor(QColor*);

protected:
  virtual void Draw() {}
  virtual void Update(float /*elapsed_seconds*/) {}

private:
  void paintEvent(QPaintEvent*) override;

  Clock::time_point m_previous_paint = Clock::now();
};

// A thin vertical bar that fills with a button's state.
class ButtonIndicator final : public MappingIndicator
{
public:
  ButtonIndicator(ControlReference* control_ref);

private:
  QSize sizeHint() const override;
  void Draw() override;

  ControlReference* const m_reference;
};

// Fixed-size square canvas with a framed area and a centred, scaled coordinate system.
class SquareIndicator : public MappingIndicator
{
protected:
  SquareIndicator();

  qreal GetContentsScale() const;
  void DrawBoundingBox(QPainter&);
  void TransformPainter(QPainter&);
};

// Square preview of a stick-like input: gate, dead zone, input shape and positions.
// An attached CalibrationWidget takes over the drawing while it is active.
class ReshapableInputIndicator : public SquareIndicator
{
public:
  void SetCalibrationWidget(CalibrationWidget* widget);

  virtual QColor GetGateBrushColor() const = 0;

protected:
  void DrawReshapableInput(ControllerEmu::ReshapableInput& group,
                           std::optional<ControllerEmu::ReshapableInput::ReshapeData> adj_coord);

  virtual void DrawUnderGate(QPainter&) {}

  bool IsCalibrating() const;

  void UpdateCalibrationWidget(Common::DVec2 point);

private:
  CalibrationWidget* m_calibration{};
};

class AnalogStickIndicator : public ReshapableInputIndicator
{
public:
  explicit AnalogStickIndicator(ControllerEmu::ReshapableInput& stick) : m_stick(stick) {}

  QColor GetGateBrushColor() const final;

private:
  void Draw() override;

  ControllerEmu::ReshapableInput& m_stick;
};

class TiltIndicator : public ReshapableInputIndicator
{
public:
  explicit TiltIndicator(ControllerEmu::Tilt& tilt) : m_tilt(tilt) {}

  QColor GetGateBrushColor() const final;

private:
  void Draw() override;
  void Update(float elapsed_seconds) override;

  ControllerEmu::Tilt& m_tilt;
  WiimoteEmu::MotionState m_motion{};
};

class CursorIndicator : public ReshapableInputIndicator
{
public:
  explicit CursorIndicator(ControllerEmu::Cursor& cursor) : m_cursor(cursor) {}

  QColor GetGateBrushColor() const final;

private:
  void Draw() override;

  ControllerEmu::Cursor& m_cursor;
};

class MixedTriggersIndicator : public MappingIndicator
{
public:
  explicit MixedTriggersIndicator(ControllerEmu::MixedTriggers& triggers);

private:
  void Draw() override;

  ControllerEmu::MixedTriggers& m_triggers;
};

class SwingIndicator : public ReshapableInputIndicator
{
public:
  explicit SwingIndicator(ControllerEmu::Force& swing) : m_swing(swing) {}

  QColor GetGateBrushColor() const final;

private:
  void Draw() override;
  void Update(float elapsed_seconds) override;

  void DrawUnderGate(QPainter& p) override;

  ControllerEmu::Force& m_swing;
  WiimoteEmu::MotionState m_motion{};
};

class ShakeMappingIndicator : public SquareIndicator
{
public:
  explicit ShakeMappingIndicator(ControllerEmu::Shake& shake) : m_shake(shake) {}

private:
  void Draw() override;
  void Update(float elapsed_seconds) override;

  struct HistoryPoint
  {
    ControllerEmu::Shake::StateData state;
    float age = 0.f;
  };

  ControllerEmu::Shake& m_shake;
  WiimoteEmu::MotionState m_motion{};

  // Newest first; entries older than a second are dropped.
  std::deque<HistoryPoint> m_history;
  // Sweeps from 0 to 1 once per second while there is movement.
  float m_sweep = 0;
};

class AccelerometerMappingIndicator : public SquareIndicator
{
public:
  explicit AccelerometerMappingIndicator(ControllerEmu::IMUAccelerometer& accel)
      : m_accelerometer(accel)
  {
  }

private:
  void Draw() override;

  ControllerEmu::IMUAccelerometer& m_accelerometer;
};

class GyroMappingIndicator : public SquareIndicator
{
public:
  explicit GyroMappingIndicator(ControllerEmu::IMUGyroscope& gyro) : m_gyroscope(gyro) {}

private:
  void Draw() override;
  void Update(float elapsed_seconds) override;

  ControllerEmu::IMUGyroscope& m_gyroscope;
  Common::Quaternion m_orientation = Common::Quaternion::Identity();
  Common::Vec3 m_last_raw_velocity = {};
  float m_still_seconds = 0;
};

class IRPassthroughMappingIndicator : public SquareIndicator
{
public:
  explicit IRPassthroughMappingIndicator(ControllerEmu::IRPassthrough& ir_group)
      : m_passthrough(ir_group)
  {
  }

private:
  void Draw() override;

  ControllerEmu::IRPassthrough& m_passthrough;
};

// Tool button under a stick indicator offering mapping and calibration. While a mapping or
// calibration is running it also draws the progress animation into the indicator.
class CalibrationWidget : public QToolButton
{
public:
  CalibrationWidget(MappingWidget& mapping_widget, ControllerEmu::ReshapableInput& input,
                    ReshapableInputIndicator& indicator);

  void Update(Common::DVec2 point);

  void Draw(QPainter& p, Common::DVec2 point);

  bool IsActive() const;

private:
  void PaintMappingPrompt(QPainter& p);
  void PaintCalibrationProgress(QPainter& p, Common::DVec2 point);

  bool Mapping() const;
  bool Calibrating() const;

  void BeginMapping();
  void BeginCalibration(std::optional<Common::DVec2> center = Common::DVec2{});
  void CommitCalibration();

  void ShowIdleActions();
  void InstallActions(std::initializer_list<QAction*> actions, QAction* default_action);

  double SecondsSinceAnimationStart() const;
  void StartAnimationClock();

  MappingWidget& m_owner;
  ControllerEmu::ReshapableInput& m_input;
  ReshapableInputIndicator& m_indicator;

  std::unique_ptr<ciface::MappingCommon::ReshapableInputMapper> m_mapper;
  std::unique_ptr<ciface::MappingCommon::CalibrationBuilder> m_calibrator;

  // How long the spinning stick keeps going after the data stops looking sensible.
  static constexpr auto SPIN_DOWN_TIME = std::chrono::seconds{2};

  Clock::time_point m_animation_epoch{};
  Clock::time_point m_spin_down_deadline{};
};
