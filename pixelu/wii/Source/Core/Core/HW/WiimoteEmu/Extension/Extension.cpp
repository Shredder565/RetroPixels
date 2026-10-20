// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/WiimoteEmu/Extension/Extension.h"

#include <cstddef>

#include "Common/CommonTypes.h"

#include "Core/HW/Wiimote.h"
#include "Core/HW/WiimoteEmu/Extension/DesiredExtensionState.h"
#include "Core/HW/WiimoteEmu/WiimoteEmu.h"

namespace WiimoteEmu
{
Extension::Extension(const char* name) : Extension(name, name)
{
}

// For extensions whose UI name differs from their config section name.
Extension::Extension(const char* config_name, const char* display_name)
    : m_config_name(config_name), m_display_name(display_name)
{
}

// The config section name; the display name is what the UI shows.
std::string Extension::GetName() const
{
  return m_config_name;
}

std::string Extension::GetDisplayName() const
{
  return m_display_name;
}

// ---- None

None::None() : Extension("None")
{
}

bool None::ReadDeviceDetectPin() const
{
  return false;
}

void None::BuildDesiredExtensionState(DesiredExtensionState* target_state)
{
  target_state->data.emplace<std::monostate>();
}

void None::Update(const DesiredExtensionState&)
{
}

void None::Reset()
{
}

void None::DoState(PointerWrap&)
{
}

int None::BusRead(u8, u8, int, u8*)
{
  return 0;
}

int None::BusWrite(u8, u8, int, const u8*)
{
  return 0;
}

// ---- EncryptedExtension

namespace
{
constexpr u8 SCRAMBLE_ON = 0xaa;
constexpr int KEY_FIRST = offsetof(EncryptedExtension::Register, encryption_key_data);
constexpr int KEY_END = KEY_FIRST + 0x10;
}  // namespace

EncryptedExtension::EncryptedExtension(ExtensionKeyScheme scheme, const char* config_name,
                                       const char* display_name)
    : Extension(config_name, display_name), m_key_scheme(scheme)
{
}

bool EncryptedExtension::ReadDeviceDetectPin() const
{
  return true;
}

int EncryptedExtension::BusRead(u8 slave_addr, u8 addr, int count, u8* data_out)
{
  if (slave_addr != I2C_ADDR)
    return 0;

  const int transferred = RawRead(&m_reg, addr, count, data_out);

  if (m_reg.encryption == SCRAMBLE_ON)
  {
    if (!m_key.has_value())
      m_key = GenerateExtensionKey(m_key_scheme, m_reg.encryption_key_data);

    // The whole requested length is scrambled, even past the end of the register file.
    m_key->Encrypt(data_out, addr, static_cast<u32>(count));
  }

  return transferred;
}

int EncryptedExtension::BusWrite(u8 slave_addr, u8 addr, int count, const u8* data_in)
{
  if (slave_addr != I2C_ADDR)
    return 0;

  const int transferred = RawWrite(&m_reg, addr, count, data_in);

  // Any write that touches the key area invalidates the derived key. (Real extensions want the
  // key in particular chunk sizes; accepting any write is good enough.)
  const int write_end = addr + count;
  if (write_end > KEY_FIRST && addr < KEY_END)
    m_key.reset();

  return transferred;
}

void EncryptedExtension::Reset()
{
  m_reg = {};
  m_key.reset();
}

void EncryptedExtension::DoState(PointerWrap& p)
{
  p.Do(m_reg);

  // The key is not saved; it is derived again from the loaded registers.
  if (p.IsReadMode())
    m_key.reset();
}

// ---- Key schemes

Extension1stParty::Extension1stParty(const char* name)
    : EncryptedExtension(ExtensionKeyScheme::FirstParty, name, name)
{
}

Extension1stParty::Extension1stParty(const char* config_name, const char* display_name)
    : EncryptedExtension(ExtensionKeyScheme::FirstParty, config_name, display_name)
{
}

Extension3rdParty::Extension3rdParty(const char* name)
    : EncryptedExtension(ExtensionKeyScheme::ThirdParty, name, name)
{
}

Extension3rdParty::Extension3rdParty(const char* config_name, const char* display_name)
    : EncryptedExtension(ExtensionKeyScheme::ThirdParty, config_name, display_name)
{
}

}  // namespace WiimoteEmu
