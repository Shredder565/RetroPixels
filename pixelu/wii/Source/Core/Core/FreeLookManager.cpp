// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/FreeLookManager.h"

#include <array>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include "Common/Common.h"
#include "Common/Config/Config.h"
#include "Common/ScopeGuard.h"

#include "Core/Config/FreeLookSettings.h"
#include "Core/Core.h"

#include "InputCommon/ControlReference/ControlReference.h"
#include "InputCommon/ControllerEmu/ControlGroup/Buttons.h"
#include "InputCommon/ControllerEmu/ControlGroup/IMUGyroscope.h"
#include "InputCommon/InputConfig.h"

#include "VideoCommon/FreeLookCamera.h"

namespace
{
// Control indices inside each group, in the order the controls are added.
enum MoveControl : int
{
  MOVE_UP,
  MOVE_DOWN,
  MOVE_LEFT,
  MOVE_RIGHT,
  MOVE_FORWARD,
  MOVE_BACKWARD,
};

enum SpeedControl : int
{
  SPEED_DECREASE,
  SPEED_INCREASE,
  SPEED_RESET,
};

enum OtherControl : int
{
  OTHER_RESET_VIEW,
};

enum FovControl : int
{
  FOV_INCREASE_X,
  FOV_DECREASE_X,
  FOV_INCREASE_Y,
  FOV_DECREASE_Y,
};

enum GyroControl : int
{
  GYRO_PITCH_UP,
  GYRO_PITCH_DOWN,
  GYRO_ROLL_LEFT,
  GYRO_ROLL_RIGHT,
  GYRO_YAW_LEFT,
  GYRO_YAW_RIGHT,
};

// "@(Shift+E)": all keys held together.
std::string Chord(const std::vector<std::string>& keys)
{
  return fmt::format("@({})", fmt::join(keys, "+"));
}

// The mouse moves the camera while a button is held, scaled down to a sensible rate.
std::string MouseDrag(const char* button, const char* axis)
{
  return fmt::format("if(`{}`,`RelativeMouse {}` * 0.10, 0)", button, axis);
}

bool IsHeld(const ControllerEmu::Buttons* group, int control)
{
  return group->controls[static_cast<size_t>(control)]->GetState<bool>();
}

InputConfig s_input_config("FreeLookController", _trans("FreeLook"), "FreeLookController",
                           "FreeLookController");
}  // namespace

FreeLookController::FreeLookController(const unsigned int index) : m_index(index)
{
  using ControllerEmu::Translatability;

  // The group order is also the order of the INI sections.
  m_move_buttons = new ControllerEmu::Buttons(_trans("Move"));
  groups.emplace_back(m_move_buttons);
  for (const char* name : {_trans("Up"), _trans("Down"), _trans("Left"), _trans("Right"),
                           _trans("Forward"), _trans("Backward")})
  {
    m_move_buttons->AddInput(Translatability::Translate, name);
  }

  m_speed_buttons = new ControllerEmu::Buttons(_trans("Speed"));
  groups.emplace_back(m_speed_buttons);
  for (const char* name : {_trans("Decrease"), _trans("Increase"), _trans("Reset")})
    m_speed_buttons->AddInput(Translatability::Translate, name);

  m_other_buttons = new ControllerEmu::Buttons(_trans("Other"));
  groups.emplace_back(m_other_buttons);
  m_other_buttons->AddInput(Translatability::Translate, _trans("Reset View"));

  m_fov_buttons = new ControllerEmu::Buttons(_trans("Field of View"));
  groups.emplace_back(m_fov_buttons);
  for (const char* name :
       {_trans("Increase X"), _trans("Decrease X"), _trans("Increase Y"), _trans("Decrease Y")})
  {
    m_fov_buttons->AddInput(Translatability::Translate, name);
  }

  m_rotation_gyro = new ControllerEmu::IMUGyroscope(_trans("Incremental Rotation"),
                                                    _trans("Incremental Rotation"));
  groups.emplace_back(m_rotation_gyro);
}

std::string FreeLookController::GetName() const
{
  return "FreeLook" + std::string(1, static_cast<char>('1' + m_index));
}

InputConfig* FreeLookController::GetConfig() const
{
  return FreeLook::GetInputConfig();
}

void FreeLookController::LoadDefaults(const ControllerInterface& ciface)
{
  EmulatedController::LoadDefaults(ciface);

  // Shift plus WASD/QE moves, Shift+1/2/F changes or resets the speed, Shift+R
  // resets the view, Shift with the mouse wheel changes the field of view.
  struct KeyDefault
  {
    ControllerEmu::Buttons* group;
    int control;
    const char* key;
  };
  const KeyDefault key_defaults[] = {
      {m_move_buttons, MOVE_UP, "E"},
      {m_move_buttons, MOVE_DOWN, "Q"},
      {m_move_buttons, MOVE_LEFT, "A"},
      {m_move_buttons, MOVE_RIGHT, "D"},
      {m_move_buttons, MOVE_FORWARD, "W"},
      {m_move_buttons, MOVE_BACKWARD, "S"},
      {m_speed_buttons, SPEED_DECREASE, "`1`"},
      {m_speed_buttons, SPEED_INCREASE, "`2`"},
      {m_speed_buttons, SPEED_RESET, "F"},
      {m_other_buttons, OTHER_RESET_VIEW, "R"},
      {m_fov_buttons, FOV_INCREASE_X, "`Axis Z+`"},
      {m_fov_buttons, FOV_DECREASE_X, "`Axis Z-`"},
      {m_fov_buttons, FOV_INCREASE_Y, "`Axis Z+`"},
      {m_fov_buttons, FOV_DECREASE_Y, "`Axis Z-`"},
  };
  for (const KeyDefault& entry : key_defaults)
    entry.group->SetControlExpression(entry.control, Chord({"Shift", entry.key}));

  // Dragging with the left button pitches and yaws, the middle button rolls.
  // The X11 backend numbers mouse buttons differently.
#if defined HAVE_X11 && HAVE_X11
  const char* const look_button = "Click 3";
#else
  const char* const look_button = "Click 1";
#endif
  const char* const roll_button = "Click 2";

  m_rotation_gyro->SetControlExpression(GYRO_PITCH_UP, MouseDrag(look_button, "Y-"));
  m_rotation_gyro->SetControlExpression(GYRO_PITCH_DOWN, MouseDrag(look_button, "Y+"));
  m_rotation_gyro->SetControlExpression(GYRO_ROLL_LEFT, MouseDrag(roll_button, "X-"));
  m_rotation_gyro->SetControlExpression(GYRO_ROLL_RIGHT, MouseDrag(roll_button, "X+"));
  m_rotation_gyro->SetControlExpression(GYRO_YAW_LEFT, MouseDrag(look_button, "X-"));
  m_rotation_gyro->SetControlExpression(GYRO_YAW_RIGHT, MouseDrag(look_button, "X+"));
}

ControllerEmu::ControlGroup* FreeLookController::GetGroup(FreeLookGroup group) const
{
  // Indexed by FreeLookGroup's enum order.
  const std::array<ControllerEmu::ControlGroup*, 5> by_group{
      m_move_buttons, m_speed_buttons, m_fov_buttons, m_other_buttons, m_rotation_gyro};
  const auto index = static_cast<size_t>(group);
  return index < by_group.size() ? by_group[index] : nullptr;
}

void FreeLookController::Update()
{
  if (!g_freelook_camera.IsActive())
    return;

  auto* const controller = g_freelook_camera.GetController();
  if (controller->SupportsInput())
    UpdateInput(static_cast<CameraControllerInput*>(controller));
}

void FreeLookController::UpdateInput(CameraControllerInput* camera)
{
  const auto lock = GetStateLock();

  // Free look has its own background-input setting: switch the input gate to
  // it for this update and put the previous gate back afterwards.
  const bool previous_gate = ControlReference::GetInputGate();
  Common::ScopeGuard restore_gate{[previous_gate] { ControlReference::SetInputGate(previous_gate); }};
  Core::UpdateInputGate(!Config::Get(Config::FREE_LOOK_BACKGROUND_INPUT));

  // Seconds since the previous update; the first update counts as one second.
  using FloatSeconds = std::chrono::duration<float>;
  float elapsed = 1.0;
  if (m_last_update)
  {
    const auto since_last = std::chrono::steady_clock::now() - *m_last_update;
    elapsed = std::chrono::duration_cast<FloatSeconds>(since_last).count();
  }
  m_last_update = std::chrono::steady_clock::now();

  // The gyro is evaluated a second time for its value, as it always has been
  // (evaluation updates its state).
  const Common::Vec3 rate =
      m_rotation_gyro->GetState() ? *m_rotation_gyro->GetState() : Common::Vec3{};
  // Gyro axes to camera axes: yaw and roll swap places and change sign.
  const Common::Vec3 camera_rate{rate.x, rate.z * -1, rate.y * -1};
  camera->Rotate(Common::Quaternion::RotateXYZ(camera_rate * elapsed));

  // Translation: (control, axis, direction). Each move reads the current speed.
  using MoveFn = void (CameraControllerInput::*)(float);
  struct Motion
  {
    int control;
    MoveFn move;
    float direction;
  };
  static constexpr Motion motions[] = {
      {MOVE_UP, &CameraControllerInput::MoveVertical, -1.0f},
      {MOVE_DOWN, &CameraControllerInput::MoveVertical, 1.0f},
      {MOVE_LEFT, &CameraControllerInput::MoveHorizontal, 1.0f},
      {MOVE_RIGHT, &CameraControllerInput::MoveHorizontal, -1.0f},
      {MOVE_FORWARD, &CameraControllerInput::MoveForward, 1.0f},
      {MOVE_BACKWARD, &CameraControllerInput::MoveForward, -1.0f},
  };
  for (const Motion& motion : motions)
  {
    if (IsHeld(m_move_buttons, motion.control))
      (camera->*motion.move)(motion.direction * camera->GetSpeed() * elapsed);
  }

  struct Zoom
  {
    int control;
    void (CameraControllerInput::*change)(float);
    float direction;
  };
  static constexpr Zoom zooms[] = {
      {FOV_INCREASE_X, &CameraControllerInput::IncreaseFovX, 1.0f},
      {FOV_DECREASE_X, &CameraControllerInput::IncreaseFovX, -1.0f},
      {FOV_INCREASE_Y, &CameraControllerInput::IncreaseFovY, 1.0f},
      {FOV_DECREASE_Y, &CameraControllerInput::IncreaseFovY, -1.0f},
  };
  for (const Zoom& zoom : zooms)
  {
    if (IsHeld(m_fov_buttons, zoom.control))
      (camera->*zoom.change)(zoom.direction * camera->GetFovStepSize() * elapsed);
  }

  // Speed changes are computed in double precision.
  if (IsHeld(m_speed_buttons, SPEED_DECREASE))
    camera->ModifySpeed(static_cast<float>(camera->GetSpeed() * -0.9 * elapsed));
  if (IsHeld(m_speed_buttons, SPEED_INCREASE))
    camera->ModifySpeed(static_cast<float>(camera->GetSpeed() * 1.1 * elapsed));
  if (IsHeld(m_speed_buttons, SPEED_RESET))
    camera->ResetSpeed();

  if (IsHeld(m_other_buttons, OTHER_RESET_VIEW))
    camera->Reset();
}

namespace FreeLook
{
InputConfig* GetInputConfig()
{
  return &s_input_config;
}

void Shutdown()
{
  s_input_config.UnregisterHotplugCallback();
  s_input_config.ClearControllers();
}

void Initialize()
{
  if (s_input_config.ControllersNeedToBeCreated())
    s_input_config.CreateController<FreeLookController>(0);

  s_input_config.RegisterHotplugCallback();
  g_freelook_camera.RefreshConfig();
  s_input_config.LoadConfig();
}

void LoadInputConfig()
{
  s_input_config.LoadConfig();
}

bool IsInitialized()
{
  return !s_input_config.ControllersNeedToBeCreated();
}

ControllerEmu::ControlGroup* GetInputGroup(int pad_num, FreeLookGroup group)
{
  auto* const controller = static_cast<FreeLookController*>(s_input_config.GetController(pad_num));
  return controller->GetGroup(group);
}

void UpdateInput()
{
  for (int i = 0; i < s_input_config.GetControllerCount(); ++i)
    static_cast<FreeLookController*>(s_input_config.GetController(i))->Update();
}
}  // namespace FreeLook
