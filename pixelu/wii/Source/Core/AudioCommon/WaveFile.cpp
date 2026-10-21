// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "AudioCommon/WaveFile.h"

#include <algorithm>
#include <cstring>
#include <string>

#include <fmt/format.h>

#include "AudioCommon/Mixer.h"
#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/StringUtil.h"
#include "Common/Swap.h"
#include "Core/Config/MainSettings.h"

namespace
{
// RIFF/WAVE header layout (44 bytes). The two size fields are placeholders until Stop() fills in
// the real sizes; the large placeholder keeps a dump readable if the program dies before that.
constexpr std::size_t HEADER_BYTES = 44;
constexpr std::size_t RIFF_SIZE_OFFSET = 4;
constexpr std::size_t DATA_SIZE_OFFSET = 40;
constexpr u32 PLACEHOLDER_SIZE = 100000000;

void PutLE32(std::array<u8, HEADER_BYTES>& header, std::size_t offset, u32 value)
{
  for (std::size_t i = 0; i < 4; ++i)
    header[offset + i] = static_cast<u8>(value >> (8 * i));
}

void PutTag(std::array<u8, HEADER_BYTES>& header, std::size_t offset, const char (&tag)[5])
{
  std::memcpy(header.data() + offset, tag, 4);
}

std::array<u8, HEADER_BYTES> MakeHeader(u32 sample_rate)
{
  std::array<u8, HEADER_BYTES> header{};
  PutTag(header, 0, "RIFF");
  PutLE32(header, RIFF_SIZE_OFFSET, PLACEHOLDER_SIZE);
  PutTag(header, 8, "WAVE");
  PutTag(header, 12, "fmt ");
  PutLE32(header, 16, 16);              // fmt chunk size
  PutLE32(header, 20, 0x00020001);      // PCM, 2 channels
  PutLE32(header, 24, sample_rate);     // frames per second
  PutLE32(header, 28, sample_rate * 4); // bytes per second
  PutLE32(header, 32, 0x00100004);      // 4 bytes per frame, 16 bits per sample
  PutTag(header, 36, "data");
  PutLE32(header, DATA_SIZE_OFFSET, PLACEHOLDER_SIZE - 32);
  return header;
}

bool WriteLE32(File::IOFile& file, u32 value)
{
  const std::array<u8, 4> bytes = {static_cast<u8>(value), static_cast<u8>(value >> 8),
                                   static_cast<u8>(value >> 16), static_cast<u8>(value >> 24)};
  return file.WriteBytes(bytes.data(), bytes.size());
}
}  // namespace

WaveFileWriter::WaveFileWriter() = default;

WaveFileWriter::~WaveFileWriter()
{
  Stop();
}

bool WaveFileWriter::Start(const std::string& filename, u32 sample_rate_divisor)
{
  if (File::Exists(filename))
  {
    const bool overwrite = Config::Get(Config::MAIN_DUMP_AUDIO_SILENT) ||
                           AskYesNoFmtT("Delete the existing file '{0}'?", filename);
    if (!overwrite)
      return false;
    File::Delete(filename);
  }

  if (m_file)
  {
    PanicAlertFmtT("The file {0} was already open, the file header will not be written.", filename);
    return false;
  }

  m_file.Open(filename, "wb");
  if (!m_file)
  {
    PanicAlertFmtT(
        "The file {0} could not be opened for writing. Please check if it's already opened "
        "by another program.",
        filename);
    return false;
  }

  m_audio_size = 0;
  if (m_stem.empty())
    SplitPath(filename, nullptr, &m_stem, nullptr);
  m_divisor = sample_rate_divisor;

  const auto sample_rate = static_cast<u32>(Mixer::FIXED_SAMPLE_RATE_DIVIDEND / sample_rate_divisor);
  const std::array<u8, HEADER_BYTES> header = MakeHeader(sample_rate);
  m_file.WriteBytes(header.data(), header.size());

  if (m_file.Tell() != HEADER_BYTES)
    PanicAlertFmt("Wrong offset: {}", m_file.Tell());

  return true;
}

void WaveFileWriter::Stop()
{
  if (m_file.IsOpen())
  {
    // Fill in the RIFF chunk size (everything after its size field) and the data size.
    m_file.Seek(RIFF_SIZE_OFFSET, File::SeekOrigin::Begin);
    WriteLE32(m_file, m_audio_size + 36);
    m_file.Seek(DATA_SIZE_OFFSET, File::SeekOrigin::Begin);
    WriteLE32(m_file, m_audio_size);
  }
  m_file.Close();
}

void WaveFileWriter::AddStereoSamplesBE(const short* sample_data, u32 count,
                                        u32 sample_rate_divisor, int l_volume, int r_volume)
{
  if (!m_file)
  {
    ERROR_LOG_FMT(AUDIO, "WaveFileWriter - file not open.");
    return;
  }

  if (count > MAX_FRAMES_PER_CALL)
  {
    ERROR_LOG_FMT(AUDIO, "WaveFileWriter - buffer too small (count = {}).", count);
    return;
  }

  const std::size_t sample_count = std::size_t{count} * 2;
  if (m_skip_silence &&
      std::all_of(sample_data, sample_data + sample_count, [](short s) { return s == 0; }))
  {
    return;
  }

  // Swap to little-endian, put the left channel first and apply the volume (256 = unity).
  for (std::size_t frame = 0; frame < count; ++frame)
  {
    const auto left = static_cast<s16>(Common::swap16(static_cast<u16>(sample_data[2 * frame + 1])));
    const auto right = static_cast<s16>(Common::swap16(static_cast<u16>(sample_data[2 * frame])));
    m_scratch[2 * frame] = static_cast<s16>(left * l_volume / 256);
    m_scratch[2 * frame + 1] = static_cast<s16>(right * r_volume / 256);
  }

  if (sample_rate_divisor != m_divisor)
  {
    // New rate: close this file and continue in "<stem><n>.wav" in the audio dump folder.
    Stop();
    ++m_sequence;
    Start(fmt::format("{}{}{}.wav", File::GetUserPath(D_DUMPAUDIO_IDX), m_stem, m_sequence),
          sample_rate_divisor);
    m_divisor = sample_rate_divisor;
  }

  m_file.WriteBytes(m_scratch.data(), sample_count * sizeof(s16));
  m_audio_size += count * 4;
}
