// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinTool/ExtractCommand.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <fmt/ostream.h>

#include <OptionParser.h>

#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "Common/StringUtil.h"

#include "DiscIO/Blob.h"
#include "DiscIO/DiscExtractor.h"
#include "DiscIO/DiscUtils.h"
#include "DiscIO/Filesystem.h"
#include "DiscIO/Volume.h"

namespace DolphinTool
{
namespace
{
struct ExtractSettings
{
  std::string output;       // Destination folder.
  std::string single_path;  // Path inside the disc, when only one file or folder is wanted.
  bool single = false;
  bool quiet = false;
};

// The file or folder at `path` in the partition's filesystem; null if it has none or no such entry.
std::unique_ptr<DiscIO::FileInfo>
FindEntry(const DiscIO::Volume& volume, const DiscIO::Partition& partition, const std::string& path)
{
  const DiscIO::FileSystem* filesystem = volume.GetFileSystem(partition);
  return filesystem ? filesystem->FindFileInfo(path) : nullptr;
}

// Checks that the volume has a disc filesystem, explaining why not otherwise.
bool HasDiscFilesystem(const DiscIO::Volume& volume)
{
  const DiscIO::Platform platform = volume.GetVolumeType();
  if (platform == DiscIO::Platform::GameCubeDisc || platform == DiscIO::Platform::WiiDisc)
    return true;

  if (platform == DiscIO::Platform::WiiWAD)
    fmt::println(std::cerr, "Error: Wii WADs are not supported.");
  else if (platform == DiscIO::Platform::ELFOrDOL)
    fmt::println(std::cerr,
                 "Error: *.elf or *.dol have no filesystem and are therefore not supported.");
  else
    fmt::println(std::cerr, "Error: Unknown volume type.");
  return false;
}

// Writes the folder at `path` (which must exist) and everything below it into `destination`,
// reporting each file unless quiet.
void WriteFolder(const DiscIO::Volume& volume, const DiscIO::Partition& partition,
                 const std::string& path, const std::string& destination, bool quiet)
{
  const DiscIO::FileSystem* filesystem = volume.GetFileSystem(partition);
  if (!filesystem)
    return;

  const std::unique_ptr<DiscIO::FileInfo> folder = filesystem->FindFileInfo(path);
  const u32 total = folder->GetTotalChildren();
  u32 done = 0;
  DiscIO::ExportDirectory(
      volume, partition, *folder, true, "", destination,
      [&done, total, quiet](const std::string& name) {
        ++done;
        const float percent = static_cast<float>(done) / static_cast<float>(total) * 100;
        if (!quiet)
        {
          fmt::println(std::cerr, "Extracting: {} | {}%", name, static_cast<int>(percent));
        }
        return false;  // Never cancel.
      });
}

void WriteFile(const DiscIO::Volume& volume, const DiscIO::Partition& partition,
               const std::string& path, const std::string& destination)
{
  const DiscIO::FileSystem* filesystem = volume.GetFileSystem(partition);
  if (filesystem)
    DiscIO::ExportFile(volume, partition, filesystem->FindFileInfo(path).get(), destination);
}

// Extracts into <output>/<partition name>/: everything (files/ plus the system data), or just
// the requested file or folder under files/.
bool ExtractFromPartition(const DiscIO::Volume& volume, const DiscIO::Partition& partition,
                          const std::string& partition_name, const ExtractSettings& settings)
{
  const std::string base = settings.output + "/" + partition_name + "/";

  if (!settings.single)
  {
    WriteFolder(volume, partition, "", base + "/files", settings.quiet);
    DiscIO::ExportSystemData(volume, partition, base);
    return true;
  }

  const std::unique_ptr<DiscIO::FileInfo> entry =
      FindEntry(volume, partition, settings.single_path);
  if (!entry)
    return false;

  std::string target = base + "files/" + settings.single_path;
  File::CreateFullPath(target);
  if (entry->IsDirectory())
  {
    // Exported into the parent, which recreates the folder itself.
    target = PathToString(StringToPath(target).remove_filename());
    WriteFolder(volume, partition, settings.single_path, target, settings.quiet);
  }
  else
  {
    WriteFile(volume, partition, settings.single_path, target);
  }
  return true;
}

// Prints `path` (unless it is the root) and everything below it, one per line with folders
// ending in '/', to stdout and to `listing`.
void ListTree(const std::string& path, const DiscIO::FileInfo& entry, std::string* listing)
{
  if (!path.empty())
  {
    const std::string line = path + '\n';
    fmt::print("{}", line);
    listing->append(line);
  }

  for (const DiscIO::FileInfo& child : entry)
  {
    const std::string child_path = path + child.GetName() + (child.IsDirectory() ? "/" : "");
    ListTree(child_path, child, listing);
  }
}

bool ListPartition(const DiscIO::Volume& volume, const DiscIO::Partition& partition,
                   const std::string& partition_name, const std::string& path, std::string* listing)
{
  const DiscIO::FileSystem* filesystem = volume.GetFileSystem(partition);
  if (!filesystem)
  {
    fmt::println(std::cerr, "Warning: partition {} has no filesystem.", partition_name);
    return false;
  }

  const std::unique_ptr<DiscIO::FileInfo> entry = filesystem->FindFileInfo(path);
  if (!entry)
  {
    // Partitionless images stay silent here.
    if (!partition_name.empty())
      fmt::println(std::cerr, "Warning: {} does not exist in this partition.", path);
    return false;
  }

  // Listed under its canonical path rather than as typed.
  ListTree(entry->GetPath(), *entry, listing);
  return true;
}

// Lists every partition (or only `wanted_partition`), each under a header line.
bool ListVolume(const DiscIO::Volume& volume, const std::string& path,
                const std::string& wanted_partition, std::string* listing)
{
  const std::vector<DiscIO::Partition> partitions = volume.GetPartitions();
  if (partitions.empty())
    return ListPartition(volume, DiscIO::PARTITION_NONE, wanted_partition, path, listing);

  bool listed_any = false;
  for (const DiscIO::Partition& partition : partitions)
  {
    const std::optional<u32> type = volume.GetPartitionType(partition);
    if (!type)
    {
      fmt::println(std::cerr, "Error: Could not get partition type.");
      return false;
    }

    const std::string name = DiscIO::NameForPartitionType(*type, true);
    if (!wanted_partition.empty() && !Common::CaseInsensitiveEquals(name, wanted_partition))
      continue;

    const std::string header = fmt::format("/// PARTITION: {} <{}> ///\n", name, path);
    fmt::print(std::cout, "{}", header);
    listing->append(header);

    if (ListPartition(volume, partition, name, path, listing))
      listed_any = true;
  }
  return listed_any;
}

struct OptionSpec
{
  const char* short_flag;
  const char* long_flag;
  bool takes_value;     // A string value; otherwise a switch.
  const char* metavar;  // Null for the default.
  const char* help;
};

constexpr OptionSpec OPTIONS[] = {
    {"-i", "--input", true, "FILE", "Path to disc image FILE."},
    {"-o", "--output", true, "FOLDER", "Path to the destination FOLDER."},
    {"-p", "--partition", true, nullptr, "Which specific partition you want to extract."},
    {"-s", "--single", true, nullptr, "Which specific file/directory you want to extract."},
    {"-l", "--list", false, nullptr,
     "List all files in volume/partition. Will print the directory/file specified with --single "
     "if defined."},
    {"-q", "--quiet", false, nullptr, "Mute all messages except for errors."},
    {"-g", "--gameonly", false, nullptr, "Only extracts the DATA partition."},
};

int Failure(const char* message)
{
  fmt::println(std::cerr, "{}", message);
  return EXIT_FAILURE;
}
}  // namespace

int Extract(const std::vector<std::string>& args)
{
  optparse::OptionParser parser;
  parser.usage("usage: extract [options]...");
  for (const OptionSpec& spec : OPTIONS)
  {
    optparse::Option& option = parser.add_option(spec.short_flag, spec.long_flag);
    option.action(spec.takes_value ? "store" : "store_true");
    if (spec.takes_value)
      option.type("string");
    option.help(spec.help);
    if (spec.metavar != nullptr)
      option.metavar(spec.metavar);
  }

  const optparse::Values& options = parser.parse_args(args);

  const bool has_output = options.is_set("output");
  const bool list_mode = options.is_set("list");
  const bool partition_given = options.is_set("partition");

  ExtractSettings settings;
  settings.quiet = options.is_set("quiet");
  settings.single = options.is_set("single");

  if (!options.is_set("input"))
    return Failure("Error: No input image set");
  const std::string& input = options["input"];

  settings.output = options["output"];
  if (!has_output && !list_mode)
    return Failure("Error: No output folder set");

  settings.single_path = options["single"];
  // --gameonly is shorthand for --partition data.
  const std::string wanted_partition = options.is_set("gameonly") ? "data" : options["partition"];

  if (has_output && !list_mode)
    File::CreateDirs(settings.output);

  if (!DiscIO::CreateBlobReader(input))
    return Failure("Error: Unable to open disc image");

  const std::unique_ptr<DiscIO::Volume> volume = DiscIO::CreateVolume(input);
  if (!volume)
    return Failure("Error: Unable to open volume");
  if (!HasDiscFilesystem(*volume))
    return EXIT_FAILURE;

  if (list_mode)
  {
    // The listing goes to stdout, and also to the --output file if given.
    const std::string start = settings.single ? settings.single_path : "/";
    if (settings.quiet && !has_output)
    {
      return Failure("Error: --quiet is set but no output file provided. Please either remove the "
                     "--quiet flag or specify --output");
    }

    std::string listing;
    if (!ListVolume(*volume, start, wanted_partition, &listing))
      return Failure("Error: Found nothing to list");

    if (has_output)
    {
      File::CreateFullPath(settings.output);
      std::ofstream file;
      file.open(settings.output);
      if (!file.is_open())
        return Failure("Error: Unable to open output file");
      file << listing;
    }
    return EXIT_SUCCESS;
  }

  bool extracted = false;
  const std::vector<DiscIO::Partition> partitions = volume->GetPartitions();
  if (partitions.empty())
  {
    if (partition_given)
    {
      fmt::println(std::cerr, "Warning: --partition has a value even though this image doesn't "
                              "have any partitions.");
    }
    extracted = ExtractFromPartition(*volume, DiscIO::PARTITION_NONE, "", settings);
  }
  else
  {
    for (const DiscIO::Partition& partition : partitions)
    {
      // Partitions of unknown type are skipped.
      const std::optional<u32> type = volume->GetPartitionType(partition);
      if (!type)
        continue;

      const std::string name = DiscIO::NameForPartitionType(*type, true);
      if (!wanted_partition.empty() && !Common::CaseInsensitiveEquals(wanted_partition, name))
        continue;

      if (ExtractFromPartition(*volume, partition, name, settings))
        extracted = true;
    }
  }

  if (!extracted)
  {
    fmt::print(std::cerr, "{}",
               settings.single ? "Error: No file/folder was extracted." :
                                 "Error: No partitions were extracted.");
    if (partition_given)
      fmt::println(std::cerr, " Maybe you misspelled your specified partition?");
    fmt::println(std::cerr, "\n");
    return EXIT_FAILURE;
  }

  if (!settings.quiet)
    fmt::println(std::cerr, "Finished Successfully!");
  return EXIT_SUCCESS;
}
}  // namespace DolphinTool
