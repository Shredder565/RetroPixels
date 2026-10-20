// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <optional>
#include <string>

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Core/HW/WiimoteEmu/Encryption.h"
#include "Core/HW/WiimoteEmu/I2CBus.h"
#include "InputCommon/ControllerEmu/ControlGroup/Attachments.h"

namespace WiimoteEmu
{
struct DesiredExtensionState;

// Something that can be plugged into an extension port: a set of input controls and an i2c
// device.
class Extension : public ControllerEmu::AttachedController, public I2CSlave
{
public:
  explicit Extension(const char* name);
  Extension(const char* config_name, const char* display_name);

  std::string GetName() const override;
  std::string GetDisplayName() const override;

  // The port's device-detect line. Ordinary extensions tie it high; the MotionPlus drives it
  // while it switches modes.
  virtual bool ReadDeviceDetectPin() const = 0;

  virtual void Reset() = 0;
  virtual void DoState(PointerWrap& p) = 0;

  // Reads the user's input into target_state (called while preparing input).
  virtual void BuildDesiredExtensionState(DesiredExtensionState* target_state) = 0;
  // Puts that input into the extension's registers.
  virtual void Update(const DesiredExtensionState& target_state) = 0;

private:
  const char* const m_config_name;
  const char* const m_display_name;
};

// The empty port.
class None : public Extension
{
public:
  explicit None();

private:
  bool ReadDeviceDetectPin() const override;
  void BuildDesiredExtensionState(DesiredExtensionState* target_state) override;
  void Update(const DesiredExtensionState& target_state) override;
  void Reset() override;
  void DoState(PointerWrap& p) override;

  int BusRead(u8 slave_addr, u8 addr, int count, u8* data_out) override;
  int BusWrite(u8 slave_addr, u8 addr, int count, const u8* data_in) override;
};

// Register file, identification and read scrambling shared by the ordinary extensions.
class EncryptedExtension : public Extension
{
public:
  static constexpr u8 I2C_ADDR = 0x52;
  static constexpr int CONTROLLER_DATA_BYTES = 21;
  static constexpr int CALIBRATION_CHECKSUM_BYTES = 2;

#pragma pack(push, 1)
  struct Register
  {
    // 0x00: input data.
    u8 controller_data[CONTROLLER_DATA_BYTES];
    u8 unknown2[11];

    // 0x20: calibration (two copies of 16 bytes on most extensions).
    std::array<u8, 0x10> calibration;
    u8 unknown3[0x10];

    // 0x40: key material written by the Wii.
    std::array<u8, 0x10> encryption_key_data;
    u8 unknown4[0xA0];

    // 0xf0: 0xaa turns scrambling on.
    u8 encryption;
    u8 unknown5[0x9];

    // 0xfa: extension identifier.
    std::array<u8, 6> identifier;
  };
#pragma pack(pop)

  static_assert(0x100 == sizeof(Register));

protected:
  EncryptedExtension(ExtensionKeyScheme scheme, const char* config_name,
                     const char* display_name);

  void Reset() override;
  void DoState(PointerWrap& p) override;

  Register m_reg = {};

private:
  bool ReadDeviceDetectPin() const override;

  int BusRead(u8 slave_addr, u8 addr, int count, u8* data_out) override;
  int BusWrite(u8 slave_addr, u8 addr, int count, const u8* data_in) override;

  const ExtensionKeyScheme m_key_scheme;

  // Derived from m_reg.encryption_key_data when first needed after a change.
  std::optional<EncryptionKey> m_key;
};

// Extensions using the first-party key constants (Nintendo's, and most licensed ones).
class Extension1stParty : public EncryptedExtension
{
protected:
  explicit Extension1stParty(const char* name);
  Extension1stParty(const char* config_name, const char* display_name);
};

// Extensions using the third-party key constants (uDraw, Drawsome, TaTaCon, Shinkansen).
class Extension3rdParty : public EncryptedExtension
{
protected:
  explicit Extension3rdParty(const char* name);
  Extension3rdParty(const char* config_name, const char* display_name);
};

}  // namespace WiimoteEmu
