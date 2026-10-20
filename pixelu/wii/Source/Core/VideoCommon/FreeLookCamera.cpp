// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/FreeLookCamera.h"

#include <algorithm>
#include <string>

#include <fmt/format.h>

#include "Common/ChunkFile.h"
#include "Common/Config/Config.h"
#include "Core/Core.h"

FreeLookCamera g_freelook_camera;

namespace
{
using Common::Matrix44;
using Common::Quaternion;
using Common::Vec3;

constexpr float SMALLEST_FOV = 0.025f;
constexpr float NORMAL_FOV = 1.0f;
constexpr float NORMAL_SPEED = 60.0f;

const char* ControlTypeName(FreeLook::ControlType type)
{
  switch (type)
  {
  case FreeLook::ControlType::SixAxis:
    return "Six Axis";
  case FreeLook::ControlType::FPS:
    return "First Person";
  case FreeLook::ControlType::Orbital:
    return "Orbital";
  }
  return "";
}

// Free six-degrees-of-freedom camera: every input is applied in view space on top of the
// current view matrix.
class SixAxisController final : public CameraControllerInput
{
public:
  Matrix44 GetView() const override { return m_view; }

  void MoveVertical(float amt) override { Apply(Matrix44::Translate(Vec3{0, amt, 0})); }
  void MoveHorizontal(float amt) override { Apply(Matrix44::Translate(Vec3{amt, 0, 0})); }
  void MoveForward(float amt) override { Apply(Matrix44::Translate(Vec3{0, 0, amt})); }

  void Rotate(const Vec3& amt) override { Rotate(Quaternion::RotateXYZ(amt)); }
  void Rotate(const Quaternion& quat) override { Apply(Matrix44::FromQuaternion(quat)); }

  void Reset() override
  {
    CameraControllerInput::Reset();
    m_view = Matrix44::Identity();
  }

  void DoState(PointerWrap& p) override
  {
    CameraControllerInput::DoState(p);
    p.Do(m_view);
  }

private:
  void Apply(const Matrix44& step) { m_view = step * m_view; }

  Matrix44 m_view = Matrix44::Identity();
};

// Shared part of the first-person and orbital cameras: accumulated pitch and yaw angles turned
// into one orientation quaternion.
class EulerCamera : public CameraControllerInput
{
public:
  void Rotate(const Vec3& amt) override
  {
    if (amt.Length() == 0)
      return;
    m_angles += amt;
    const Quaternion pitch = Quaternion::RotateX(m_angles.x);
    const Quaternion yaw = Quaternion::RotateY(m_angles.y);
    m_orientation = (pitch * yaw).Normalized();
  }

  void Rotate(const Quaternion& quat) override { Rotate(Common::FromQuaternionToEuler(quat)); }

protected:
  void ResetOrientation()
  {
    m_angles = Vec3{};
    m_orientation = Quaternion::Identity();
  }

  void DoOrientationState(PointerWrap& p)
  {
    p.Do(m_angles);
    p.Do(m_orientation);
  }

  // A view-space direction expressed in world space.
  Vec3 WorldDirection(const Vec3& view_dir) const { return m_orientation.Conjugate() * view_dir; }

  Vec3 m_angles{};
  Quaternion m_orientation = Quaternion::Identity();
};

// Walks through the scene: movement follows the view direction.
class FPSController final : public EulerCamera
{
public:
  Matrix44 GetView() const override
  {
    return Matrix44::FromQuaternion(m_orientation) * Matrix44::Translate(m_position);
  }

  void MoveVertical(float amt) override { m_position += WorldDirection(Vec3{0, 1, 0}) * amt; }
  void MoveHorizontal(float amt) override { m_position += WorldDirection(Vec3{1, 0, 0}) * amt; }
  void MoveForward(float amt) override { m_position += WorldDirection(Vec3{0, 0, 1}) * amt; }

  void Reset() override
  {
    CameraControllerInput::Reset();
    m_position = Vec3{};
    ResetOrientation();
  }

  void DoState(PointerWrap& p) override
  {
    CameraControllerInput::DoState(p);
    DoOrientationState(p);
    p.Do(m_position);
  }

private:
  Vec3 m_position{};
};

// Circles the scene origin at a distance; only forward/back movement (zoom) is used.
class OrbitalController final : public EulerCamera
{
public:
  Matrix44 GetView() const override
  {
    return Matrix44::Translate(Vec3{0, 0, -m_distance}) * Matrix44::FromQuaternion(m_orientation);
  }

  void MoveVertical(float) override {}
  void MoveHorizontal(float) override {}
  void MoveForward(float amt) override { m_distance = std::max(m_distance - amt, CLOSEST); }

  void Reset() override
  {
    CameraControllerInput::Reset();
    ResetOrientation();
    m_distance = CLOSEST;
  }

  void DoState(PointerWrap& p) override
  {
    CameraControllerInput::DoState(p);
    DoOrientationState(p);
    p.Do(m_distance);
  }

private:
  static constexpr float CLOSEST = 0.0f;
  float m_distance = CLOSEST;
};
}  // namespace

Common::Vec2 CameraControllerInput::GetFieldOfViewMultiplier() const
{
  return Common::Vec2{m_fov_x, m_fov_y};
}

void CameraControllerInput::DoState(PointerWrap& p)
{
  p.Do(m_speed);
  p.Do(m_fov_x);
  p.Do(m_fov_y);
}

void CameraControllerInput::IncreaseFovX(float fov)
{
  m_fov_x = std::max(m_fov_x + fov, SMALLEST_FOV);
}

void CameraControllerInput::IncreaseFovY(float fov)
{
  m_fov_y = std::max(m_fov_y + fov, SMALLEST_FOV);
}

float CameraControllerInput::GetFovStepSize() const
{
  return 1.5f;
}

void CameraControllerInput::Reset()
{
  m_fov_x = NORMAL_FOV;
  m_fov_y = NORMAL_FOV;
  m_dirty = true;
}

void CameraControllerInput::ModifySpeed(float amt)
{
  m_speed = std::max(m_speed + amt, 0.0f);
}

void CameraControllerInput::ResetSpeed()
{
  m_speed = NORMAL_SPEED;
}

float CameraControllerInput::GetSpeed() const
{
  return m_speed;
}

FreeLookCamera::FreeLookCamera()
{
  RefreshConfig();
}

void FreeLookCamera::RefreshConfig()
{
  m_enabled = Config::Get(Config::FREE_LOOK_ENABLED);

  const FreeLook::ControlType wanted = Config::Get(Config::FL1_CONTROL_TYPE);
  if (m_type == wanted)
    return;

  switch (wanted)
  {
  case FreeLook::ControlType::Orbital:
    m_controller = std::make_unique<OrbitalController>();
    break;
  case FreeLook::ControlType::FPS:
    m_controller = std::make_unique<FPSController>();
    break;
  default:
    m_controller = std::make_unique<SixAxisController>();
    break;
  }
  m_type = wanted;
}

Common::Matrix44 FreeLookCamera::GetView() const
{
  return m_controller->GetView();
}

Common::Vec2 FreeLookCamera::GetFieldOfViewMultiplier() const
{
  return m_controller->GetFieldOfViewMultiplier();
}

void FreeLookCamera::DoState(PointerWrap& p)
{
  if (p.IsWriteMode() || p.IsMeasureMode())
  {
    p.Do(m_type);
    if (m_controller != nullptr)
      m_controller->DoState(p);
    return;
  }

  // Loading (or verifying): the controller state only fits a controller of the same type.
  const std::optional<FreeLook::ControlType> configured = m_type;
  p.Do(m_type);
  if (configured == m_type)
  {
    m_controller->DoState(p);
    return;
  }
  if (!p.IsReadMode())
    return;

  const std::string configured_name = configured ? ControlTypeName(*configured) : "";
  const std::string loaded_name = m_type ? ControlTypeName(*m_type) : "";
  Core::DisplayMessage(fmt::format("State needs same free look camera type. Settings value '{}', "
                                   "loaded value '{}'.  Aborting load state",
                                   configured_name, loaded_name),
                       5000);
  p.SetVerifyMode();
}

bool FreeLookCamera::IsActive() const
{
  return m_enabled;
}

CameraController* FreeLookCamera::GetController() const
{
  return m_controller.get();
}
