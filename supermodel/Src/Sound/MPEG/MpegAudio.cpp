// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// MPEG audio playback for the Digital Sound Board (see MpegAudio.h).
//
// Replacement tracks: Music.xml can map an MPEG ROM start offset of a game
// to an audio file on disk,
//   <games>
//     <game name="scud">
//       <track mpeg_rom_start_offset="0x123400" file_start_offset="0" filepath="song1.mp3" />
//     </game>
//   </games>
// When the board starts a stream at that offset, the file plays instead
// (from file_start_offset to its end).

#define MINIMP3_IMPLEMENTATION
#include "Pkgs/minimp3.h"
#include "MpegAudio.h"
#include "Util/ConfigBuilders.h"
#include "OSD/Logger.h"

#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>

namespace
{
  constexpr int kHeaderBytes = 4;   // an MPEG audio frame header

  using Bytes = std::shared_ptr<uint8_t[]>;

  struct Replacement
  {
    Bytes data;
    size_t size = 0;
    size_t startInFile = 0;

    // Where playback starts: the configured offset, or the start of the
    // file if that is past its end.
    size_t Start() const { return startInFile < size ? startInFile : 0; }
  };

  std::map<uint32_t, Replacement> s_replacements;   // by MPEG ROM start offset

  struct Stream
  {
    mp3dec_t decoder;
    mp3dec_frame_info_t frame;
    const uint8_t *data = nullptr;
    int size = 0;
    int pos = 0;                  // byte position of the next frame
    bool loop = false;
    bool stopped = false;
    int frameSamples = 0;         // samples per channel decoded from the last frame
    int frameOut = 0;             // interleaved index of the next one to output
    short pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    Bytes owner;                  // keeps a replacement file alive while it plays
  };

  Stream s_stream{};

  bool ReadWholeFile(const std::string &path, Replacement &out)
  {
    FILE *fp = fopen(path.c_str(), "rb");
    if (fp == nullptr)
    {
      ErrorLog("Unable to load music track from disk: %s.", path.c_str());
      return false;
    }
    fseek(fp, 0, SEEK_END);
    const long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    Bytes data(new uint8_t[size], std::default_delete<uint8_t[]>());
    const size_t got = fread(data.get(), 1, (size_t)size, fp);
    (void)got;
    fclose(fp);
    out.data = data;
    out.size = (size_t)size;
    return true;
  }

  // Offset from b to a as the 32-bit difference the stream positions use.
  int Distance(const uint8_t *a, const uint8_t *b)
  {
    return (int)(uint32_t)((uintptr_t)a - (uintptr_t)b);
  }

  void OutputDecoded(int16_t *&left, int16_t *&right, int &count)
  {
    const int channels = s_stream.frame.channels;
    while (s_stream.frameOut < s_stream.frameSamples * channels && count > 0)
    {
      *left++ = s_stream.pcm[s_stream.frameOut];
      *right++ = s_stream.pcm[s_stream.frameOut + channels - 1];   // mono: both from the one channel
      s_stream.frameOut += channels;
      count--;
    }
  }

  void OutputSilence(int16_t *&left, int16_t *&right, int &count)
  {
    for (; count > 0; count--)
    {
      *left++ = 0;
      *right++ = 0;
    }
  }
}

void MpegDec::LoadCustomTracks(const std::string &music_filepath, const Game &game)
{
  s_replacements.clear();

  if (game.mpeg_board.empty() || !std::filesystem::exists(music_filepath))
    return;   // no MPEG board, or no Music.xml (it is optional)

  Util::Config::Node xml("xml");
  if (Util::Config::FromXMLFile(&xml, music_filepath))
  {
    ErrorLog("Custom music configuration file could not be loaded. Original game tracks will be used.");
    return;
  }

  std::map<std::string, Replacement> files;   // each file read once

  for (auto &root : xml)
  {
    if (root.Key() != "games")
      continue;
    for (auto &gameNode : root)
    {
      if (gameNode.Key() != "game" || gameNode["name"].Empty())
        continue;
      if (gameNode["name"].ValueAs<std::string>() != game.name)
        continue;

      for (auto &track : gameNode)
      {
        if (track.Key() != "track")
          continue;
        if (track["mpeg_rom_start_offset"].Empty())
        {
          ErrorLog("%s: Track in '%s' is missing 'mpeg_rom_start_offset' attribute and will be ignored.", music_filepath.c_str(), game.name.c_str());
          continue;
        }
        if (track["filepath"].Empty())
        {
          ErrorLog("%s: Track in '%s' is missing 'filepath' attribute and will be ignored.", music_filepath.c_str(), game.name.c_str());
          continue;
        }
        size_t startInFile = 0;
        if (track["file_start_offset"].Exists())
          startInFile = track["file_start_offset"].ValueAs<size_t>();
        const std::string path = track["filepath"].ValueAs<std::string>();
        const uint32_t romOffset = track["mpeg_rom_start_offset"].ValueAs<uint32_t>();

        if (s_replacements.count(romOffset) != 0)
        {
          ErrorLog("%s: Multiple tracks defined for '%s' MPEG ROM offset 0x%08x. Only the first will be used.", music_filepath.c_str(), game.name.c_str(), romOffset);
          continue;
        }

        if (files.count(path) == 0)
        {
          Replacement contents;
          if (!ReadWholeFile(path, contents))
            continue;
          files[path] = contents;
          InfoLog("Loaded custom track: %s.", path.c_str());
          printf("Loaded custom track: %s.\n", path.c_str());
        }

        Replacement r = files[path];
        r.startInFile = startInFile;
        s_replacements[romOffset] = r;
      }
    }
  }
}

void MpegDec::SetMemory(const uint8_t *data, int offset, int length, bool loop)
{
  mp3dec_init(&s_stream.decoder);

  auto it = s_replacements.find((uint32_t)offset);
  if (it == s_replacements.end())
  {
    s_stream.data = data + offset;
    s_stream.size = length;
    s_stream.owner = nullptr;
  }
  else
  {
    const Replacement &r = it->second;
    s_stream.data = r.data.get() + r.Start();
    s_stream.size = (int)(r.size - r.Start());
    s_stream.owner = r.data;
  }

  s_stream.pos = 0;
  s_stream.frameSamples = 0;
  s_stream.frameOut = 0;
  s_stream.loop = loop;
  s_stream.stopped = false;
}

void MpegDec::UpdateMemory(const uint8_t *data, int offset, int length, bool loop)
{
  // The position stays on the same byte of the stream; it is kept relative
  // to the new start.
  auto it = s_replacements.find((uint32_t)offset);
  if (it == s_replacements.end())
  {
    const uint8_t *start = data + offset;
    s_stream.pos -= Distance(start, s_stream.data);
    s_stream.data = start;
    s_stream.size = length;
  }
  else if (it->second.data == s_stream.owner)
  {
    // The replacement file that is playing: a game's loop points do not
    // match the file, so the loop always runs to its end.
    const Replacement &r = it->second;
    const uint8_t *start = r.data.get() + r.Start();
    s_stream.pos -= Distance(start, s_stream.data);
    s_stream.data = start;
    s_stream.size = (int)(r.size - r.Start());
  }
  // (a replacement for another file than the one playing: left alone)

  s_stream.loop = loop;
}

int MpegDec::GetPosition()
{
  return s_stream.pos;
}

void MpegDec::SetPosition(int pos)
{
  s_stream.pos = pos;
}

void MpegDec::Stop()
{
  s_stream.stopped = true;
}

bool MpegDec::IsLoaded()
{
  return s_stream.data != nullptr;
}

void MpegDec::DecodeAudio(int16_t *left, int16_t *right, int numStereoSamples)
{
  int count = numStereoSamples;
  if (s_stream.stopped || s_stream.data == nullptr)
    OutputSilence(left, right, count);

  OutputDecoded(left, right, count);   // what is left of the last frame

  while (count > 0)
  {
    s_stream.frameSamples = mp3dec_decode_frame(&s_stream.decoder, s_stream.data + s_stream.pos, s_stream.size - s_stream.pos, s_stream.pcm, &s_stream.frame);
    s_stream.pos += s_stream.frame.frame_bytes;
    s_stream.frameOut = 0;
    OutputDecoded(left, right, count);

    if (s_stream.pos >= s_stream.size - kHeaderBytes)
    {
      if (s_stream.loop)
        s_stream.pos = 0;
      else
        OutputSilence(left, right, count);
    }
  }
}
