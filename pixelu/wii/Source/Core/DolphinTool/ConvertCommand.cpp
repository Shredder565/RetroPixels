// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinTool/ConvertCommand.h"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <OptionParser.h>
#include <fmt/ostream.h>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "DiscIO/Blob.h"
#include "DiscIO/DiscUtils.h"
#include "DiscIO/ScrubbedBlob.h"
#include "DiscIO/Volume.h"
#include "DiscIO/WIABlob.h"
#include "UICommon/UICommon.h"

namespace DolphinTool
{
namespace
{
using DiscIO::BlobType;
using DiscIO::WIARVZCompressionType;

// Value names accepted on the command line.
constexpr std::pair<std::string_view, BlobType> FORMAT_NAMES[] = {
    {"iso", BlobType::PLAIN},
    {"gcz", BlobType::GCZ},
    {"wia", BlobType::WIA},
    {"rvz", BlobType::RVZ},
};

constexpr std::pair<std::string_view, WIARVZCompressionType> COMPRESSION_NAMES[] = {
    {"none", WIARVZCompressionType::None},   {"purge", WIARVZCompressionType::Purge},
    {"bzip2", WIARVZCompressionType::Bzip2}, {"lzma", WIARVZCompressionType::LZMA},
    {"lzma2", WIARVZCompressionType::LZMA2}, {"zstd", WIARVZCompressionType::Zstd},
};

template <typename T, std::size_t N>
std::optional<T> Lookup(const std::pair<std::string_view, T> (&table)[N], std::string_view name)
{
  for (const auto& [key, value] : table)
  {
    if (key == name)
      return value;
  }
  return std::nullopt;
}

void Warn(std::string_view text)
{
  fmt::print(std::cerr, "Warning: {} Continuing anyway.\n", text);
}

int Fail(std::string_view text)
{
  fmt::print(std::cerr, "Error: {}\n", text);
  return EXIT_FAILURE;
}

optparse::OptionParser MakeParser()
{
  optparse::OptionParser parser;
  parser.usage("usage: convert [options]... [FILE]...");

  parser.add_option("-u", "--user")
      .type("string")
      .action("store")
      .help("User folder path, required for temporary processing files. "
            "Will be automatically created if this option is not set.")
      .set_default("");
  parser.add_option("-i", "--input")
      .type("string")
      .action("store")
      .help("Path to disc image FILE.")
      .metavar("FILE");
  parser.add_option("-o", "--output")
      .type("string")
      .action("store")
      .help("Path to the destination FILE.")
      .metavar("FILE");
  parser.add_option("-f", "--format")
      .type("string")
      .action("store")
      .help("Container format to use. Default is RVZ. [%choices]")
      .choices({"iso", "gcz", "wia", "rvz"});
  parser.add_option("-s", "--scrub")
      .action("store_true")
      .help("Scrub junk data as part of conversion.");
  parser.add_option("-b", "--block_size")
      .type("int")
      .action("store")
      .help("Block size for GCZ/WIA/RVZ formats, as an integer. Suggested value for RVZ: 131072 "
            "(128 KiB)");
  parser.add_option("-c", "--compression")
      .type("string")
      .action("store")
      .help("Compression method to use when converting to WIA/RVZ. Suggested value for RVZ: zstd "
            "[%choices]")
      .choices({"none", "zstd", "bzip2", "lzma", "lzma2"});
  parser.add_option("-l", "--compression_level")
      .type("int")
      .action("store")
      .help("Level of compression for the selected method. Ignored if 'none'. Suggested value for "
            "zstd: 5");
  return parser;
}

std::optional<int> IntOption(const optparse::Values& values, const char* name)
{
  if (!values.is_set(name))
    return std::nullopt;
  return static_cast<int>(values.get(name));
}

// The GCZ header's sub-type: 0 for GameCube, 1 for Wii, all ones for anything else.
u32 GCZSubType(const DiscIO::Volume* volume)
{
  if (volume != nullptr)
  {
    if (volume->GetVolumeType() == DiscIO::Platform::GameCubeDisc)
      return 0;
    if (volume->GetVolumeType() == DiscIO::Platform::WiiDisc)
      return 1;
  }
  return std::numeric_limits<u32>::max();
}
}  // namespace

int ConvertCommand(const std::vector<std::string>& args)
{
  optparse::OptionParser parser = MakeParser();
  const optparse::Values& options = parser.parse_args(args);

  // The user folder must exist before anything else: conversions write temporary files there,
  // and a missing one could send them to an unexpected place.
  UICommon::SetUserDirectory(options["user"]);
  UICommon::Init();

  if (!options.is_set("input"))
    return Fail("No input set");
  const std::string& input = options["input"];

  if (!options.is_set("output"))
    return Fail("No output set");
  const std::string& output = options["output"];

  const std::optional<BlobType> chosen_format = Lookup(FORMAT_NAMES, options["format"]);
  if (!chosen_format)
    return Fail("No output format set");
  const BlobType format = *chosen_format;

  std::unique_ptr<DiscIO::BlobReader> reader = DiscIO::CreateBlobReader(input);
  if (!reader)
    return Fail("The input file could not be opened.");

  const bool scrub = static_cast<bool>(options.get("scrub"));

  // Non-disc input can still be converted, just not scrubbed.
  const std::unique_ptr<DiscIO::Volume> volume = DiscIO::CreateDisc(input);
  if (!volume)
  {
    if (scrub)
      return Fail("Scrubbing is only supported for GC/Wii disc images.");
    Warn("The input file is not a GC/Wii disc image.");
  }

  if (scrub)
  {
    if (volume->IsDatelDisc())
      return Fail("Scrubbing a Datel disc is not supported.");

    reader = DiscIO::ScrubbedBlob::Create(input);
    if (!reader)
      return Fail("Unable to process disc image. Try again without --scrub.");

    if (format == BlobType::RVZ)
      Warn("Scrubbing an RVZ container does not offer significant space advantages.");
    else if (format == BlobType::PLAIN)
      Warn("Scrubbing does not save space when converting to ISO unless using external "
           "compression.");
  }
  else if (format == BlobType::GCZ && volume &&
           volume->GetVolumeType() == DiscIO::Platform::WiiDisc && !volume->IsDatelDisc())
  {
    Warn("Converting Wii disc images to GCZ without scrubbing may not offer space advantages over "
         "ISO.");
  }

  if (volume && volume->IsNKit())
    Warn("Converting an NKit file, output will still be NKit!");

  // Block size: required by every format except ISO.
  const std::optional<int> block_size = IntOption(options, "block_size");
  if (format != BlobType::PLAIN)
  {
    if (!block_size)
      return Fail("Block size must be set for GCZ/RVZ/WIA");
    if (!DiscIO::IsDiscImageBlockSizeValid(*block_size, format))
      return Fail("Block size is not valid for this format");

    if (*block_size < DiscIO::PREFERRED_MIN_BLOCK_SIZE ||
        *block_size > DiscIO::PREFERRED_MAX_BLOCK_SIZE)
    {
      Warn("Block size is not ideal for performance.");
    }

    if (format == BlobType::GCZ && volume &&
        !DiscIO::IsGCZBlockSizeLegacyCompatible(*block_size, volume->GetDataSize()))
    {
      Warn("For GCZs to be compatible with Dolphin < 5.0-11893, the file size must be an integer "
           "multiple of the block size and must not be an integer multiple of the block size "
           "multiplied by 32.");
    }
  }

  // Compression method and level: WIA and RVZ only.
  const std::optional<WIARVZCompressionType> compression =
      Lookup(COMPRESSION_NAMES, options["compression"]);
  std::optional<int> level = IntOption(options, "compression_level");

  const bool wia_family = format == BlobType::WIA || format == BlobType::RVZ;
  if (wia_family)
  {
    if (!compression)
      return Fail("Compression method must be set for WIA or RVZ");

    // WIA predates Zstandard, and RVZ has no use for Purge.
    const bool unsupported = format == BlobType::WIA ? *compression == WIARVZCompressionType::Zstd :
                                                       *compression == WIARVZCompressionType::Purge;
    if (unsupported)
      return Fail("Compression type is not supported for the container format");

    if (*compression == WIARVZCompressionType::None)
    {
      level = 0;
    }
    else
    {
      if (!level)
        return Fail("Compression level must be set when compression type is not 'none'");

      const auto [lowest, highest] = DiscIO::GetAllowedCompressionLevels(*compression, false);
      if (*level < lowest || *level > highest)
        return Fail("Compression level not in acceptable range");
    }
  }

  // Progress is not shown.
  const auto ignore_progress = [](const std::string&, float) { return true; };

  bool converted = false;
  if (format == BlobType::PLAIN)
  {
    converted = DiscIO::ConvertToPlain(reader.get(), input, output, ignore_progress);
  }
  else if (format == BlobType::GCZ)
  {
    converted = DiscIO::ConvertToGCZ(reader.get(), input, output, GCZSubType(volume.get()),
                                     *block_size, ignore_progress);
  }
  else if (wia_family)
  {
    converted = DiscIO::ConvertToWIAOrRVZ(reader.get(), input, output, format == BlobType::RVZ,
                                          *compression, *level, *block_size, ignore_progress);
  }
  else
  {
    ASSERT(false);
  }

  if (!converted)
    return Fail("Conversion failed");
  return EXIT_SUCCESS;
}
}  // namespace DolphinTool
