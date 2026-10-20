// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <type_traits>
#include <variant>

#include "Common/BitUtils.h"

#include "Core/HW/WiimoteEmu/Extension/Classic.h"
#include "Core/HW/WiimoteEmu/Extension/DrawsomeTablet.h"
#include "Core/HW/WiimoteEmu/Extension/Drums.h"
#include "Core/HW/WiimoteEmu/Extension/Extension.h"
#include "Core/HW/WiimoteEmu/Extension/Guitar.h"
#include "Core/HW/WiimoteEmu/Extension/Nunchuk.h"
#include "Core/HW/WiimoteEmu/Extension/Shinkansen.h"
#include "Core/HW/WiimoteEmu/Extension/TaTaCon.h"
#include "Core/HW/WiimoteEmu/Extension/Turntable.h"
#include "Core/HW/WiimoteEmu/Extension/UDrawTablet.h"
#include "Core/HW/WiimoteEmu/ExtensionPort.h"

namespace WiimoteEmu
{
// The input of whichever extension is selected, as gathered for one update.
struct DesiredExtensionState
{
  // The alternative index is the ExtensionNumber; movies and netplay store it.
  using ExtensionData =
      std::variant<std::monostate, Nunchuk::DesiredState, Classic::DesiredState,
                   Guitar::DesiredState, Drums::DesiredState, Turntable::DesiredState,
                   UDrawTablet::DesiredState, DrawsomeTablet::DesiredState,
                   TaTaCon::DesiredState, Shinkansen::DesiredState>;

  ExtensionData data = std::monostate{};

private:
  template <ExtensionNumber N, typename T>
  static constexpr bool IsAt = std::is_same_v<std::variant_alternative_t<N, ExtensionData>, T>;

  static_assert(std::variant_size_v<ExtensionData> == ExtensionNumber::MAX);
  static_assert(IsAt<ExtensionNumber::NUNCHUK, Nunchuk::DesiredState>);
  static_assert(IsAt<ExtensionNumber::CLASSIC, Classic::DesiredState>);
  static_assert(IsAt<ExtensionNumber::GUITAR, Guitar::DesiredState>);
  static_assert(IsAt<ExtensionNumber::DRUMS, Drums::DesiredState>);
  static_assert(IsAt<ExtensionNumber::TURNTABLE, Turntable::DesiredState>);
  static_assert(IsAt<ExtensionNumber::UDRAW_TABLET, UDrawTablet::DesiredState>);
  static_assert(IsAt<ExtensionNumber::DRAWSOME_TABLET, DrawsomeTablet::DesiredState>);
  static_assert(IsAt<ExtensionNumber::TATACON, TaTaCon::DesiredState>);
  static_assert(IsAt<ExtensionNumber::SHINKANSEN, Shinkansen::DesiredState>);
};

// Copies the extension's desired input to the start of its register file, or zeros when the
// state holds a different extension's input.
template <typename T>
void DefaultExtensionUpdate(EncryptedExtension::Register* reg,
                            const DesiredExtensionState& target_state)
{
  const T* desired = std::get_if<T>(&target_state.data);
  Common::BitCastPtr<T>(&reg->controller_data) = desired ? *desired : T{};
}
}  // namespace WiimoteEmu
