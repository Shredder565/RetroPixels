// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <algorithm>
#include <cstring>
#include <type_traits>
#include <vector>

#include "Common/CommonTypes.h"

namespace WiimoteEmu
{
class I2CBus;

// A device on the remote's i2c bus (speaker, camera, extension). A device answers only for its
// own slave address and returns how many bytes it transferred; 0 means it did not answer.
class I2CSlave
{
  friend I2CBus;

protected:
  virtual ~I2CSlave() = default;

  virtual int BusRead(u8 slave_addr, u8 addr, int count, u8* data_out) = 0;
  virtual int BusWrite(u8 slave_addr, u8 addr, int count, const u8* data_in) = 0;

  // Plain reads and writes of a 256 byte register file. Transfers stop at the end of the
  // register file (no wrap-around to address 0).
  template <typename T>
  static int RawRead(T* reg_data, u8 addr, int count, u8* data_out)
  {
    const int length = ClampToRegisterFile<T>(addr, count);
    std::memcpy(data_out, reinterpret_cast<const u8*>(reg_data) + addr, ToSize(length));
    return length;
  }

  template <typename T>
  static int RawWrite(T* reg_data, u8 addr, int count, const u8* data_in)
  {
    const int length = ClampToRegisterFile<T>(addr, count);
    std::memcpy(reinterpret_cast<u8*>(reg_data) + addr, data_in, ToSize(length));
    return length;
  }

private:
  template <typename T>
  static int ClampToRegisterFile(u8 addr, int count)
  {
    static_assert(std::is_standard_layout_v<T> && std::is_trivially_copyable_v<T>);
    static_assert(sizeof(T) == 0x100, "i2c register files are 256 bytes");
    return std::min(count, static_cast<int>(sizeof(T)) - addr);
  }

  static size_t ToSize(int length) { return length > 0 ? static_cast<size_t>(length) : 0; }
};

// The bus routes each transfer to the first attached device that answers.
class I2CBus
{
public:
  void AddSlave(I2CSlave* slave);
  void RemoveSlave(I2CSlave* slave);

  void Reset();

  int BusRead(u8 slave_addr, u8 addr, int count, u8* data_out);
  int BusWrite(u8 slave_addr, u8 addr, int count, const u8* data_in);

private:
  // Not owned.
  std::vector<I2CSlave*> m_slaves;
};

}  // namespace WiimoteEmu
