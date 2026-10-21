// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "InputCommon/ControllerInterface/ForceFeedback/ForceFeedbackDevice.h"

#include <array>
#include <atomic>
#include <string>

#include "Common/Thread.h"

namespace ciface::ForceFeedback
{
namespace
{
// Applies a strength to an effect's type-specific parameters; returns whether they changed.
bool SetStrength(DICONSTANTFORCE* params, int strength)
{
  const LONG before = params->lMagnitude;
  params->lMagnitude = strength;
  return params->lMagnitude != before;
}

// (A ramp from a strength to the same strength: ramps mean little for rumble.)
bool SetStrength(DIRAMPFORCE* params, int strength)
{
  const LONG before = params->lStart;
  params->lStart = strength;
  params->lEnd = strength;
  return params->lStart != before;
}

bool SetStrength(DIPERIODIC* params, int strength)
{
  const DWORD before = params->dwMagnitude;
  params->dwMagnitude = static_cast<DWORD>(strength);
  return params->dwMagnitude != before;
}

// The effect types tried, in output order.
enum class Family
{
  Constant,
  Ramp,
  Periodic,
};

struct EffectType
{
  const GUID* guid;
  const char* name;
  Family family;
};

const std::array<EffectType, 7> EFFECT_TYPES{{
    {&GUID_ConstantForce, "Constant", Family::Constant},
    {&GUID_RampForce, "Ramp", Family::Ramp},
    {&GUID_Square, "Square", Family::Periodic},
    {&GUID_Sine, "Sine", Family::Periodic},
    {&GUID_Triangle, "Triangle", Family::Periodic},
    {&GUID_SawtoothUp, "Sawtooth Up", Family::Periodic},
    {&GUID_SawtoothDown, "Sawtooth Down", Family::Periodic},
}};
}  // namespace

// An output driving one DirectInput effect. SetState only records the wanted strength; the
// effect thread applies it.
class ForceFeedbackDevice::Effect : public Core::Device::Output
{
public:
  Effect(ForceFeedbackDevice* device, const char* name, LPDIRECTINPUTEFFECT effect)
      : m_effect(effect), m_device(device), m_name(name)
  {
  }

  std::string GetName() const override { return m_name; }

  void SetState(ControlState state) override
  {
    const int strength = int(DI_FFNOMINALMAX * state);
    if (m_wanted.exchange(strength) != strength)
      m_device->m_effects_changed.Set();
  }

  // On the effect thread.
  void Apply() { ApplyStrength(m_wanted); }

  // On the effect thread, before the device goes.
  void Free()
  {
    m_effect->Stop();
    m_effect->Unload();
    m_effect->Release();
  }

protected:
  const LPDIRECTINPUTEFFECT m_effect;

private:
  virtual void ApplyStrength(int strength) = 0;

  ForceFeedbackDevice* const m_device;
  const char* const m_name;
  std::atomic<int> m_wanted = 0;
};

template <typename Params>
class ForceFeedbackDevice::TypedEffect final : public ForceFeedbackDevice::Effect
{
public:
  TypedEffect(ForceFeedbackDevice* device, const char* name, LPDIRECTINPUTEFFECT effect,
              const Params& params)
      : Effect(device, name, effect), m_params(params)
  {
  }

private:
  void ApplyStrength(int strength) override
  {
    if (!SetStrength(&m_params, strength))
      return;

    if (strength == 0)
    {
      m_effect->Stop();
      return;
    }

    // Starting the effect with its new parameters (it runs for RUMBLE_LENGTH_MS).
    DIEFFECT update{};
    update.dwSize = sizeof(update);
    update.cbTypeSpecificParams = sizeof(m_params);
    update.lpvTypeSpecificParams = &m_params;
    m_effect->SetParameters(&update, DIEP_START | DIEP_TYPESPECIFICPARAMS);
  }

  Params m_params;
};

bool ForceFeedbackDevice::InitForceFeedback(const LPDIRECTINPUTDEVICE8 device, int axis_count)
{
  if (axis_count == 0)
    return false;

  // Effects are created along the X axis (what a wheel turns on); gamepads accept any axis.
  // DirectInput only reads these while creating the effects.
  DWORD axes[] = {DIJOFS_X};
  LONG directions[] = {-200};

  DIEFFECT effect{};
  effect.dwSize = sizeof(effect);
  effect.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
  effect.dwDuration = DI_SECONDS / 1000 * RUMBLE_LENGTH_MS;
  effect.dwSamplePeriod = 0;
  effect.dwGain = DI_FFNOMINALMAX;
  effect.dwTriggerButton = DIEB_NOTRIGGER;
  effect.dwTriggerRepeatInterval = 0;
  effect.cAxes = DWORD(std::size(axes));
  effect.rgdwAxes = axes;
  effect.rglDirection = directions;
  effect.dwStartDelay = 0;

  // Every effect starts at zero strength, as it is now.
  DICONSTANTFORCE constant{};
  DIRAMPFORCE ramp{};
  DIPERIODIC periodic{};
  periodic.dwPeriod = DI_SECONDS / 1000 * RUMBLE_PERIOD_MS;

  for (const EffectType& type : EFFECT_TYPES)
  {
    switch (type.family)
    {
    case Family::Constant:
      effect.cbTypeSpecificParams = sizeof(constant);
      effect.lpvTypeSpecificParams = &constant;
      break;
    case Family::Ramp:
      effect.cbTypeSpecificParams = sizeof(ramp);
      effect.lpvTypeSpecificParams = &ramp;
      break;
    case Family::Periodic:
      effect.cbTypeSpecificParams = sizeof(periodic);
      effect.lpvTypeSpecificParams = &periodic;
      break;
    }

    LPDIRECTINPUTEFFECT created;
    if (FAILED(device->CreateEffect(*type.guid, &effect, &created, nullptr)))
      continue;

    switch (type.family)
    {
    case Family::Constant:
      AddOutput(new TypedEffect<DICONSTANTFORCE>(this, type.name, created, constant));
      break;
    case Family::Ramp:
      AddOutput(new TypedEffect<DIRAMPFORCE>(this, type.name, created, ramp));
      break;
    case Family::Periodic:
      AddOutput(new TypedEffect<DIPERIODIC>(this, type.name, created, periodic));
      break;
    }
  }

  if (!Outputs().empty())
  {
    // Turn off the wheel's self-centring spring.
    DIPROPDWORD autocenter;
    autocenter.diph.dwSize = sizeof(DIPROPDWORD);
    autocenter.diph.dwHeaderSize = sizeof(DIPROPHEADER);
    autocenter.diph.dwObj = 0;
    autocenter.diph.dwHow = DIPH_DEVICE;
    autocenter.dwData = DIPROPAUTOCENTER_OFF;
    device->SetProperty(DIPROP_AUTOCENTER, &autocenter.diph);

    m_thread_running.Set();
    m_effect_thread = std::thread(&ForceFeedbackDevice::EffectThread, this);
  }

  return true;
}

void ForceFeedbackDevice::DeInitForceFeedback()
{
  if (!m_thread_running.TestAndClear())
    return;

  m_effects_changed.Set();
  m_effect_thread.join();
}

void ForceFeedbackDevice::EffectThread()
{
  Common::SetCurrentThreadName("ForceFeedback update thread");

  // All outputs of this device are effects.
  while (m_thread_running.IsSet())
  {
    m_effects_changed.Wait();
    for (Output* output : Outputs())
      static_cast<Effect*>(output)->Apply();
  }

  for (Output* output : Outputs())
    static_cast<Effect*>(output)->Free();
}
}  // namespace ciface::ForceFeedback
