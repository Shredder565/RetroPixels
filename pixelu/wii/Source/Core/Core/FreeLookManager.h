// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <chrono>
#include <optional>

#include "InputCommon/ControllerEmu/ControllerEmu.h"

class CameraControllerInput;
class InputConfig;

namespace ControllerEmu
{
class ControlGroup;
class Buttons;
class IMUGyroscope;
}  // namespace ControllerEmu

// Mapping groups of the free look controller, for the mapping UI.
enum class FreeLookGroup
{
  Move,
  Speed,
  FieldOfView,
  Other,
  Rotation,
};

// The free look camera's input: one emulated controller ("FreeLook1",
// saved in FreeLookController.ini) whose buttons and gyro drive the camera.
namespace FreeLook
{
void Shutdown();
void Initialize();
void LoadInputConfig();
bool IsInitialized();
void UpdateInput();

InputConfig* GetInputConfig();
ControllerEmu::ControlGroup* GetInputGroup(int pad_num, FreeLookGroup group);
}  // namespace FreeLook

class FreeLookController final : public ControllerEmu::EmulatedController
{
public:
  explicit FreeLookController(unsigned int index);

  std::string GetName() const override;
  InputConfig* GetConfig() const override;
  void LoadDefaults(const ControllerInterface& ciface) override;

  ControllerEmu::ControlGroup* GetGroup(FreeLookGroup group) const;

  // Applies this frame's input to the camera, if free look is on and the
  // active camera controller takes input.
  void Update();

private:
  void UpdateInput(CameraControllerInput* camera_controller);

  ControllerEmu::Buttons* m_move_buttons;
  ControllerEmu::Buttons* m_speed_buttons;
  ControllerEmu::Buttons* m_fov_buttons;
  ControllerEmu::Buttons* m_other_buttons;
  ControllerEmu::IMUGyroscope* m_rotation_gyro;

  const unsigned int m_index;
  // When input was last applied; the camera moves by rate times elapsed time.
  std::optional<std::chrono::steady_clock::time_point> m_last_update;
};
