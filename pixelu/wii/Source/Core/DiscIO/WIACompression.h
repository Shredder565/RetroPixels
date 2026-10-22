// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

// The compression layers used inside WIA and RVZ files (an internal part of WIABlob).

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

#include "Common/CommonTypes.h"

namespace DiscIO
{
enum class WIARVZCompressionType : u32;

// ---------------------------------------------------------------------------------------------
// Decoding. A decoder is fed a group's compressed bytes as they're loaded from the file and
// writes into a fixed output area; each Step() consumes and produces as much as it can.

struct DecodeInput
{
  const u8* data = nullptr;
  size_t loaded = 0;  // bytes of data available so far
  size_t total = 0;   // bytes the group's compressed data has in all
  size_t pos = 0;     // bytes consumed; advanced by the decoder
};

struct DecodeOutput
{
  u8* data = nullptr;
  size_t capacity = 0;
  size_t pos = 0;  // bytes produced; advanced by the decoder
};

class Decoder
{
public:
  virtual ~Decoder();

  // False on corrupt data.
  virtual bool Step(DecodeInput& in, DecodeOutput& out) = 0;
  // True once the end of the compressed stream has been reached.
  virtual bool Finished() const { return m_finished; }

protected:
  bool m_finished = false;
};

// For Purge, `purge_output_size` is the size the data decompresses to. The compressor data is
// what the file's header stores for LZMA and LZMA2.
std::unique_ptr<Decoder> MakeDecoder(WIARVZCompressionType type, const u8* compressor_data,
                                     size_t compressor_data_size, u64 purge_output_size);

// Undoes RVZ packing on top of `inner`'s output. `already_decoded` holds output of `inner`
// that was produced before packing was known to apply. `data_offset` is the offset of the
// group's data in its partition (or on the disc), which the junk generator needs.
std::unique_ptr<Decoder> MakeRVZUnpacker(std::unique_ptr<Decoder> inner,
                                         std::vector<u8> already_decoded, u64 data_offset,
                                         u32 packed_size);

// ---------------------------------------------------------------------------------------------
// Encoding. Call Begin, then optionally HashOnly, then Feed, then Finish; afterwards Data() and
// Size() give the result.

class Encoder
{
public:
  virtual ~Encoder();

  virtual bool Begin(std::optional<u64> input_size) = 0;
  // Purge includes data stored in front of the compressed data (the hash exception lists) in
  // its checksum; other encoders ignore this.
  virtual bool HashOnly(const u8* data, size_t size) { return true; }
  virtual bool Feed(const u8* data, size_t size) = 0;
  virtual bool Finish() = 0;

  virtual const u8* Data() const = 0;
  virtual size_t Size() const = 0;
};

// Null for WIARVZCompressionType::None. For LZMA and LZMA2, the compressor data the header
// needs is written to the two out parameters if they are non-null (up to 7 bytes).
std::unique_ptr<Encoder> MakeEncoder(WIARVZCompressionType type, int level,
                                     u8* compressor_data_out, u8* compressor_data_size_out);

}  // namespace DiscIO
