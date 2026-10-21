// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/Mapping/MappingIndicator.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

#include <fmt/format.h>

#include <QAction>
#include <QDateTime>
#include <QEasingCurve>
#include <QPainter>
#include <QPainterPath>

#include "Common/MathUtil.h"

#include "Core/HW/WiimoteEmu/Camera.h"

#include "InputCommon/ControllerEmu/Control/Control.h"
#include "InputCommon/ControllerEmu/ControlGroup/Cursor.h"
#include "InputCommon/ControllerEmu/ControlGroup/Force.h"
#include "InputCommon/ControllerEmu/ControlGroup/MixedTriggers.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/ControllerInterface/CoreDevice.h"
#include "InputCommon/ControllerInterface/MappingCommon.h"

#include "DolphinQt/Config/Mapping/MappingWidget.h"
#include "DolphinQt/Config/Mapping/MappingWindow.h"
#include "DolphinQt/Settings.h"

namespace
{
// Gate fill colours per kind of input.
const QColor GATE_STICK = Qt::lightGray;
const QColor GATE_C_STICK = Qt::yellow;
const QColor GATE_CURSOR = 0xaed6f1;
const QColor GATE_TILT = 0xa2d9ce;
const QColor GATE_SWING = 0xcea2d9;

// Pixel geometry.
constexpr int DOT_RADIUS = 2;
constexpr int SQUARE_SIDE = 100;
constexpr int SQUARE_MARGIN = 2;
constexpr int TRIGGER_ROW_HEIGHT = 32;

// Sphere previews (accelerometer, gyroscope), in indicator units.
constexpr float BALL_RADIUS = 0.7f;
constexpr float MARKER_DISTANCE = 0.85f;
constexpr int BALL_DOTS = 200;

// Stick gates are approximated by this many points (a multiple of eight).
constexpr int GATE_POINTS = 32;

QPen Cosmetic(QPen pen)
{
  pen.setCosmetic(true);
  return pen;
}

// A round, cosmetic pen that draws a single point as a dot.
QPen DotPen(QPen pen)
{
  pen.setWidth(DOT_RADIUS * 2);
  pen.setCapStyle(Qt::PenCapStyle::RoundCap);
  return Cosmetic(pen);
}

QPointF Polar(double angle, double radius)
{
  return {std::cos(angle) * radius, std::sin(angle) * radius};
}

// Closed outline built by asking for a radius at evenly spaced angles.
template <typename RadiusAt>
QPolygonF TraceOutline(RadiusAt&& radius_at)
{
  QPolygonF outline{GATE_POINTS};
  for (int index = 0; index < GATE_POINTS; ++index)
  {
    const double angle = MathUtil::TAU * index / outline.size();
    outline[index] = Polar(angle, radius_at(angle));
  }
  return outline;
}

// A six-point band hugging the outline around one direction. TAU is added first so that the
// half-width subtracted below never pushes the angle negative.
template <typename RadiusAt>
QPolygonF TraceNotch(RadiusAt&& radius_at, double direction, double width, double depth)
{
  const double mid = direction + MathUtil::TAU;
  const double low = mid - width / 2;
  const double high = mid + width / 2;

  const double mid_outer = radius_at(mid);
  const double low_outer = radius_at(low);
  const double high_outer = radius_at(high);

  QPolygonF band{6};
  band[0] = Polar(low, low_outer - depth);
  band[1] = Polar(mid, mid_outer - depth);
  band[2] = Polar(high, high_outer - depth);
  band[3] = Polar(high, high_outer);
  band[4] = Polar(mid, mid_outer);
  band[5] = Polar(low, low_outer);
  return band;
}

// True when the point lies well past the calibrated outline, i.e. the stick is miscalibrated.
bool BeyondCalibration(Common::DVec2 point, ControllerEmu::ReshapableInput& input)
{
  constexpr double TOLERANCE = 1.3;

  const auto center = input.GetCenter();
  const double distance = (point - center).Length();
  const double direction = std::atan2(point.y - center.y, point.x - center.x);
  return distance > input.GetInputRadiusAtAngle(direction + MathUtil::TAU) * TOLERANCE;
}

void PaintNotches(QPainter& p, ControllerEmu::ReshapableInput& stick, QColor color)
{
  const double width = stick.GetVirtualNotchSize();
  if (width <= 0.0)
    return;

  const auto gate_radius = [&stick](double angle) { return stick.GetGateRadiusAtAngle(angle); };
  constexpr double step = MathUtil::TAU / 8.0;
  constexpr double depth = 1.0 - ControllerEmu::MINIMUM_NOTCH_DISTANCE;

  p.setBrush(color);
  for (int notch = 0; notch < 8; ++notch)
    p.drawPolygon(TraceNotch(gate_radius, step * notch, width, depth));
}

// Evenly spread points on a unit sphere (golden-angle spiral).
template <typename Visit>
void ForEachSpherePoint(int count, Visit&& visit)
{
  const auto turn = static_cast<float>(MathUtil::PI * (3.f - std::sqrt(5.f)));
  const float z_first = 1.f / count - 1.f;
  const float z_step = 2.f / count;

  for (int index = 0; index != count; ++index)
  {
    const float z = z_first + z_step * index;
    const float ring = std::sqrt(1.f - z * z);
    visit(Common::Vec3{std::cos(turn * index) * ring, std::sin(turn * index) * ring, z});
  }
}

// Dots of the rotated sphere's front half.
// Works with any rotation type that can be applied to a Vec3 (quaternion or matrix).
template <typename Rotation>
void PaintBall(QPainter& p, const Rotation& rotation, QColor color)
{
  p.setPen(Cosmetic(QPen(color, 0.5)));
  ForEachSpherePoint(BALL_DOTS, [&](const Common::Vec3& dot) {
    const auto turned = rotation * dot;
    if (turned.y > 0)
      p.drawPoint(QPointF(turned.x, turned.z) * BALL_RADIUS);
  });
}

// A marker is drawn when it faces the viewer or sticks out past the ball's silhouette.
void PaintMarker(QPainter& p, const Common::Vec3& where, QColor color)
{
  const bool in_front = where.y > 0;
  if (in_front || Common::Vec2(where.x, where.z).Length() > BALL_RADIUS)
  {
    p.setPen(DotPen(color));
    p.drawPoint(QPointF(where.x, where.z));
  }
}

// Sketch of a stick tilted along +x by |amount|: filled cap plus an outlined inner ring.
void PaintTiltedStick(QPainter& p, ReshapableInputIndicator& indicator, double amount)
{
  QColor cap = indicator.GetGateBrushColor();
  indicator.AdjustGateColor(&cap);
  p.setPen(QPen{cap.darker(125), 0});
  p.setBrush(cap);
  constexpr float cap_radius = 0.65f;
  p.drawEllipse(QPointF{amount * 0.35f, 0.f}, cap_radius, cap_radius);

  p.setPen(QPen{indicator.GetRawInputColor(), 0});
  p.setBrush(Qt::NoBrush);
  constexpr float ring_radius = 0.45f;
  p.drawEllipse(QPointF{amount * 0.45f, 0.f}, ring_radius, ring_radius);
}

}  // namespace

// Palette ------------------------------------------------------------------------------------

QPen MappingIndicator::GetBBoxPen() const
{
  return QPen(palette().shadow().color(), 0);
}

QBrush MappingIndicator::GetBBoxBrush() const
{
  return palette().base();
}

QColor MappingIndicator::GetRawInputColor() const
{
  QColor faded = palette().text().color();
  faded.setAlphaF(0.5);
  return faded;
}

QPen MappingIndicator::GetInputShapePen() const
{
  return QPen{GetRawInputColor(), 0.0, Qt::DashLine};
}

QColor MappingIndicator::GetAdjustedInputColor() const
{
  return Qt::red;
}

QColor MappingIndicator::GetCenterColor() const
{
  return Qt::blue;
}

QColor MappingIndicator::GetDeadZoneColor() const
{
  QColor shade = Settings::Instance().IsThemeDark() ? Qt::white : Qt::black;
  shade.setAlphaF(0.25);
  return shade;
}

QPen MappingIndicator::GetDeadZonePen() const
{
  return QPen(GetDeadZoneColor(), 0);
}

QBrush MappingIndicator::GetDeadZoneBrush(QPainter& painter) const
{
  // Keep the hatching in device space so it doesn't scale with the indicator.
  QBrush hatch{GetDeadZoneColor(), Qt::FDiagPattern};
  hatch.setTransform(painter.transform().inverted());
  return hatch;
}

QColor MappingIndicator::GetTextColor() const
{
  return palette().text().color();
}

QColor MappingIndicator::GetAltTextColor() const
{
  // Readable on top of the adjusted-input fill.
  return palette().highlightedText().color();
}

void MappingIndicator::AdjustGateColor(QColor* color)
{
  if (!Settings::Instance().IsThemeDark())
    return;
  color->setHsvF(color->hueF(), std::min(color->saturationF(), 0.5f), color->valueF() * 0.35f);
}

void MappingIndicator::paintEvent(QPaintEvent*)
{
  // Long gaps between repaints (hidden tab, stalls) are clamped so motion doesn't jump.
  constexpr float longest_step = 0.1f;

  const auto now = Clock::now();
  const auto since_last = static_cast<float>(DT_s{now - m_previous_paint}.count());
  m_previous_paint = now;

  const auto state_lock = ControllerEmu::EmulatedController::GetStateLock();
  Update(std::min(since_last, longest_step));
  Draw();
}

// Button -------------------------------------------------------------------------------------

ButtonIndicator::ButtonIndicator(ControlReference* control_ref) : m_reference{control_ref}
{
  setSizePolicy(QSizePolicy::Policy::Fixed, QSizePolicy::Policy::Fixed);
}

QSize ButtonIndicator::sizeHint() const
{
  // As tall as a bracket in the current font.
  const int text_height = QFontMetrics(font()).boundingRect(QStringLiteral("[")).height();
  return QSize{DOT_RADIUS + 2, text_height};
}

void ButtonIndicator::Draw()
{
  QPainter p(this);
  p.setPen(GetBBoxPen());
  p.setBrush(GetBBoxBrush());
  p.drawRect(QRect{{0, 0}, size() - QSize{1, 1}});

  const ControlState level = std::clamp(m_reference->GetState<ControlState>(), 0.0, 1.0);
  QSizeF bar = size() - QSizeF{2, 2};
  bar.setHeight(bar.height() * level);

  // Fill upwards from the bottom edge.
  p.translate(0, height());
  p.scale(1, -1);

  p.setPen(Qt::NoPen);
  p.setBrush(std::lround(level) != 0 ? GetAdjustedInputColor() : GetRawInputColor());
  p.drawRect(QRectF{{1, 1}, bar});
}

// Square base --------------------------------------------------------------------------------

SquareIndicator::SquareIndicator()
{
  // The frame takes one extra pixel on each side, outside the margin.
  constexpr int outer = SQUARE_SIDE + 2 * (SQUARE_MARGIN + 1);
  setFixedWidth(outer);
  setFixedHeight(outer);
}

qreal SquareIndicator::GetContentsScale() const
{
  return (SQUARE_SIDE - 1.0) / 2;
}

void SquareIndicator::DrawBoundingBox(QPainter& p)
{
  constexpr qreal inset = SQUARE_MARGIN + 0.5;
  constexpr qreal side = SQUARE_SIDE + 1.0;
  p.setBrush(GetBBoxBrush());
  p.setPen(GetBBoxPen());
  p.drawRect(QRectF{inset, inset, side, side});
}

void SquareIndicator::TransformPainter(QPainter& p)
{
  p.setRenderHint(QPainter::Antialiasing, true);
  p.setRenderHint(QPainter::SmoothPixmapTransform, true);

  // Origin in the middle, one unit per half-side.
  p.translate(width() / 2.0, height() / 2.0);
  const qreal units = GetContentsScale();
  p.scale(units, units);
}

// Reshapable inputs --------------------------------------------------------------------------

void ReshapableInputIndicator::SetCalibrationWidget(CalibrationWidget* widget)
{
  m_calibration = widget;
}

bool ReshapableInputIndicator::IsCalibrating() const
{
  return m_calibration && m_calibration->IsActive();
}

void ReshapableInputIndicator::UpdateCalibrationWidget(Common::DVec2 point)
{
  if (m_calibration)
    m_calibration->Update(point);
}

void ReshapableInputIndicator::DrawReshapableInput(
    ControllerEmu::ReshapableInput& stick,
    std::optional<ControllerEmu::ReshapableInput::ReshapeData> adj_coord)
{
  QPainter p(this);
  DrawBoundingBox(p);
  TransformPainter(p);
  // Stick +y is up; the widget's is down.
  p.scale(1.0, -1.0);

  const auto raw = stick.GetReshapableState(false);
  UpdateCalibrationWidget(raw);

  if (IsCalibrating())
  {
    // The calibration animation replaces the normal picture.
    m_calibration->Draw(p, raw);
    return;
  }

  DrawUnderGate(p);

  QColor gate_fill = GetGateBrushColor();
  QColor gate_edge = gate_fill.darker(125);
  AdjustGateColor(&gate_fill);
  AdjustGateColor(&gate_edge);

  p.setPen(QPen(gate_edge, 0));
  p.setBrush(gate_fill);
  p.drawPolygon(TraceOutline([&stick](double angle) { return stick.GetGateRadiusAtAngle(angle); }));

  PaintNotches(p, stick, gate_edge);

  // Dead zone, input shape and centre are drawn relative to the calibrated centre.
  const auto center = stick.GetCenter();
  p.save();
  p.translate(center.x, center.y);

  p.setPen(GetDeadZonePen());
  p.setBrush(GetDeadZoneBrush(p));
  p.drawPolygon(
      TraceOutline([&stick](double angle) { return stick.GetDeadzoneRadiusAtAngle(angle); }));

  p.setPen(GetInputShapePen());
  p.setBrush(Qt::NoBrush);
  p.drawPolygon(
      TraceOutline([&stick](double angle) { return stick.GetInputRadiusAtAngle(angle); }));

  if (center.x || center.y)
  {
    p.setPen(DotPen(GetCenterColor()));
    p.drawPoint(QPointF{});
  }
  p.restore();

  p.setPen(DotPen(GetRawInputColor()));
  p.drawPoint(QPointF{raw.x, raw.y});

  if (adj_coord.has_value())
  {
    p.setPen(DotPen(GetAdjustedInputColor()));
    p.drawPoint(QPointF{adj_coord->x, adj_coord->y});
  }
}

QColor AnalogStickIndicator::GetGateBrushColor() const
{
  // The C-Stick gets its own colour.
  if (m_stick.name == "C-Stick")
    return GATE_C_STICK;
  return GATE_STICK;
}

void AnalogStickIndicator::Draw()
{
  const auto adjusted = m_stick.GetReshapableState(true);
  const bool moved = adjusted.x || adjusted.y;
  DrawReshapableInput(m_stick, moved ? std::make_optional(adjusted) : std::nullopt);
}

QColor TiltIndicator::GetGateBrushColor() const
{
  return GATE_TILT;
}

void TiltIndicator::Update(float elapsed_seconds)
{
  WiimoteEmu::EmulateTilt(&m_motion, &m_tilt, elapsed_seconds);
}

void TiltIndicator::Draw()
{
  // Angles in half-turns; a full turn is 2.
  auto tilt = Common::DVec2{-m_motion.angle.y, m_motion.angle.x} / MathUtil::PI;

  // Shrink a hair so exactly +1 stays +1, then wrap anything past half a turn back into
  // [-1, 1) so it stays inside the square.
  tilt *= 0.9999f;
  const auto wrap = [](double half_turns) { return std::fmod(half_turns + 2 + 1, 2) - 1; };
  tilt.x = wrap(tilt.x);
  tilt.y = wrap(tilt.y);

  const bool tilted = tilt.x || tilt.y;
  DrawReshapableInput(m_tilt, tilted ? std::make_optional(tilt) : std::nullopt);
}

QColor CursorIndicator::GetGateBrushColor() const
{
  return GATE_CURSOR;
}

void CursorIndicator::Draw()
{
  const auto pointer = m_cursor.GetState(true);
  std::optional<Common::DVec2> shown;
  if (pointer.IsVisible())
    shown = Common::DVec2(pointer.x, pointer.y);
  DrawReshapableInput(m_cursor, shown);
}

QColor SwingIndicator::GetGateBrushColor() const
{
  return GATE_SWING;
}

void SwingIndicator::Update(float elapsed_seconds)
{
  WiimoteEmu::EmulateSwing(&m_motion, &m_swing, elapsed_seconds);
}

void SwingIndicator::Draw()
{
  DrawReshapableInput(m_swing, Common::DVec2{-m_motion.position.x, m_motion.position.z});
}

void SwingIndicator::DrawUnderGate(QPainter& p)
{
  // Forward/backward (Z) dead zone as a horizontal band.
  const double band = m_swing.GetDeadzonePercentage();
  if (band > 0.0)
  {
    p.setPen(GetDeadZonePen());
    p.setBrush(GetDeadZoneBrush(p));
    p.drawRect(QRectF(-1, -band, 2, band * 2));
  }

  // Raw Z as a straight line across.
  const auto raw = m_swing.GetState(false);
  p.setPen(Cosmetic(QPen(GetRawInputColor(), DOT_RADIUS)));
  p.drawLine(QLineF(-1, raw.z, 1, raw.z));

  // Emulated Z as a line bent by the current swing angle.
  const auto& position = m_motion.position;
  const auto bend =
      std::max(std::abs(m_motion.angle.x), std::abs(m_motion.angle.z)) / MathUtil::TAU;
  if (!position.y && !bend)
    return;

  const double ends = -(position.y + bend);
  QPainterPath curve;
  curve.moveTo(-1.0, ends);
  curve.quadTo({0, -(position.y - bend)}, {1, ends});

  p.setBrush(Qt::NoBrush);
  p.setPen(Cosmetic(QPen(GetAdjustedInputColor(), DOT_RADIUS)));
  p.drawPath(curve);
}

// Triggers -----------------------------------------------------------------------------------

MixedTriggersIndicator::MixedTriggersIndicator(ControllerEmu::MixedTriggers& group)
    : m_triggers(group)
{
  setSizePolicy(QSizePolicy::Policy::Expanding, QSizePolicy::Policy::Ignored);
  const int rows = static_cast<int>(group.GetTriggerCount());
  setFixedHeight(rows * TRIGGER_ROW_HEIGHT + 1);
}

void MixedTriggersIndicator::Draw()
{
  // Controls are laid out as the digital buttons followed by the analog halves.
  constexpr int PAIR = 2;
  constexpr std::array<u16, PAIR> masks = {0x1, 0x2};

  const ControlState threshold = m_triggers.GetThreshold();
  const ControlState deadzone = m_triggers.GetDeadzone();

  std::array<ControlState, PAIR> raw_values;
  std::array<ControlState, PAIR> adjusted_values;
  u16 pressed_bits = 0;
  m_triggers.GetState(&pressed_bits, masks.data(), raw_values.data(), false);
  m_triggers.GetState(&pressed_bits, masks.data(), adjusted_values.data(), true);

  // Each row: analog bar on the left, a square button on the right.
  const int row_width = width() - 1;
  const int button_width = TRIGGER_ROW_HEIGHT;
  const int bar_width = row_width - button_width;

  QPainter p(this);
  p.setRenderHint(QPainter::TextAntialiasing, true);

  p.setPen(Qt::NoPen);
  p.setBrush(GetBBoxBrush());
  p.drawRect(QRectF(0.5, 0.5, row_width, TRIGGER_ROW_HEIGHT * PAIR));

  for (std::size_t row = 0; row != PAIR; ++row)
  {
    const QString bar_label = QString::fromStdString(m_triggers.controls[PAIR + row]->ui_name);
    const QString button_label = QString::fromStdString(m_triggers.controls[row]->ui_name);
    const bool pressed = (pressed_bits & masks[row]) != 0;

    const QRectF row_rect(0.5, 0.5, row_width, TRIGGER_ROW_HEIGHT);
    const QRectF bar_rect(0.5, 0.5, bar_width, TRIGGER_ROW_HEIGHT);
    const QRectF fill_rect(0.5, 0.5, adjusted_values[row] * bar_width, TRIGGER_ROW_HEIGHT);
    const QRectF button_rect(bar_width + 0.5, 0.5, button_width, TRIGGER_ROW_HEIGHT);

    // Label first, so the fill covers it; the covered part is redrawn inverted below.
    p.setPen(GetTextColor());
    p.drawText(bar_rect, Qt::AlignCenter, bar_label);

    p.setPen(Qt::NoPen);
    p.setBrush(GetAdjustedInputColor());
    p.drawRect(fill_rect);

    // Raw value as a dot along the bottom edge.
    p.setPen(DotPen(GetRawInputColor()));
    p.drawPoint(
        QPoint(static_cast<int>(raw_values[row] * bar_width), TRIGGER_ROW_HEIGHT - DOT_RADIUS));

    p.setPen(GetDeadZonePen());
    p.setBrush(GetDeadZoneBrush(p));
    p.drawRect(QRectF(1.5, 1.5, bar_width * deadzone, TRIGGER_ROW_HEIGHT - 2));

    const int threshold_at = static_cast<int>(bar_width * threshold);
    p.setPen(GetInputShapePen());
    p.drawLine(threshold_at, 0, threshold_at, TRIGGER_ROW_HEIGHT);

    p.setPen(GetBBoxPen());
    p.setBrush(pressed ? QBrush(GetAdjustedInputColor()) : GetBBoxBrush());
    p.drawRect(button_rect);

    p.setPen(GetBBoxPen());
    p.setBrush(Qt::NoBrush);
    p.drawRect(row_rect);

    p.setPen(pressed ? GetAltTextColor() : GetTextColor());
    p.drawText(button_rect, Qt::AlignCenter, button_label);

    p.setPen(GetAltTextColor());
    p.setClipping(true);
    p.setClipRect(fill_rect);
    p.drawText(bar_rect, Qt::AlignCenter, bar_label);
    p.setClipping(false);

    p.translate(0.0, TRIGGER_ROW_HEIGHT);
  }
}

// Shake --------------------------------------------------------------------------------------

void ShakeMappingIndicator::Update(float elapsed_seconds)
{
  WiimoteEmu::EmulateShake(&m_motion, &m_shake, elapsed_seconds);

  for (HistoryPoint& point : m_history)
    point.age += elapsed_seconds;

  // Ages only grow towards the back, so everything after the first stale entry goes.
  const auto stale =
      std::ranges::find_if(m_history, [](const HistoryPoint& point) { return point.age > 1.f; });
  m_history.erase(stale, m_history.end());

  // Half a unit of travel fills the indicator.
  constexpr float full_scale = 0.5f;
  m_history.push_front(HistoryPoint{m_motion.position / full_scale});

  const bool moving = std::ranges::any_of(
      m_history, [](const HistoryPoint& point) { return point.state.LengthSquared() != 0.0; });

  // The sweep line rests at zero until something moves, then loops once a second.
  if (!m_sweep && !moving)
    return;

  m_sweep += elapsed_seconds;
  if (m_sweep > 1.f)
    m_sweep = moving ? std::fmod(m_sweep, 1.f) : 0;
}

void ShakeMappingIndicator::Draw()
{
  QPainter p(this);
  DrawBoundingBox(p);
  TransformPainter(p);
  // Positive acceleration points up.
  p.scale(1.0, -1.0);

  const double band = m_shake.GetDeadzone();
  if (band > 0.0)
  {
    p.setPen(GetDeadZonePen());
    p.setBrush(GetDeadZoneBrush(p));
    p.drawRect(QRectF(-1, 0, 2, band));
  }

  // Current raw X/Y/Z as three dots side by side.
  const auto raw = m_shake.GetState(false);
  const std::size_t axes = raw.data.size();
  p.setPen(DotPen(GetRawInputColor()));
  for (std::size_t axis = 0; axis != axes; ++axis)
    p.drawPoint(QPointF{-0.5 + axis * 0.5, raw.data[axis]});

  const double sweep_x = 1.0 - m_sweep * 2.0;
  p.setPen(QPen(GetRawInputColor(), 0));
  p.drawLine(QPointF{sweep_x, -1.0}, QPointF{sweep_x, 1.0});

  // One trace per axis, newest at the right edge.
  const std::array<QColor, 3> trace_colors = {Qt::blue, Qt::green, Qt::red};
  p.setBrush(Qt::NoBrush);
  for (std::size_t axis = 0; axis != axes; ++axis)
  {
    QPolygonF trace;
    for (const HistoryPoint& point : m_history)
      trace.append(QPointF{1.0 - point.age * 2.0, point.state.data[axis]});

    p.setPen(QPen(trace_colors[axis], 0));
    p.drawPolyline(trace);
  }
}

// Accelerometer ------------------------------------------------------------------------------

void AccelerometerMappingIndicator::Draw()
{
  const std::optional<Common::Vec3> reading = m_accelerometer.GetState();
  const Common::Vec3 accel = reading.value_or(Common::Vec3{});
  const auto rotation = WiimoteEmu::GetRotationFromAcceleration(accel);

  QPainter p(this);
  p.setRenderHint(QPainter::TextAntialiasing, true);
  DrawBoundingBox(p);
  TransformPainter(p);
  // The remote's axes run opposite to the screen's on both X and Z.
  p.scale(-1.0, -1.0);

  PaintBall(p, rotation, GetRawInputColor());

  p.setPen(QPen(GetRawInputColor(), 0));
  p.setBrush(Qt::NoBrush);
  p.drawEllipse(QPointF{}, BALL_RADIUS, BALL_RADIUS);

  p.setPen(Qt::NoPen);
  const auto top = rotation * Common::Vec3{0, 0, MARKER_DISTANCE};
  PaintMarker(p, top, GetAdjustedInputColor());
  PaintMarker(p, -top, GetCenterColor());

  // Targets are drawn in plain pixel space.
  p.setBrush(Qt::NoBrush);
  p.resetTransform();
  p.translate(width() / 2.0, height() / 2.0);

  const qreal units = GetContentsScale();
  p.setPen(GetAdjustedInputColor());
  p.drawEllipse(QPointF{0, -MARKER_DISTANCE} * units, DOT_RADIUS, DOT_RADIUS);
  p.setPen(GetCenterColor());
  p.drawEllipse(QPointF{0, MARKER_DISTANCE} * units, DOT_RADIUS, DOT_RADIUS);

  if (!reading)
    return;

  const QRect corner(0, 0, SQUARE_SIDE / 2 - 2, SQUARE_SIDE / 2 - 1);
  // i18n: "g" is the symbol for "gravitational force equivalent" (g-force).
  const std::string g_force =
      fmt::format("{:.2f} g", accel.Length() / WiimoteEmu::GRAVITY_ACCELERATION);
  p.setPen(GetTextColor());
  p.drawText(corner, Qt::AlignBottom | Qt::AlignRight, QString::fromStdString(g_force));
}

// Gyroscope ----------------------------------------------------------------------------------

void GyroMappingIndicator::Update(float elapsed_seconds)
{
  const Common::Vec3 velocity = m_gyroscope.GetState().value_or(Common::Vec3{});

  const Common::Vec3 screen_axes(-1, +1, -1);
  m_orientation *= WiimoteEmu::GetRotationFromGyroscope(velocity * screen_axes * elapsed_seconds);
  m_orientation = m_orientation.Normalized();

  // After a second without (dead-zoned) motion the picture snaps back upright.
  constexpr float settle_time = 1.f;
  if (velocity.LengthSquared())
    m_still_seconds = 0;
  else if (m_still_seconds < settle_time)
    m_still_seconds += elapsed_seconds;

  if (m_still_seconds >= settle_time)
    m_orientation = Common::Quaternion::Identity();
}

void GyroMappingIndicator::Draw()
{
  const std::optional<Common::Vec3> reading = m_gyroscope.GetState();
  const Common::Vec3 raw_velocity = m_gyroscope.GetRawState();
  const Common::Vec3 velocity = reading.value_or(Common::Vec3{});
  const Common::Vec3 noise = raw_velocity - m_last_raw_velocity;
  m_last_raw_velocity = raw_velocity;

  const bool still = !velocity.LengthSquared();
  const Common::Matrix33 rotation =
      reading ? Common::Matrix33::FromQuaternion(m_orientation) : Common::Matrix33{};

  QPainter p(this);
  DrawBoundingBox(p);
  TransformPainter(p);

  if (const auto deadzone = m_gyroscope.GetDeadzone(); deadzone)
  {
    // The strip below the ball shows the dead zone; a line in it shows the current noise.
    constexpr auto strip_height = 1 - BALL_RADIUS;
    constexpr auto strip_bottom = 1;

    p.setPen(GetDeadZonePen());
    p.setBrush(GetDeadZoneBrush(p));
    p.drawRect(QRectF{-1, strip_bottom, 2, -strip_height});

    if (reading)
    {
      const auto peak_noise = std::max({std::abs(noise.x), std::abs(noise.y), std::abs(noise.z)});
      const auto level = std::min(peak_noise / deadzone * strip_height - strip_bottom, 1.0);
      p.setPen(Cosmetic(QPen(GetRawInputColor(), DOT_RADIUS)));
      p.drawLine(QLineF(-1.0, -level, 1.0, -level));

      // Keep the line from showing through the ball.
      p.setPen(Qt::NoPen);
      p.setBrush(GetBBoxBrush());
      p.drawEllipse(QPointF{}, BALL_RADIUS, BALL_RADIUS);
    }
  }

  PaintBall(p, rotation, GetRawInputColor());

  QColor outline = GetAdjustedInputColor();
  if (still)
    outline = m_gyroscope.IsCalibrating() ? GetCenterColor() : GetRawInputColor();
  p.setPen(QPen(outline, 0));
  p.setBrush(Qt::NoBrush);
  p.drawEllipse(QPointF{}, BALL_RADIUS, BALL_RADIUS);

  p.setPen(Qt::NoPen);
  PaintMarker(p, rotation * Common::Vec3{0, 0, -MARKER_DISTANCE}, GetAdjustedInputColor());
  PaintMarker(p, rotation * Common::Vec3{0, MARKER_DISTANCE, 0}, GetCenterColor());

  p.setBrush(Qt::NoBrush);
  p.resetTransform();
  p.translate(width() / 2.0, height() / 2.0);

  p.setPen(GetAdjustedInputColor());
  p.drawEllipse(QPointF{0, -MARKER_DISTANCE} * GetContentsScale(), DOT_RADIUS, DOT_RADIUS);
  p.setPen(GetCenterColor());
  p.drawEllipse(QPointF{}, DOT_RADIUS, DOT_RADIUS);
}

// IR passthrough -----------------------------------------------------------------------------

void IRPassthroughMappingIndicator::Draw()
{
  QPainter p(this);
  DrawBoundingBox(p);
  TransformPainter(p);
  p.scale(1.0, -1.0);

  QPen dot =
      DotPen(m_passthrough.enabled.GetValue() ? GetAdjustedInputColor() : GetRawInputColor());

  constexpr auto object_count = static_cast<std::size_t>(WiimoteEmu::CameraLogic::NUM_POINTS);
  for (std::size_t object = 0; object != object_count; ++object)
  {
    const ControlState size = m_passthrough.GetObjectSize(object);
    if (!(size > 0))
      continue;

    // Camera space is 0..1; the indicator is -1..1.
    const QPointF camera{m_passthrough.GetObjectPositionX(object),
                         m_passthrough.GetObjectPositionY(object)};
    dot.setWidth(static_cast<int>(size * SQUARE_SIDE / 2));
    p.setPen(dot);
    p.drawPoint((camera - QPointF{0.5, 0.5}) * 2.0);
  }
}

// Calibration --------------------------------------------------------------------------------

CalibrationWidget::CalibrationWidget(MappingWidget& mapping_widget,
                                     ControllerEmu::ReshapableInput& input,
                                     ReshapableInputIndicator& indicator)
    : m_owner(mapping_widget), m_input(input), m_indicator(indicator)
{
  MappingWindow* const window = mapping_widget.GetParent();
  connect(window, &MappingWindow::CancelMapping, this, &CalibrationWidget::ShowIdleActions);
  connect(window, &MappingWindow::ConfigChanged, this, &CalibrationWidget::ShowIdleActions);

  m_indicator.SetCalibrationWidget(this);

  // The split button makes the extra menu entries discoverable.
  setPopupMode(ToolButtonPopupMode::MenuButtonPopup);
  setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);

  ShowIdleActions();
}

bool CalibrationWidget::IsActive() const
{
  return Mapping() || Calibrating();
}

bool CalibrationWidget::Mapping() const
{
  return m_mapper != nullptr;
}

bool CalibrationWidget::Calibrating() const
{
  return m_calibrator != nullptr;
}

void CalibrationWidget::InstallActions(std::initializer_list<QAction*> actions,
                                       QAction* default_action)
{
  // Drop whatever the button offered before; the new actions aren't attached yet.
  for (QAction* const old : this->actions())
    delete old;

  for (QAction* const action : actions)
    addAction(action);
  setDefaultAction(default_action);
}

void CalibrationWidget::ShowIdleActions()
{
  m_calibrator.reset();
  m_mapper.reset();

  // i18n: A button to start the process of game controller analog stick mapping and calibration.
  auto* const map_all = new QAction(tr("Map and Calibrate"), this);
  // i18n: A button to start the process of game controller analog stick calibration.
  auto* const calibrate = new QAction(tr("Calibrate"), this);
  // i18n: A button to calibrate the center and extremities of a game controller analog stick.
  auto* const calibrate_centre = new QAction(tr("Center and Calibrate"), this);
  // i18n: A button to reset game controller analog stick calibration.
  auto* const reset = new QAction(tr("Reset Calibration"), this);

  connect(map_all, &QAction::triggered, this, &CalibrationWidget::BeginMapping);
  connect(calibrate, &QAction::triggered, this, [this] { BeginCalibration(); });
  connect(calibrate_centre, &QAction::triggered, this, [this] { BeginCalibration(std::nullopt); });
  connect(reset, &QAction::triggered, this, [this] {
    const auto state_lock = m_owner.GetController()->GetStateLock();
    m_input.SetCalibrationToDefault();
    m_input.SetCenter({});
  });

  InstallActions({map_all, calibrate, calibrate_centre, reset}, map_all);
}

void CalibrationWidget::BeginMapping()
{
  StartAnimationClock();

  // i18n: A button to stop a game controller button mapping process.
  auto* const cancel = new QAction(tr("Cancel Mapping"), this);
  connect(cancel, &QAction::triggered, this, &CalibrationWidget::ShowIdleActions);
  InstallActions({cancel}, cancel);

  MappingWindow* const window = m_owner.GetParent();
  std::vector<std::string> devices{window->GetController()->GetDefaultDevice().ToString()};
  if (window->IsCreateOtherDeviceMappingsEnabled())
    devices = g_controller_interface.GetAllDeviceStrings();

  const auto state_lock = window->GetController()->GetStateLock();
  m_mapper = std::make_unique<ciface::MappingCommon::ReshapableInputMapper>(g_controller_interface,
                                                                            devices);
}

void CalibrationWidget::BeginCalibration(std::optional<Common::DVec2> center)
{
  StartAnimationClock();
  m_calibrator = std::make_unique<ciface::MappingCommon::CalibrationBuilder>(center);

  // i18n: A button to abort a game controller calibration process.
  auto* const cancel = new QAction(tr("Cancel Calibration"), this);
  connect(cancel, &QAction::triggered, this, &CalibrationWidget::ShowIdleActions);

  // i18n: A button to finalize a game controller calibration process.
  auto* const finish = new QAction(tr("Finish Calibration"), this);
  connect(finish, &QAction::triggered, this, &CalibrationWidget::CommitCalibration);

  InstallActions({finish, cancel}, cancel);
}

void CalibrationWidget::CommitCalibration()
{
  const auto state_lock = m_owner.GetController()->GetStateLock();
  m_calibrator->ApplyResults(&m_input);
  ShowIdleActions();
}

void CalibrationWidget::Update(Common::DVec2 point)
{
  // Called from the indicator's paint, with the controller state lock already held.
  QFont look = parentWidget()->font();
  QPalette colours = parentWidget()->palette();

  if (Mapping())
  {
    // Each direction that gets mapped restarts the prompt animation.
    if (m_mapper->Update())
      StartAnimationClock();

    if (m_mapper->IsComplete())
    {
      const bool wants_calibration = m_mapper->IsCalibrationNeeded();
      const bool applied = m_mapper->ApplyResults(m_owner.GetController(), &m_input);

      if (!applied)
      {
        ShowIdleActions();
      }
      else
      {
        emit m_owner.ConfigChanged();
        if (wants_calibration)
        {
          BeginCalibration();
        }
        else
        {
          // Digital directions get a square calibration around the origin.
          m_input.SetCalibrationFromGate(ControllerEmu::SquareStickGate{1});
          m_input.SetCenter({});
          ShowIdleActions();
        }
      }

      m_mapper.reset();
    }
  }
  else if (Calibrating())
  {
    m_calibrator->Update(point);

    if (!m_calibrator->IsCalibrationDataSensible())
      m_spin_down_deadline = Clock::now() + SPIN_DOWN_TIME;
    else if (m_calibrator->IsComplete())
      CommitCalibration();
  }
  else if (BeyondCalibration(point, m_input))
  {
    // Flag a miscalibrated stick with bold red text.
    look.setBold(true);
    colours.setColor(QPalette::ButtonText, Qt::red);
  }

  setFont(look);
  setPalette(colours);
}

double CalibrationWidget::SecondsSinceAnimationStart() const
{
  return DT_s{Clock::now() - m_animation_epoch}.count();
}

void CalibrationWidget::StartAnimationClock()
{
  m_animation_epoch = Clock::now();
}

void CalibrationWidget::Draw(QPainter& p, Common::DVec2 point)
{
  PaintMappingPrompt(p);
  PaintCalibrationProgress(p, point);
}

void CalibrationWidget::PaintMappingPrompt(QPainter& p)
{
  if (!Mapping())
    return;

  // Point towards the direction being asked for.
  p.rotate(qRadiansToDegrees(m_mapper->GetCurrentAngle()));

  // 0 -> 1 -> 0 once per second.
  const double phase = std::fmod(SecondsSinceAnimationStart(), 1);
  const double bounce = 1 - std::abs(1 - (2 * phase));

  PaintTiltedStick(p, m_indicator, QEasingCurve(QEasingCurve::OutBounce).valueForProgress(bounce));

  // An equilateral arrowhead sliding outwards.
  constexpr float tip = 0.2f;
  constexpr float half_base = tip / std::numbers::sqrt3_v<float>;
  const double slide = QEasingCurve(QEasingCurve::InOutQuart).valueForProgress(bounce) * 0.3 + 0.1;

  p.save();
  p.translate(slide, 0.0);
  p.setPen(Qt::NoPen);
  p.setBrush(m_indicator.GetRawInputColor());
  p.drawPolygon(QPolygonF{{tip, 0.f}, {0.f, -half_base}, {0.f, +half_base}});
  p.restore();
}

void CalibrationWidget::PaintCalibrationProgress(QPainter& p, Common::DVec2 point)
{
  if (!Calibrating())
    return;

  const double seconds = SecondsSinceAnimationStart();

  // The spin fades out over SPIN_DOWN_TIME once the data stops looking sensible.
  const double spin = std::max(DT_s{m_spin_down_deadline - Clock::now()} / SPIN_DOWN_TIME, 0.0);
  const double push =
      QEasingCurve(QEasingCurve::OutCirc).valueForProgress(std::min(seconds * 2, 1.0)) * spin;

  // A stick circling clockwise, starting from the middle.
  p.save();
  p.rotate(seconds * -360.0);
  PaintTiltedStick(p, m_indicator, -push);
  p.restore();

  const auto center = m_calibrator->GetCenter();
  p.save();
  p.translate(center.x, center.y);

  p.setPen(m_indicator.GetInputShapePen());
  p.setBrush(Qt::NoBrush);
  p.drawPolygon(TraceOutline(
      [this](double angle) { return m_calibrator->GetCalibrationRadiusAtAngle(angle); }));

  if (center.x || center.y)
  {
    p.setPen(DotPen(m_indicator.GetCenterColor()));
    p.drawPoint(QPointF{});
  }
  p.restore();

  // The input dot only appears past half travel; otherwise it clutters the spinning stick.
  constexpr double half_travel_squared = 0.5 * 0.5;
  if (Common::DVec2{point.x, point.y}.LengthSquared() > half_travel_squared)
  {
    p.setPen(DotPen(m_indicator.GetAdjustedInputColor()));
    p.drawPoint(QPointF{point.x, point.y});
  }
}
