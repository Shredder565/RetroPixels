// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinTool/VerifyCommand.h"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <OptionParser.h>
#include <fmt/format.h>
#include <fmt/ostream.h>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Core/AchievementManager.h"
#include "DiscIO/Volume.h"
#include "DiscIO/VolumeVerifier.h"
#include "UICommon/UICommon.h"

namespace DolphinTool
{
namespace
{
using Verifier = DiscIO::VolumeVerifier;

std::string ToHex(const std::vector<u8>& digest)
{
  std::string hex;
  for (const u8 byte : digest)
    hex += fmt::format("{:02x}", byte);
  return hex;
}

void PrintDigestLine(std::string_view name, const std::vector<u8>& digest)
{
  if (digest.empty())
    fmt::print(std::cout, "{} not computed\n", name);
  else
    fmt::print(std::cout, "{}: {}\n", name, ToHex(digest));
}

const char* SeverityName(Verifier::Severity severity)
{
  switch (severity)
  {
  case Verifier::Severity::None:
    return "None";
  case Verifier::Severity::Low:
    return "Low";
  case Verifier::Severity::Medium:
    return "Medium";
  case Verifier::Severity::High:
    return "High";
  }
  ASSERT(false);
  return "";
}

void PrintReport(const Verifier::Result& result)
{
  PrintDigestLine("CRC32", result.hashes.crc32);
  PrintDigestLine("MD5", result.hashes.md5);
  PrintDigestLine("SHA1", result.hashes.sha1);

  fmt::print(std::cout, "Problems Found: {}\n", result.problems.empty() ? "No" : "Yes");
  for (const Verifier::Problem& problem : result.problems)
  {
    fmt::print(std::cout, "\nSeverity: {}", SeverityName(problem.severity));
    fmt::print(std::cout, "\nSummary: {}\n\n", problem.text);
  }
}
}  // namespace

int VerifyCommand(const std::vector<std::string>& args)
{
  optparse::OptionParser parser;
  parser.usage("usage: verify [options]...");
  parser.add_option("-u", "--user")
      .type("string")
      .action("store")
      .help("User folder path, required for temporary processing files. "
            "Will be automatically created if this option is not set.")
      .set_default("");
  parser.add_option("-i", "--input")
      .type("string")
      .action("store")
      .help("Path to input file.")
      .metavar("FILE");
  parser.add_option("-a", "--algorithm")
      .type("string")
      .action("store")
      .help("Optional. Compute and print the digest using the selected algorithm, then exit. "
            "[%choices]")
      .choices({"crc32", "md5", "sha1", "rchash"});

  const optparse::Values& options = parser.parse_args(args);

  // Set up the user folder first; verification may need temporary files in it.
  UICommon::SetUserDirectory(options["user"]);
  UICommon::Init();

  if (!options.is_set("input"))
  {
    fmt::print(std::cerr, "Error: No input set\n");
    return EXIT_FAILURE;
  }
  const std::string& input = options["input"];

  // Without --algorithm: the verifier's default digests and a full report. With it: that one
  // digest, printed bare. "rchash" (the RetroAchievements hash) needs achievements support.
  const bool single_algorithm = options.is_set("algorithm");
  DiscIO::Hashes<bool> wanted{};
  bool want_rc_hash = false;
  if (!single_algorithm)
  {
    wanted = Verifier::GetDefaultHashesToCalculate();
  }
  else
  {
    const std::string& algorithm = options["algorithm"];
    wanted.crc32 = algorithm == "crc32";
    wanted.md5 = algorithm == "md5";
    wanted.sha1 = algorithm == "sha1";
#ifdef USE_RETRO_ACHIEVEMENTS
    want_rc_hash = algorithm == "rchash";
#endif
  }

  if (!wanted.crc32 && !wanted.md5 && !wanted.sha1 && !want_rc_hash)
  {
    fmt::print(std::cerr, "Error: No algorithms selected for the operation\n");
    return EXIT_FAILURE;
  }

  const std::unique_ptr<DiscIO::Volume> volume = DiscIO::CreateVolume(input);
  if (!volume)
  {
    fmt::print(std::cerr, "Error: Unable to open input file\n");
    return EXIT_FAILURE;
  }

  // The verifier runs even when only the RetroAchievements hash was asked for.
  Verifier verifier(*volume, false, wanted);
  verifier.Start();
  while (verifier.GetBytesProcessed() != verifier.GetTotalBytes())
    verifier.Process();
  verifier.Finish();
  const Verifier::Result& result = verifier.GetResult();

  std::string rc_hash = "0";
#ifdef USE_RETRO_ACHIEVEMENTS
  if (want_rc_hash)
    rc_hash = AchievementManager::CalculateHash(input);
#endif

  if (!single_algorithm)
  {
    PrintReport(result);
    return EXIT_SUCCESS;
  }

  const std::vector<u8>* digest = nullptr;
  if (wanted.crc32 && !result.hashes.crc32.empty())
    digest = &result.hashes.crc32;
  else if (wanted.md5 && !result.hashes.md5.empty())
    digest = &result.hashes.md5;
  else if (wanted.sha1 && !result.hashes.sha1.empty())
    digest = &result.hashes.sha1;

  if (digest != nullptr)
  {
    fmt::print(std::cout, "{}\n", ToHex(*digest));
  }
  else if (want_rc_hash)
  {
    fmt::print(std::cout, "{}\n", rc_hash);
  }
  else
  {
    fmt::print(std::cerr, "Error: No hash computed\n");
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
}  // namespace DolphinTool
