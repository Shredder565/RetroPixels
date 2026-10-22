// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <thread>

#include <windows.h>

#include "Common/Event.h"
#include "Common/Flag.h"

#include "InputCommon/ControllerInterface/CoreDevice.h"
#include "InputCommon/ControllerInterface/DInput/DInput8.h"

namespace ciface::ForceFeedback
{
// A device with DirectInput force feedback: one output per supported effect type (constant,
// ramp and the periodic waveforms), each played at the requested strength. Effects are
// updated on a thread of their own, as DirectInput calls can be slow.
class ForceFeedbackDevice : public Core::Device
{
public:
  // Creates the effects the device supports and adds them as outputs. Returns false when
  // the device has no force feedback axes.
  bool InitForceFeedback(const LPDIRECTINPUTDEVICE8, int axis_count);
  // Stops the effect thread, which releases the effects. Must come before the DirectInput
  // device is released.
  void DeInitForceFeedback();

private:
  class Effect;
  template <typename Params>
  class TypedEffect;

  void EffectThread();

  std::thread m_effect_thread;
  // Set when an output's strength changes (or to stop the thread).
  Common::Event m_effects_changed;
  Common::Flag m_thread_running;
};
}  // namespace ciface::ForceFeedback
