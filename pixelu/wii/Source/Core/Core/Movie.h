// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/HW/WiimoteEmu/DesiredWiimoteState.h"

struct BootParameters;

struct GCPadStatus;
class PointerWrap;

namespace Core
{
class System;
}

namespace ExpansionInterface
{
enum class Slot : int;
}

namespace WiimoteCommon
{
class DataReportBuilder;
}

namespace WiimoteEmu
{
class EncryptionKey;
enum ExtensionNumber : u8;
}  // namespace WiimoteEmu

// TAS movies: recording and playing back controller input in the DTM format.
namespace Movie
{
// What sits in each GameCube controller port of a movie.
enum class ControllerType
{
  None = 0,
  GC,
  GBA,
};
using ControllerTypeArray = std::array<ControllerType, 4>;
using WiimoteEnabledArray = std::array<bool, 4>;

// One GameCube pad poll as stored in a DTM file. The layout is part of the
// file format and must not change.
#pragma pack(push, 1)
struct ControllerState
{
  // Byte 0 and 1: digital inputs, one bit each.
  bool Start : 1, A : 1, B : 1, X : 1, Y : 1, Z : 1;
  bool DPadUp : 1, DPadDown : 1, DPadLeft : 1, DPadRight : 1;
  bool L : 1, R : 1;
  bool disc : 1;          // the disc was swapped at this poll
  bool reset : 1;         // the reset button was tapped at this poll
  bool is_connected : 1;  // the pad counts as plugged in
  bool get_origin : 1;    // the game asked for the analog origin again

  // Bytes 2 to 7: analog values.
  u8 TriggerL, TriggerR;
  u8 AnalogStickX, AnalogStickY;
  u8 CStickX, CStickY;
};
static_assert(sizeof(ControllerState) == 8, "ControllerState should be 8 bytes");
#pragma pack(pop)

// The 256-byte header at the start of every DTM file. Other tools read this
// format too, so the layout stays fixed; new options go into the reserved bytes.
#pragma pack(push, 1)
struct DTMHeader
{
  // gameID without its trailing NULs.
  std::string_view GetGameID() const
  {
    return {gameID.data(), strnlen(gameID.data(), gameID.size())};
  }

  std::array<u8, 4> filetype;  // magic: 'D' 'T' 'M' 0x1A

  std::array<char, 6> gameID;
  bool bWii;

  // Bit n (0-3): GameCube port n+1 in use; bit n+4: Wii Remote n+1 in use.
  u8 controllers;

  bool bFromSaveState;  // starts from <movie>.sav rather than from boot
  u64 frameCount;
  u64 inputCount;
  u64 lagCount;
  u64 uniqueID;  // always written as 0
  u32 numRerecords;
  std::array<char, 32> author;  // UTF-8

  std::array<char, 16> videoBackend;   // UTF-8
  std::array<char, 16> audioEmulator;  // UTF-8
  std::array<u8, 16> md5;              // MD5 of the disc image

  u64 recordingStartTime;  // RTC at the start, in seconds since 1970

  // The settings below are applied during playback when bSaveConfig is set.
  bool bSaveConfig;
  bool bSkipIdle;
  bool bDualCore;
  bool bProgressive;
  bool bDSPHLE;
  bool bFastDiscSpeed;
  u8 CPUCore;  // a PowerPC::CPUCore value
  bool bEFBAccessEnable;
  bool bEFBCopyEnable;
  bool bSkipEFBCopyToRam;
  bool bEFBCopyCacheEnable;
  bool bEFBEmulateFormatChanges;
  bool bImmediateXFB;
  bool bSkipXFBCopyToRam;
  u8 memcards;      // bit 0: slot A, bit 1: slot B
  bool bClearSave;  // start playback with a fresh memory card or save
  u8 bongos;        // bit n: port n+1 has bongos
  bool bSyncGPU;
  bool bNetPlay;
  bool bPAL60;
  u8 language;
  u8 reserved3;
  bool bFollowBranch;
  bool bUseFMA;
  u8 GBAControllers;  // bit n: port n+1 has an emulated GBA
  bool bWidescreen;   // SYSCONF aspect ratio is 16:9
  u8 countryCode;     // SYSCONF country code
  std::array<u8, 5> reserved;
  std::array<char, 40> discChange;  // file name of the disc to switch to
  std::array<u8, 20> revision;      // build's git hash, as bytes
  u32 DSPiromHash;
  u32 DSPcoefHash;
  u64 tickCount;
  std::array<u8, 11> reserved2;  // pads the header to 256 bytes
};
static_assert(sizeof(DTMHeader) == 256, "DTMHeader should be 256 bytes");

#pragma pack(pop)

enum class PlayMode
{
  None = 0,
  Recording,
  Playing,
};

class MovieManager
{
public:
  explicit MovieManager(Core::System& system);
  MovieManager(const MovieManager& other) = delete;
  MovieManager(MovieManager&& other) = delete;
  MovieManager& operator=(const MovieManager& other) = delete;
  MovieManager& operator=(MovieManager&& other) = delete;
  ~MovieManager();

  void FrameUpdate();
  void InputUpdate();
  // Runs at every boot, with or without a movie.
  void Init(const BootParameters& boot);

  void SetPolledDevice();

  bool IsRecordingInput() const;
  bool IsRecordingInputFromSaveState() const;
  bool IsJustStartingRecordingInputFromSaveState() const;
  bool IsJustStartingPlayingInputFromSaveState() const;
  bool IsPlayingInput() const;
  bool IsMovieActive() const;
  bool IsReadOnly() const;
  u64 GetRecordingStartTime() const;

  u64 GetCurrentFrame() const;
  u64 GetTotalFrames() const;
  u64 GetCurrentInputCount() const;
  u64 GetTotalInputCount() const;
  u64 GetCurrentLagCount() const;
  u64 GetTotalLagCount() const;

  void SetClearSave(bool enabled);
  void SignalDiscChange(const std::string& new_path);
  void SetReset(bool reset);

  bool IsConfigSaved() const;
  bool IsStartingFromClearSave() const;
  bool IsUsingMemcard(ExpansionInterface::Slot slot) const;
  bool IsNetPlayRecording() const;

  bool IsUsingPad(int controller) const;
  bool IsUsingWiimote(int wiimote) const;
  bool IsUsingBongo(int controller) const;
  bool IsUsingGBA(int controller) const;
  void ChangePads();
  void ChangeWiiPads();

  void SetReadOnly(bool bEnabled);

  bool BeginRecordingInput(const ControllerTypeArray& controllers,
                           const WiimoteEnabledArray& wiimotes);
  void RecordInput(const GCPadStatus* PadStatus, int controllerID);
  void RecordWiimote(int wiimote, const WiimoteEmu::SerializedWiimoteState& serialized_state);

  bool PlayInput(const std::string& movie_path, std::optional<std::string>* savestate_path);
  void LoadInput(const std::string& movie_path);
  void ReadHeader();
  void PlayController(GCPadStatus* PadStatus, int controllerID);
  bool PlayWiimote(int wiimote, WiimoteEmu::DesiredWiimoteState* desired_state);
  void EndPlayInput(bool cont);
  void SaveRecording(const std::string& filename);
  void DoState(PointerWrap& p);
  void Shutdown();
  void CheckPadStatus(const GCPadStatus* PadStatus, int controllerID);
  void CheckWiimoteStatus(int wiimote, const WiimoteEmu::DesiredWiimoteState& desired_state);

  // On-screen display helpers; called from the GPU thread.
  std::string GetInputDisplay();
  std::string GetRTCDisplay() const;
  std::string GetRerecords() const;

private:
  void GetSettings();
  void CheckInputEnd();
  void SetInputDisplay(size_t slot, std::string text);
  // Switches to `mode` (with a message) unless the movie is already in it.
  void SwitchPlayMode(PlayMode mode, const char* message);
  void ReportMismatch(const std::vector<u8>& saved_input, size_t index);

  // Run on detached threads.
  void CheckMD5();
  void GetMD5();

  bool m_read_only = true;
  u32 m_rerecords = 0;
  PlayMode m_play_mode = PlayMode::None;

  std::array<ControllerType, 4> m_controllers{};
  std::array<bool, 4> m_wiimotes{};
  // The pad poll being recorded or played back.
  ControllerState m_pad_state{};
  DTMHeader m_temp_header{};
  // Input data after the header; m_current_byte is the read/write position.
  std::vector<u8> m_temp_input;
  u64 m_current_byte = 0;
  u64 m_current_frame = 0;
  u64 m_total_frames = 0;  // in VI frames
  u64 m_current_lag_count = 0;
  u64 m_total_lag_count = 0;
  u64 m_current_input_count = 0;
  u64 m_total_input_count = 0;
  u64 m_total_tick_count = 0;
  u64 m_tick_count_at_last_input = 0;
  u64 m_recording_start_time = 0;
  bool m_save_config = false;
  bool m_net_play = false;
  bool m_clear_save = false;
  bool m_has_disc_change = false;
  bool m_reset = false;
  std::string m_author;
  std::string m_disc_change_filename;
  std::array<u8, 16> m_md5{};
  u8 m_bongos = 0;
  u8 m_memcards = 0;
  std::array<u8, 20> m_revision{};
  u32 m_dsp_irom_hash = 0;
  u32 m_dsp_coef_hash = 0;

  bool m_recording_from_save_state = false;
  // Whether any input device was read during the current frame.
  bool m_polled = false;

  // Disc image of the running game; empty when not booted from a disc.
  std::string m_current_file_name;

  // Lines for GetInputDisplay: pads 1-4, then Wii Remotes 1-4. Written on the
  // CPU thread and read on the GPU thread, hence the lock.
  std::mutex m_input_display_lock;
  std::array<std::string, 8> m_input_display;

  Core::System& m_system;
};

}  // namespace Movie
