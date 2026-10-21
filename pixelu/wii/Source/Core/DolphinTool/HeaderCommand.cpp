// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinTool/HeaderCommand.h"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <OptionParser.h>
#include <fmt/ostream.h>
#include <picojson.h>

#include "Common/CommonTypes.h"
#include "DiscIO/Blob.h"
#include "DiscIO/Enums.h"
#include "DiscIO/Volume.h"

namespace DolphinTool
{
namespace
{
// Everything the command can report, read once.
struct ImageDetails
{
  // Container.
  u64 block_size = 0;  // 0 when the format has no blocks.
  std::string compression_method;
  std::optional<int> compression_level;

  // Game, if the image holds a volume.
  bool has_volume = false;
  std::string internal_name;
  std::optional<u16> revision;
  std::string game_id;
  std::optional<u64> title_id;
  std::string region;
  std::string country;
};

ImageDetails ReadDetails(const DiscIO::BlobReader& reader, const DiscIO::Volume* volume)
{
  ImageDetails details;
  details.block_size = reader.GetBlockSize();
  details.compression_method = reader.GetCompressionMethod();
  details.compression_level = reader.GetCompressionLevel();
  if (volume != nullptr)
  {
    details.has_volume = true;
    details.internal_name = volume->GetInternalName();
    details.revision = volume->GetRevision();
    details.game_id = volume->GetGameID();
    details.title_id = volume->GetTitleID();
    details.region = DiscIO::GetName(volume->GetRegion(), false);
    details.country = DiscIO::GetName(volume->GetCountry(), false);
  }
  return details;
}

// Numbers are JSON doubles, so very large title IDs lose precision (as they always have).
void PrintJson(const ImageDetails& d)
{
  picojson::object out;
  if (d.block_size != 0)
    out["block_size"] = picojson::value(static_cast<double>(d.block_size));
  if (!d.compression_method.empty())
    out["compression_method"] = picojson::value(d.compression_method);
  if (d.compression_level)
    out["compression_level"] = picojson::value(static_cast<double>(*d.compression_level));

  if (d.has_volume)
  {
    out["internal_name"] = picojson::value(d.internal_name);
    if (d.revision)
      out["revision"] = picojson::value(static_cast<double>(*d.revision));
    out["game_id"] = picojson::value(d.game_id);
    if (d.title_id)
      out["title_id"] = picojson::value(static_cast<double>(*d.title_id));
    out["region"] = picojson::value(d.region);
    out["country"] = picojson::value(d.country);
  }

  std::cout << picojson::value(out) << '\n';
}

void PrintHumanReadable(const ImageDetails& d)
{
  if (d.block_size != 0)
    fmt::print(std::cout, "Block Size: {}\n", d.block_size);
  if (!d.compression_method.empty())
    fmt::print(std::cout, "Compression Method: {}\n", d.compression_method);
  if (d.compression_level)
    fmt::print(std::cout, "Compression Level: {}\n", *d.compression_level);

  if (!d.has_volume)
    return;

  fmt::print(std::cout, "Internal Name: {}\n", d.internal_name);
  if (d.revision)
    fmt::print(std::cout, "Revision: {}\n", *d.revision);
  fmt::print(std::cout, "Game ID: {}\n", d.game_id);
  if (d.title_id)
    fmt::print(std::cout, "Title ID: {:016x}\n", *d.title_id);
  fmt::print(std::cout, "Region: {}\n", d.region);
  fmt::print(std::cout, "Country: {}\n", d.country);
}

// One bare value per line, "N/A" where the container has none.
void PrintSelected(const ImageDetails& d, bool block_size, bool method, bool level)
{
  if (block_size)
  {
    if (d.block_size == 0)
      fmt::print(std::cout, "N/A\n");
    else
      fmt::print(std::cout, "{}\n", d.block_size);
  }
  if (method)
  {
    if (d.compression_method.empty())
      fmt::print(std::cout, "N/A\n");
    else
      fmt::print(std::cout, "{}\n", d.compression_method);
  }
  if (level)
  {
    if (!d.compression_level)
      fmt::print(std::cout, "N/A\n");
    else
      fmt::print(std::cout, "{}\n", *d.compression_level);
  }
}
}  // namespace

int HeaderCommand(const std::vector<std::string>& args)
{
  optparse::OptionParser parser;
  parser.usage("usage: header [options]...");
  parser.add_option("-i", "--input")
      .type("string")
      .action("store")
      .help("Path to disc image FILE.")
      .metavar("FILE");
  parser.add_option("-j", "--json")
      .action("store_true")
      .help("Optional. Print the information as JSON, then exit. Overrides other print options.");
  parser.add_option("-b", "--block_size")
      .action("store_true")
      .help("Optional. Print the block size of GCZ/WIA/RVZ formats, then exit.");
  parser.add_option("-c", "--compression")
      .action("store_true")
      .help("Optional. Print the compression method of GCZ/WIA/RVZ formats, then exit.");
  parser.add_option("-l", "--compression_level")
      .action("store_true")
      .help("Optional. Print the level of compression for WIA/RVZ formats, then exit.");

  const optparse::Values& options = parser.parse_args(args);

  const std::string& input = options["input"];
  if (input.empty())
  {
    fmt::print(std::cerr, "Error: No input set\n");
    return EXIT_FAILURE;
  }

  const bool want_json = options.is_set_by_user("json");
  const bool want_block_size = options.is_set_by_user("block_size");
  const bool want_method = options.is_set_by_user("compression");
  const bool want_level = options.is_set_by_user("compression_level");

  const std::unique_ptr<DiscIO::BlobReader> reader = DiscIO::CreateBlobReader(input);
  if (!reader)
  {
    fmt::print(std::cerr, "Error: Unable to open disc image\n");
    return EXIT_FAILURE;
  }
  const std::unique_ptr<DiscIO::Volume> volume = DiscIO::CreateVolume(reader->CopyReader());
  const ImageDetails details = ReadDetails(*reader, volume.get());

  if (want_json)
    PrintJson(details);
  else if (want_block_size || want_method || want_level)
    PrintSelected(details, want_block_size, want_method, want_level);
  else
    PrintHumanReadable(details);

  return EXIT_SUCCESS;
}
}  // namespace DolphinTool
