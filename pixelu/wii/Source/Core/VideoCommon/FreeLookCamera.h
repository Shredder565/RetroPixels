// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <memory>
#include <optional>

#include "Common/Matrix.h"
#include "Core/Config/FreeLookSettings.h"

class PointerWrap;

// Something that produces the free-look view matrix and field-of-view scale.
class CameraController
{
public:
  CameraController() = default;
  virtual ~CameraController() = default;

  CameraController(const CameraController&) = delete;
  CameraController& operator=(const CameraController&) = delete;
  CameraController(CameraController&&) = delete;
  CameraController& operator=(CameraController&&) = delete;

  virtual Common::Matrix44 GetView() const = 0;
  virtual Common::Vec2 GetFieldOfViewMultiplier() const = 0;

  virtual void DoState(PointerWrap& p) = 0;

  virtual bool IsDirty() const = 0;
  virtual void SetClean() = 0;

  virtual bool SupportsInput() const = 0;
};

// A controller driven by the free-look hotkeys/controller: movement, rotation, FOV and speed.
class CameraControllerInput : public CameraController
{
public:
  Common::Vec2 GetFieldOfViewMultiplier() const final;

  void DoState(PointerWrap& p) override;

  bool IsDirty() const final { return m_dirty; }
  void SetClean() final { m_dirty = false; }

  bool SupportsInput() const final { return true; }

  virtual void MoveVertical(float amt) = 0;
  virtual void MoveHorizontal(float amt) = 0;
  virtual void MoveForward(float amt) = 0;

  virtual void Rotate(const Common::Vec3& amt) = 0;
  virtual void Rotate(const Common::Quaternion& quat) = 0;

  // Puts the FOV back to normal and marks the camera dirty; overrides also reset the pose.
  virtual void Reset() = 0;

  void IncreaseFovX(float fov);
  void IncreaseFovY(float fov);
  float GetFovStepSize() const;

  void ModifySpeed(float multiplier);
  void ResetSpeed();
  float GetSpeed() const;

private:
  float m_fov_x = 1.0f;
  float m_fov_y = 1.0f;
  float m_speed = 60.0f;
  bool m_dirty = false;
};

// The free-look camera in use, rebuilt when the configured control type changes.
class FreeLookCamera
{
public:
  FreeLookCamera();

  void RefreshConfig();

  Common::Matrix44 GetView() const;
  Common::Vec2 GetFieldOfViewMultiplier() const;

  void DoState(PointerWrap& p);

  bool IsActive() const;

  CameraController* GetController() const;

private:
  std::unique_ptr<CameraController> m_controller;
  bool m_enabled = false;
  std::optional<FreeLook::ControlType> m_type;
};

extern FreeLookCamera g_freelook_camera;
