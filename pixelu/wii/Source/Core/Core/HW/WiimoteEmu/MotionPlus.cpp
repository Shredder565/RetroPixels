// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/WiimoteEmu/MotionPlus.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

#include <mbedtls/bignum.h>

#include "Common/BitUtils.h"
#include "Common/ChunkFile.h"
#include "Common/Hash.h"
#include "Common/Logging/Log.h"
#include "Common/MathUtil.h"

#include "Core/HW/Wiimote.h"
#include "Core/HW/WiimoteEmu/Extension/DesiredExtensionState.h"

namespace
{
// Number of remote updates in the given time.
constexpr u8 UpdatesIn(int milliseconds)
{
  return static_cast<u8>(::Wiimote::UPDATE_FREQ * milliseconds / 1000);
}

// How long the MotionPlus is deaf while switching on or off. The detect pin drops meanwhile,
// which makes the remote send status reports.
constexpr u8 SWITCH_TIME = UpdatesIn(20);

// Time before parameter x is ready. A real unit needs about 1.5 s and games allow a minute.
// It must not be instant: the SDK only waits correctly after reading a value that is not
// ready yet.
constexpr u8 PARAMETER_X_TIME = UpdatesIn(500);

// Times before y0 (about 30 ms on hardware) and y1 (about 1.2 s; games accept faster).
constexpr u8 PARAMETER_Y0_TIME = UpdatesIn(30);
constexpr u8 PARAMETER_Y1_TIME = UpdatesIn(500);

// The challenge: the console sends nothing, the MotionPlus publishes x = s^2 mod n and then
// either y0 = s or y1 = s * sqrt(v) mod n. s may be any value in [2, n); this one is fixed so
// emulation stays deterministic. The trailing NUL is part of it.
constexpr u8 CHALLENGE_SECRET[] = "DOLPHIN DOES WHAT NINTENDON'T.";

constexpr char CHALLENGE_MODULUS[] =
    "67614561104116375676885818084175632651294951727285593632649596941616763967271774525888270484"
    "88546653264235848263182009106217734439508352645687684489830161";

constexpr char CHALLENGE_SQRT_V[] =
    "00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "00000000000000000000000000000000000000000000000000000000000000";

// An mbedtls big integer with automatic cleanup.
class BigInt
{
public:
  BigInt() { mbedtls_mpi_init(&m_value); }
  ~BigInt() { mbedtls_mpi_free(&m_value); }

  BigInt(const BigInt&) = delete;
  BigInt& operator=(const BigInt&) = delete;

  static BigInt FromDecimal(const char* digits)
  {
    BigInt result;
    mbedtls_mpi_read_string(&result.m_value, 10, digits);
    return result;
  }

  static BigInt FromSecret()
  {
    BigInt result;
    mbedtls_mpi_read_binary(&result.m_value, CHALLENGE_SECRET, sizeof(CHALLENGE_SECRET));
    return result;
  }

  // this = this * factor mod modulus
  void MultiplyMod(const BigInt& factor, const BigInt& modulus)
  {
    mbedtls_mpi_mul_mpi(&m_value, &m_value, &factor.m_value);
    mbedtls_mpi_mod_mpi(&m_value, &m_value, &modulus.m_value);
  }

  // this = this^2 mod modulus
  void SquareMod(const BigInt& modulus)
  {
    mbedtls_mpi_mul_mpi(&m_value, &m_value, &m_value);
    mbedtls_mpi_mod_mpi(&m_value, &m_value, &modulus.m_value);
  }

  // Stores the value little endian, zero padded. Leaves `out` alone if it does not fit.
  template <std::size_t N>
  void StoreLittleEndian(std::array<u8, N>* out) const
  {
    if (mbedtls_mpi_write_binary(&m_value, out->data(), out->size()) == 0)
      std::reverse(out->begin(), out->end());
  }

private:
  BigInt(BigInt&& other) noexcept : BigInt() { mbedtls_mpi_swap(&m_value, &other.m_value); }

  mbedtls_mpi m_value;
};

// Copies bit `from` of `src` into bit `to` of `dst`.
void MoveBit(u8& dst, int to, u8 src, int from)
{
  const u8 bit = static_cast<u8>(1u << to);
  if ((src >> from) & 1)
    dst = static_cast<u8>(dst | bit);
  else
    dst = static_cast<u8>(dst & ~bit);
}
}  // namespace

namespace WiimoteEmu
{
Common::Vec3 MotionPlus::DataFormat::Data::GetAngularVelocity(const CalibrationBlocks& blocks) const
{
  const auto calibration = blocks.GetRelevantCalibration(is_slow);

  // The calibration's signs do not follow the right-hand rule for pitch and yaw.
  const auto handedness = Common::Vec3(-1, +1, -1);

  constexpr auto radians_per_degree = float(MathUtil::TAU / 360);

  return gyro.GetNormalizedValue(calibration.value) * handedness *
         Common::Vec3(calibration.degrees) * radians_per_degree;
}

auto MotionPlus::CalibrationBlocks::GetRelevantCalibration(SlowType is_slow) const
    -> RelevantCalibration
{
  // x is pitch, y is roll, z is yaw.
  const CalibrationBlock& for_pitch = is_slow.x ? slow : fast;
  const CalibrationBlock& for_roll = is_slow.y ? slow : fast;
  const CalibrationBlock& for_yaw = is_slow.z ? slow : fast;

  RelevantCalibration result;
  result.value.zero = {for_pitch.pitch_zero, for_roll.roll_zero, for_yaw.yaw_zero};
  result.value.max = {for_pitch.pitch_scale, for_roll.roll_scale, for_yaw.yaw_scale};
  result.degrees = {static_cast<u16>(for_pitch.degrees_div_6 * 6),
                    static_cast<u16>(for_roll.degrees_div_6 * 6),
                    static_cast<u16>(for_yaw.degrees_div_6 * 6)};
  return result;
}

void MotionPlus::CalibrationData::UpdateChecksum()
{
  // CRC-32 over both blocks and their uid bytes (0x0e bytes each), skipping the checksum
  // halves themselves.
  const u8* const bytes = reinterpret_cast<const u8*>(this);
  u32 crc = Common::StartCRC32();
  crc = Common::UpdateCRC32(crc, bytes, 0xe);
  crc = Common::UpdateCRC32(crc, bytes + 0x10, 0xe);

  crc32_msb = static_cast<u16>(crc >> 16);
  crc32_lsb = static_cast<u16>(crc);
}

MotionPlus::MotionPlus() : Extension("MotionPlus")
{
}

void MotionPlus::Reset()
{
  m_reg_data = {};
  m_progress_timer = {};

  // Byte 0 tells whether the MotionPlus is built into the remote; ours can be unplugged.
  // Byte 2 changes on activation and byte 4 holds the passthrough mode.
  m_reg_data.ext_identifier = {0x00, 0x00, 0xa6, 0x20, 0x00, 0x05};

  static_assert(CALIBRATION_FAST_SCALE_DEGREES % 6 == 0);
  static_assert(CALIBRATION_SLOW_SCALE_DEGREES % 6 == 0);

  // Scale signs as on a real Wii Remote Plus, which makes every axis follow the right-hand
  // rule once GetAngularVelocity's sign fix is applied.
  const auto fill_block = [](CalibrationBlock& block, u16 degrees) {
    block.yaw_zero = CALIBRATION_ZERO;
    block.roll_zero = CALIBRATION_ZERO;
    block.pitch_zero = CALIBRATION_ZERO;
    block.yaw_scale = static_cast<u16>(CALIBRATION_ZERO - CALIBRATION_SCALE_OFFSET);
    block.roll_scale = static_cast<u16>(CALIBRATION_ZERO + CALIBRATION_SCALE_OFFSET);
    block.pitch_scale = static_cast<u16>(CALIBRATION_ZERO - CALIBRATION_SCALE_OFFSET);
    block.degrees_div_6 = static_cast<u8>(degrees / 6);
  };

  CalibrationData calibration;
  fill_block(calibration.fast, CALIBRATION_FAST_SCALE_DEGREES);
  fill_block(calibration.slow, CALIBRATION_SLOW_SCALE_DEGREES);

  // Games compare these against the last remote that passed the challenge and skip the
  // challenge on a match.
  calibration.uid_1 = 0x0b;
  calibration.uid_2 = 0xe9;

  calibration.UpdateChecksum();

  Common::BitCastPtr<CalibrationData>(m_reg_data.calibration_data.data()) = calibration;
}

void MotionPlus::DoState(PointerWrap& p)
{
  p.Do(m_reg_data);
  p.Do(m_progress_timer);
}

MotionPlus::ActivationStatus MotionPlus::GetActivationStatus() const
{
  const bool on_active_address = m_reg_data.ext_identifier[2] == (ACTIVE_DEVICE_ADDR << 1);

  if (!on_active_address)
    return m_progress_timer != 0 ? ActivationStatus::Deactivating : ActivationStatus::Inactive;

  return m_reg_data.challenge_state == ChallengeState::Activating ? ActivationStatus::Activating :
                                                                    ActivationStatus::Active;
}

MotionPlus::PassthroughMode MotionPlus::GetPassthroughMode() const
{
  return static_cast<PassthroughMode>(m_reg_data.ext_identifier[4]);
}

ExtensionPort& MotionPlus::GetExtPort()
{
  return m_extension_port;
}

int MotionPlus::BusRead(u8 slave_addr, u8 addr, int count, u8* data_out)
{
  const ActivationStatus status = GetActivationStatus();

  if (status == ActivationStatus::Inactive)
  {
    // Traffic for anything but 0x53 goes to the passthrough port.
    if (slave_addr != INACTIVE_DEVICE_ADDR)
      return m_i2c_bus.BusRead(slave_addr, addr, count, data_out);
    return RawRead(&m_reg_data, addr, count, data_out);
  }

  // Active: only 0x52 answers and nothing is passed through. While switching: nothing answers.
  if (status != ActivationStatus::Active || slave_addr != ACTIVE_DEVICE_ADDR)
    return 0;
  return RawRead(&m_reg_data, addr, count, data_out);
}

int MotionPlus::BusWrite(u8 slave_addr, u8 addr, int count, const u8* data_in)
{
  const ActivationStatus status = GetActivationStatus();

  if (status == ActivationStatus::Inactive)
  {
    if (slave_addr != INACTIVE_DEVICE_ADDR)
      return m_i2c_bus.BusWrite(slave_addr, addr, count, data_in);

    DEBUG_LOG_FMT(WIIMOTE, "M+ (inactive) write at {:#x}: {}", addr,
                  ArrayToString(data_in, static_cast<u32>(count)));

    const int written = RawWrite(&m_reg_data, addr, count, data_in);
    if (addr == PASSTHROUGH_MODE_OFFSET)
      OnPassthroughModeWrite();
    return written;
  }

  if (status != ActivationStatus::Active || slave_addr != ACTIVE_DEVICE_ADDR)
    return 0;

  DEBUG_LOG_FMT(WIIMOTE, "M+ (active) write at {:#x}: {}", addr,
                ArrayToString(data_in, static_cast<u32>(count)));

  const int written = RawWrite(&m_reg_data, addr, count, data_in);

  if (addr == offsetof(Register, init_trigger))
  {
    // Any value deactivates. The same write also initializes the passthrough extension.
    Deactivate();
    m_i2c_bus.BusWrite(slave_addr, addr, count, data_in);
  }
  else if (addr == offsetof(Register, challenge_type))
  {
    // Once x has been read the game asks for y0 or y1 here.
    if (m_reg_data.challenge_state == ChallengeState::ParameterXReady)
    {
      DEBUG_LOG_FMT(WIIMOTE, "M+ challenge type {:#x} requested", m_reg_data.challenge_type);

      // Games fail the challenge after roughly 0.5 s (y0) or 9.5 s (y1).
      m_progress_timer = m_reg_data.challenge_type == 0 ? PARAMETER_Y0_TIME : PARAMETER_Y1_TIME;
      m_reg_data.challenge_state = ChallengeState::PreparingY;
    }
  }
  else if (addr == offsetof(Register, calibration_trigger))
  {
    DEBUG_LOG_FMT(WIIMOTE, "M+ calibration trigger {:#x}", m_reg_data.calibration_trigger);
  }
  else if (addr == PASSTHROUGH_MODE_OFFSET)
  {
    // Some games write 0 here to switch the MotionPlus off.
    OnPassthroughModeWrite();
  }

  return written;
}

void MotionPlus::OnPassthroughModeWrite()
{
  const ActivationStatus status = GetActivationStatus();

  const PassthroughMode mode = GetPassthroughMode();
  const bool valid_mode = mode == PassthroughMode::Disabled || mode == PassthroughMode::Nunchuk ||
                          mode == PassthroughMode::Classic;

  if (valid_mode && status != ActivationStatus::Active)
    Activate();
  else if (!valid_mode && status != ActivationStatus::Inactive)
    Deactivate();
}

void MotionPlus::Activate()
{
  DEBUG_LOG_FMT(WIIMOTE, "M+ activated");

  m_reg_data.ext_identifier[2] = ACTIVE_DEVICE_ADDR << 1;

  // Clears the extension_connected and is_mp_data bits along with everything else.
  m_reg_data.controller_data = {};

  m_reg_data.challenge_state = ChallengeState::Activating;
  m_progress_timer = SWITCH_TIME;
}

void MotionPlus::Deactivate()
{
  DEBUG_LOG_FMT(WIIMOTE, "M+ deactivated");

  m_reg_data.ext_identifier[2] = INACTIVE_DEVICE_ADDR << 1;
  m_progress_timer = SWITCH_TIME;
}

bool MotionPlus::ReadDeviceDetectPin() const
{
  const ActivationStatus status = GetActivationStatus();
  if (status == ActivationStatus::Inactive)
    return m_extension_port.IsDeviceConnected();
  return status == ActivationStatus::Active;
}

void MotionPlus::BuildDesiredExtensionState(DesiredExtensionState*)
{
  // The remote builds the gyroscope state itself.
}

void MotionPlus::Update(const DesiredExtensionState&)
{
  if (m_progress_timer != 0)
    --m_progress_timer;

  if (m_progress_timer == 0 && GetActivationStatus() == ActivationStatus::Activating)
  {
    // Switched on: start working on parameter x.
    m_reg_data.challenge_state = ChallengeState::PreparingX;
    m_progress_timer = PARAMETER_X_TIME;
  }

  if (GetActivationStatus() != ActivationStatus::Active)
    return;

  u8* const report = m_reg_data.controller_data.data();
  DataFormat format = Common::BitCastPtr<DataFormat>(report);

  const bool ext_present = m_extension_port.IsDeviceConnected();
  if (ext_present != bool(format.extension_connected))
  {
    if (ext_present)
    {
      // A real MotionPlus initializes newly attached extensions itself. This is a simplified
      // version that the emulated extensions are happy with.
      DEBUG_LOG_FMT(WIIMOTE, "M+ setting up the attached extension");

      // Unencrypted mode.
      const u8 init_value = 0x55;
      m_i2c_bus.BusWrite(ACTIVE_DEVICE_ADDR, offsetof(Register, init_trigger), 1, &init_value);

      std::array<u8, 6> id = {};
      m_i2c_bus.BusRead(ACTIVE_DEVICE_ADDR, offsetof(Register, ext_identifier), int(id.size()),
                        id.data());
      m_reg_data.passthrough_ext_id_0 = id[0];
      m_reg_data.passthrough_ext_id_4 = id[4];
      m_reg_data.passthrough_ext_id_5 = id[5];

      m_i2c_bus.BusRead(ACTIVE_DEVICE_ADDR, offsetof(Register, calibration_data),
                        int(m_reg_data.passthrough_ext_calib.size()),
                        m_reg_data.passthrough_ext_calib.data());
    }

    format.extension_connected = ext_present;
    Common::BitCastPtr<DataFormat>(report) = format;
  }

  // The challenge only advances once the current step's time is up.
  if (m_progress_timer != 0)
    return;

  if (m_reg_data.challenge_state == ChallengeState::PreparingX)
  {
    BigInt x = BigInt::FromSecret();
    x.SquareMod(BigInt::FromDecimal(CHALLENGE_MODULUS));
    x.StoreLittleEndian(&m_reg_data.challenge_data);

    DEBUG_LOG_FMT(WIIMOTE, "M+ challenge parameter x is ready");
    m_reg_data.challenge_state = ChallengeState::ParameterXReady;
  }
  else if (m_reg_data.challenge_state == ChallengeState::PreparingY)
  {
    BigInt y = BigInt::FromSecret();
    if (m_reg_data.challenge_type != 0)
    {
      y.MultiplyMod(BigInt::FromDecimal(CHALLENGE_SQRT_V), BigInt::FromDecimal(CHALLENGE_MODULUS));
    }
    y.StoreLittleEndian(&m_reg_data.challenge_data);

    DEBUG_LOG_FMT(WIIMOTE, "M+ challenge parameter y is ready");
    m_reg_data.challenge_state = ChallengeState::ParameterYReady;
  }
}

MotionPlus::DataFormat::Data MotionPlus::GetGyroscopeData(const Common::Vec3& angular_velocity)
{
  DataFormat::Data result;
  for (std::size_t axis = 0; axis != angular_velocity.data.size(); ++axis)
  {
    const float rate = angular_velocity.data[axis];

    // Prefer the precise "slow" range whenever the rate fits in it.
    const bool slow = std::abs(rate) < SLOW_MAX_RAD_PER_SEC;
    const s32 counts = static_cast<s32>(std::lround(rate * (slow ? SLOW_SCALE : FAST_SCALE)));

    result.is_slow.data[axis] = slow;
    result.gyro.value.data[axis] = static_cast<u16>(std::clamp(counts + ZERO_VALUE, 0, MAX_VALUE));
  }
  return result;
}

MotionPlus::DataFormat::Data MotionPlus::GetDefaultGyroscopeData()
{
  constexpr auto ZERO = static_cast<u16>(ZERO_VALUE);
  return DataFormat::Data{DataFormat::GyroRawValue{DataFormat::GyroType{ZERO, ZERO, ZERO}},
                          DataFormat::SlowType{true, true, true}};
}

// On hardware a read of register 0x00 triggers this. Doing it once per update instead keeps
// emulation deterministic.
void MotionPlus::PrepareInput(const MotionPlus::DataFormat::Data& gyroscope_data)
{
  if (GetActivationStatus() != ActivationStatus::Active)
    return;

  u8* const report = m_reg_data.controller_data.data();
  DataFormat format = Common::BitCastPtr<DataFormat>(report);

  // Update() owns this bit (it reacts to changes), so keep what is stored.
  const bool ext_present = format.extension_connected;

  // Reports alternate between gyroscope data and passthrough data. (A real unit first sends
  // one garbage report; games do not need that.)
  format.is_mp_data ^= true;

  if (!format.is_mp_data)
  {
    const PassthroughMode mode = GetPassthroughMode();
    if (mode == PassthroughMode::Nunchuk || mode == PassthroughMode::Classic)
    {
      // A real unit reads six bytes from 0x52/0x00 and sends them in the next report; using
      // them right away is simpler. A failed read (unplugged) falls back to gyroscope data.
      constexpr int PASSTHROUGH_BYTES = 6;
      const int got = m_i2c_bus.BusRead(ExtensionPort::REPORT_I2C_SLAVE,
                                        ExtensionPort::REPORT_I2C_ADDR, PASSTHROUGH_BYTES, report);
      if (got == PASSTHROUGH_BYTES)
      {
        ApplyPassthroughModifications(mode, report);
        format = Common::BitCastPtr<DataFormat>(report);
        format.is_mp_data = false;
      }
      else
      {
        format.is_mp_data = true;
      }
    }
    else
    {
      // Passthrough off. Any other mode would have deactivated the MotionPlus already.
      if (mode != PassthroughMode::Disabled)
      {
        ERROR_LOG_FMT(WIIMOTE, "M+ in unexpected passthrough mode {}", static_cast<int>(mode));
      }
      format.is_mp_data = true;
    }
  }

  if (format.is_mp_data)
  {
    const auto& rates = gyroscope_data.gyro.value;
    const auto& slow = gyroscope_data.is_slow;

    format.pitch_slow = u8(slow.x);
    format.roll_slow = u8(slow.y);
    format.yaw_slow = u8(slow.z);

    format.pitch1 = u8(rates.x);
    format.roll1 = u8(rates.y);
    format.yaw1 = u8(rates.z);

    format.pitch2 = u8(rates.x >> 8) & 0x3f;
    format.roll2 = u8(rates.y >> 8) & 0x3f;
    format.yaw2 = u8(rates.z >> 8) & 0x3f;
  }

  format.extension_connected = ext_present;
  format.zero = 0;

  Common::BitCastPtr<DataFormat>(report) = format;
}

// Passthrough loses some bits to make room for the MotionPlus flags (WiiBrew, verified bit by
// bit on hardware).
void MotionPlus::ApplyPassthroughModifications(PassthroughMode mode, u8* data)
{
  u8& last = data[5];
  switch (mode)
  {
  case PassthroughMode::Nunchuk:
    // The accelerometer LSBs are dropped: buttons and remaining accel bits shift into place.
    // The steps must run in this order.
    MoveBit(last, 6, last, 7);
    MoveBit(last, 7, data[4], 0);
    MoveBit(last, 4, last, 3);
    MoveBit(last, 3, last, 1);
    MoveBit(last, 2, last, 0);
    break;

  case PassthroughMode::Classic:
    // The left stick's X and Y LSBs make room for the two buttons of byte 5.
    MoveBit(data[0], 0, last, 0);
    MoveBit(data[1], 0, last, 1);
    break;

  default:
    break;
  }
}

void MotionPlus::ReversePassthroughModifications(PassthroughMode mode, u8* data)
{
  u8& last = data[5];
  switch (mode)
  {
  case PassthroughMode::Nunchuk:
    MoveBit(last, 0, last, 2);
    MoveBit(last, 1, last, 3);
    MoveBit(last, 3, last, 4);
    MoveBit(data[4], 0, last, 7);
    MoveBit(last, 7, last, 6);

    // The lost LSBs are approximated by their neighbours.
    MoveBit(last, 2, last, 3);
    MoveBit(last, 4, last, 5);
    MoveBit(last, 6, last, 7);
    break;

  case PassthroughMode::Classic:
    MoveBit(last, 0, data[0], 0);
    MoveBit(last, 1, data[1], 0);

    MoveBit(data[0], 0, data[0], 1);
    MoveBit(data[1], 0, data[1], 1);

    // An unused Classic Controller button bit, here "not pressed". (On a DJ Hero turntable
    // this bit matters, so passthrough cannot work for it.)
    data[4] = static_cast<u8>(data[4] | 1);
    break;

  default:
    break;
  }
}

}  // namespace WiimoteEmu
