// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"

#include "Core/HW/Triforce/SerialDevice.h"

namespace MagCard
{

// Sanwa-style magnetic card reader/writer with a thermal printer, as used on
// serial port B by F-Zero AX and Mario Kart Arcade GP. The host polls with ENQ
// and gets a status packet back; commands arrive as STX packets and run over
// several polls. A simulated player inserts and takes the card.
class MagneticCardReader : public Triforce::SerialDevice
{
public:
  static constexpr std::size_t TRACK_SIZE = 69;
  static constexpr std::size_t NUM_TRACKS = 3;

  // Owned by the baseboard; not saved with the reader.
  struct Settings
  {
    std::string card_name;
    std::string card_path;

    bool insert_card_when_possible = true;
    bool report_dispenser_empty = false;
  };

  explicit MagneticCardReader(Settings* settings);
  ~MagneticCardReader() override;

  MagneticCardReader(const MagneticCardReader&) = delete;
  MagneticCardReader& operator=(const MagneticCardReader&) = delete;
  MagneticCardReader(MagneticCardReader&&) = delete;
  MagneticCardReader& operator=(MagneticCardReader&&) = delete;

  void Update() override;

  void DoState(PointerWrap& p) override;

protected:
  // Where the card is. Each model reports these with its own values.
  enum class R : u8
  {
    NO_CARD,
    READ_WRITE_HEAD,
    THERMAL_HEAD,
    DISPENSER_THERMAL,
    ENTRY_SLOT,

    MAX_POSITIONS,
  };

  // Error status.
  enum class P : u8
  {
    NO_ERR = 0x30,
    READ_ERR = 0x31,
    WRITE_ERR = 0x32,
    CARD_JAM = 0x33,
    MOTOR_ERR = 0x34,
    PRINT_ERR = 0x35,
    ILLEGAL_ERR = 0x38,
    BATTERY_ERR = 0x40,
    SYSTEM_ERR = 0x41,
    TRACK_1_READ_ERR = 0x51,
    TRACK_2_READ_ERR = 0x52,
    TRACK_3_READ_ERR = 0x53,
    TRACK_1_AND_2_READ_ERR = 0x54,
    TRACK_1_AND_3_READ_ERR = 0x55,
    TRACK_2_AND_3_READ_ERR = 0x56,
  };

  // Job status.
  enum class S : u8
  {
    NO_JOB = 0x30,
    ILLEGAL_COMMAND = 0x32,
    RUNNING_COMMAND = 0x33,
    WAITING_FOR_CARD = 0x34,
    DISPENSER_EMPTY = 0x35,
    NO_DISPENSER = 0x36,
    CARD_FULL = 0x37,
  };

  struct Status
  {
    R r = R::NO_CARD;
    P p = P::NO_ERR;
    S s = S::NO_JOB;

    void SoftReset()
    {
      p = P::NO_ERR;
      s = S::NO_JOB;
    }
  };

  enum class InitMode : u8
  {
    Standard = 0x30,
    EjectAfter = 0x31,
    ResetSpecifications = 0x32,
  };

  // The model's wire value for the card position (and its flag bits).
  virtual u8 GetPositionValue() = 0;

  virtual bool IsReadyForCard();

  virtual void Command_10_Initialize();
  virtual void Command_D0_ControlShutter();

  void SetPError(P error_code);
  void SetSError(S error_code);
  void FinishCommand();

  void MoveCard(R new_position);

  // Somewhere the reader can sense it, the entry slot included.
  bool IsCardPresent() const;
  // Past the entry slot.
  bool IsCardLoaded() const;

  Settings* const m_card_settings;

  Status m_status{};

  // The parameters of the running command.
  std::vector<u8> m_current_packet;

private:
  using Track = std::optional<std::array<u8, TRACK_SIZE>>;

  bool AcceptPacket(std::span<const u8> packet);
  void SendStatus();
  void StepCommand();
  void StepPerson();
  void RunCommand();

  bool IsRunning() const { return m_step > 0; }
  std::string CardFilePath() const;
  bool CardFileExists() const;
  void LoadCardFile();
  void SaveCardFile();
  void ClearTracks();
  void EjectCard();
  void DispenseCard();
  void AppendTrack(std::size_t index);

  void ReadData();
  void GetData();
  void WriteData();
  void PrintLine(bool check_size);
  void PrintBarcode();
  void Clean();
  void Dispense();

  u8 m_command = 0;
  std::vector<u8> m_reply;
  s32 m_step = 0;
  std::array<Track, NUM_TRACKS> m_tracks;
  DT m_machine_time{};
  DT m_person_time{};
};

}  // namespace MagCard
