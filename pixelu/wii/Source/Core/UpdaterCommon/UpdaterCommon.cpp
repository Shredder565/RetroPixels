// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "UpdaterCommon/UpdaterCommon.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdarg>
#include <cstdlib>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <OptionParser.h>
#include <ed25519.h>
#include <fmt/format.h>
#include <mbedtls/base64.h>
#include <mbedtls/sha256.h>
#include <zlib.h>

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/HttpRequest.h"
#include "Common/IOFile.h"
#include "Common/StringUtil.h"
#include "UpdaterCommon/Platform.h"
#include "UpdaterCommon/UI.h"

#ifdef _WIN32
#include <windows.h>

#include <filesystem>

#include "Common/CommonFuncs.h"
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

// Ed25519 public keys that update manifests are signed with: the release key, and the key of the
// updater test server (used when UI::IsTestMode()). Kept byte for byte.
const std::array<u8, 32> UPDATE_PUB_KEY = {
    0x2a, 0xb3, 0xd1, 0xdc, 0x6e, 0xf5, 0x07, 0xf6, 0xa0, 0x6c, 0x7c, 0x54, 0xdf, 0x54, 0xf4, 0x42,
    0x80, 0xa6, 0x28, 0x8b, 0x6d, 0x70, 0x14, 0xb5, 0x4c, 0x34, 0x95, 0x20, 0x4d, 0xd4, 0xd3, 0x5d};
const std::array<u8, 32> UPDATE_PUB_KEY_TEST = {
    0x0c, 0x5f, 0xdc, 0xd1, 0x15, 0x71, 0xfb, 0x86, 0x4f, 0x9e, 0x6d, 0xe6, 0x65, 0x39, 0x43, 0xe1,
    0x9e, 0xe0, 0x9b, 0x28, 0xc9, 0x1a, 0x60, 0xb7, 0x67, 0x1c, 0xf3, 0xf6, 0xca, 0x1b, 0xdd, 0x1a};

namespace
{
File::IOFile s_log;

void CloseLog()
{
  s_log.Flush();
  s_log.Close();
}

void Fail(const std::string& message)
{
  LogToFile("%s\n", message.c_str());
  UI::SetVisible(true);
  UI::Error(message);
}

std::optional<u8> HexDigit(char c)
{
  if (c >= '0' && c <= '9')
    return static_cast<u8>(c - '0');
  if (c >= 'a' && c <= 'f')
    return static_cast<u8>(c - 'a' + 10);
  if (c >= 'A' && c <= 'F')
    return static_cast<u8>(c - 'A' + 10);
  return std::nullopt;
}

// Exactly 2 * out.size() hex digits, either case.
bool DecodeHex(std::string_view hex, std::span<u8> out)
{
  if (hex.size() != out.size() * 2)
    return false;

  for (std::size_t i = 0; i < out.size(); ++i)
  {
    const std::optional<u8> high = HexDigit(hex[i * 2]);
    const std::optional<u8> low = HexDigit(hex[i * 2 + 1]);
    if (!high || !low)
      return false;
    out[i] = static_cast<u8>((*high << 4) | *low);
  }
  return true;
}

std::string HashName(const Manifest::Hash& hash)
{
  return HexEncode(hash.data(), hash.size());
}

// Both manifests and content-store files are gzip streams.
std::optional<std::string> Gunzip(const std::string& compressed)
{
  z_stream stream{};
  stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(compressed.data()));
  stream.avail_in = static_cast<uInt>(compressed.size());
  // Window bits + 16 selects the gzip wrapper.
  inflateInit2(&stream, MAX_WBITS + 16);

  constexpr std::size_t CHUNK = 20 * 1024 * 1024;
  const auto chunk = std::make_unique_for_overwrite<char[]>(CHUNK);
  std::string inflated;
  int status;
  do
  {
    stream.next_out = reinterpret_cast<Bytef*>(chunk.get());
    stream.avail_out = static_cast<uInt>(CHUNK);
    status = inflate(&stream, Z_NO_FLUSH);
    inflated.append(chunk.get(), CHUNK - stream.avail_out);
  } while (status == Z_OK);
  inflateEnd(&stream);

  if (status != Z_STREAM_END)
  {
    LogToFile("Could not read the data as gzip: error %d.\n", status);
    return std::nullopt;
  }
  return inflated;
}

// One base64 signature line against the signed part of a manifest.
bool SignatureMatches(const std::string& signed_text, const std::string& encoded_signature)
{
  constexpr std::size_t SIGNATURE_SIZE = 64;
  std::array<u8, SIGNATURE_SIZE> signature{};
  std::size_t decoded_size = 0;
  const int result = mbedtls_base64_decode(signature.data(), signature.size(), &decoded_size,
                                           reinterpret_cast<const u8*>(encoded_signature.data()),
                                           encoded_signature.size());
  if (result != 0 || decoded_size != SIGNATURE_SIZE)
  {
    LogToFile("Invalid base64: %s\n", encoded_signature.c_str());
    return false;
  }

  const std::array<u8, 32>& key = UI::IsTestMode() ? UPDATE_PUB_KEY_TEST : UPDATE_PUB_KEY;
  return ed25519_verify(signature.data(), reinterpret_cast<const u8*>(signed_text.data()),
                        signed_text.size(), key.data()) != 0;
}

// The manifest body: lines of "<path>\t<32 hex digits>\n". A repeated path keeps its last hash.
std::optional<Manifest> ParseManifestText(const std::string& text)
{
  Manifest manifest;
  std::size_t line_start = 0;
  while (line_start < text.size())
  {
    const std::size_t entry = manifest.entries.size();
    const std::size_t tab = text.find('\t', line_start);
    if (tab == std::string::npos)
    {
      LogToFile("Manifest entry %zu: could not find filename end.\n", entry);
      return std::nullopt;
    }
    const std::size_t newline = text.find('\n', tab);
    if (newline == std::string::npos)
    {
      LogToFile("Manifest entry %zu: could not find hash end.\n", entry);
      return std::nullopt;
    }

    const std::string hex = text.substr(tab + 1, newline - tab - 1);
    Manifest::Hash hash{};
    if (hex.size() != 32 || !DecodeHex(hex, hash))
    {
      LogToFile("Manifest entry %zu: invalid hash: \"%s\".\n", entry, hex.c_str());
      return std::nullopt;
    }

    manifest.entries[text.substr(line_start, tab - line_start)] = hash;
    line_start = newline + 1;
  }
  return manifest;
}

// Downloads, unpacks and checks a manifest. After the body comes a blank line and then one
// base64 signature per line; any one valid signature is enough.
std::optional<Manifest> DownloadManifest(const std::string& url)
{
  Common::HttpRequest http;
  const Common::HttpRequest::Response reply = http.Get(url);
  if (!reply)
  {
    LogToFile("Manifest download failed.\n");
    return std::nullopt;
  }

  std::optional<std::string> text = Gunzip(std::string(reply->begin(), reply->end()));
  if (!text)
    return std::nullopt;

  const std::size_t blank_line = text->rfind("\n\n");
  if (blank_line == std::string::npos)
  {
    LogToFile("No signature was found in manifest.\n");
    return std::nullopt;
  }

  // The signed part is the body including its last newline.
  const std::vector<std::string> signatures = SplitString(text->substr(blank_line + 2), '\n');
  text->resize(blank_line + 1);

  const bool verified = std::ranges::any_of(signatures, [&text](const std::string& signature) {
    return SignatureMatches(*text, signature);
  });
  if (!verified)
  {
    LogToFile("Could not verify signature of the manifest.\n");
    return std::nullopt;
  }

  return ParseManifestText(*text);
}

TodoList PlanUpdate(const Manifest& current, const Manifest& next)
{
  TodoList todo;

  for (const auto& [filename, hash] : current.entries)
  {
    if (!next.entries.contains(filename))
      todo.to_delete.push_back({filename, hash});
  }

  for (const auto& [filename, hash] : next.entries)
  {
    std::optional<Manifest::Hash> old_hash;
    if (const auto found = current.entries.find(filename); found != current.entries.end())
      old_hash = found->second;

    if (old_hash == hash)
      continue;

    todo.to_download.push_back({filename, hash});
    todo.to_update.push_back({filename, old_hash, hash});
  }

  return todo;
}

// Content-store path of a content hash: "ab/cd/ef01...".
std::string ContentStorePath(const std::string& hash_name)
{
  return hash_name.substr(0, 2) + '/' + hash_name.substr(2, 2) + '/' + hash_name.substr(4);
}

bool FetchContent(std::span<const TodoList::DownloadOp> downloads, const std::string& store_url,
                  const std::string& temp_dir)
{
  Common::HttpRequest http(std::chrono::seconds(30), [](s64 total, s64 now, s64, s64) {
    UI::SetCurrentProgress(static_cast<int>(now), static_cast<int>(total));
    return true;
  });

  UI::SetTotalMarquee(false);

  const std::size_t count = downloads.size();
  for (std::size_t i = 0; i < count; ++i)
  {
    const TodoList::DownloadOp& download = downloads[i];
    UI::SetTotalProgress(static_cast<int>(i + 1), static_cast<int>(count));

    // Files with identical contents share one download.
    const std::string hash_name = HashName(download.hash);
    const std::string cached = temp_dir + DIR_SEP + hash_name;
    if (File::Exists(cached))
      continue;

    UI::SetDescription(
        fmt::format("Downloading {}... (File {} of {})", download.filename, i + 1, count));
    UI::SetCurrentMarquee(false);

    const std::string url = store_url + ContentStorePath(hash_name);
    LogToFile("Downloading %s ...\n", url.c_str());
    const Common::HttpRequest::Response reply = http.Get(url);
    if (!reply)
      return false;

    UI::SetCurrentMarquee(true);
    UI::SetDescription("Verifying " + download.filename + "...");

    const std::optional<std::string> contents = Gunzip(std::string(reply->begin(), reply->end()));
    if (!contents)
      return false;
    if (ComputeHash(*contents) != download.hash)
    {
      LogToFile("Wrong hash on downloaded content %s.\n", url.c_str());
      return false;
    }
    if (!File::WriteStringToFile(cached, *contents))
    {
      LogToFile("Could not write cache file %s.\n", cached.c_str());
      return false;
    }
  }
  return true;
}

bool MoveAside(const std::string& path)
{
  const std::string backup = path + ".bak";
  LogToFile("Backing up existing %s to .bak.\n", path.c_str());
  if (File::Rename(path, backup))
    return true;
  LogToFile("Could not rename %s to %s for backup.\n", path.c_str(), backup.c_str());
  return false;
}

// Hash of a file's contents; nullopt if it cannot be read.
std::optional<Manifest::Hash> HashOfFile(const std::string& path)
{
  std::string contents;
  if (!File::ReadFileToString(path, contents))
    return std::nullopt;
  return ComputeHash(contents);
}

bool WriteUpdatedFiles(std::span<const TodoList::UpdateOp> updates, const std::string& install_dir,
                       const std::string& temp_dir)
{
#ifdef _WIN32
  // Updating the updater itself: Windows lets a running executable be renamed but not
  // overwritten, so it is always moved aside to .bak (and that copy is left behind).
  const std::filesystem::path own_name =
      std::filesystem::path(Common::GetModuleName(nullptr).value()).filename();
#endif

  for (const TodoList::UpdateOp& op : updates)
  {
    const std::string target = install_dir + DIR_SEP + op.filename;
    if (!File::CreateFullPath(target))
    {
      LogToFile("Could not create directory structure for %s.\n", op.filename.c_str());
      return false;
    }

#ifndef _WIN32
    // The manifest has no permission bits, so an existing file's mode is carried over.
    std::optional<mode_t> mode;
#endif

    if (File::Exists(target))
    {
#ifdef _WIN32
      const bool replacing_self = std::filesystem::path(op.filename) == own_name;
#else
      struct stat info;
      if (lstat(target.c_str(), &info) != 0)
        continue;
      if (S_ISLNK(info.st_mode))
      {
        LogToFile("%s is symlink, skipping\n", target.c_str());
        continue;
      }
      mode = info.st_mode;
      const bool replacing_self = false;
#endif

      const std::optional<Manifest::Hash> existing = HashOfFile(target);
      if (!existing)
      {
        LogToFile("Could not read existing file %s.\n", op.filename.c_str());
        return false;
      }
      if (*existing == op.new_hash)
      {
        LogToFile("File %s was already up to date. Partial update?\n", op.filename.c_str());
        continue;
      }
      // Keep a copy of anything that is not the expected old version (local changes).
      if ((existing != op.old_hash || replacing_self) && !MoveAside(target))
        return false;
    }

    const std::string hash_name = HashName(op.new_hash);
    LogToFile("Updating file %s from content %s...\n", op.filename.c_str(), hash_name.c_str());
    if (!File::CopyRegularFile(temp_dir + DIR_SEP + hash_name, target))
    {
      LogToFile("Could not update file %s.\n", op.filename.c_str());
      return false;
    }

#ifndef _WIN32
    if (mode && chmod(target.c_str(), *mode) != 0)
      return false;
#endif
  }
  return true;
}

bool RemoveDroppedFiles(std::span<const TodoList::DeleteOp> deletions,
                        const std::string& install_dir)
{
  for (const TodoList::DeleteOp& op : deletions)
  {
    const std::string target = install_dir + DIR_SEP + op.filename;
    if (!File::Exists(target))
    {
      LogToFile("File %s is already missing.\n", op.filename.c_str());
      continue;
    }

    const std::optional<Manifest::Hash> existing = HashOfFile(target);
    if (!existing)
    {
      LogToFile("Could not read file planned for deletion: %s.\n", op.filename.c_str());
      return false;
    }
    // A locally changed file is kept as .bak.
    if (*existing != op.old_hash && !MoveAside(target))
      return false;

    File::Delete(target);
  }
  return true;
}

bool ApplyPlan(const TodoList& todo, const std::string& install_dir, const std::string& store_url,
               const std::string& temp_dir)
{
  LogToFile("Starting download step...\n");
  if (!FetchContent(todo.to_download, store_url, temp_dir))
    return false;
  LogToFile("Download step completed.\n");

  LogToFile("Starting platform version check step...\n");
  UI::SetDescription("Checking platform...");
  if (!Platform::VersionCheck(todo.to_update, install_dir, temp_dir))
    return false;
  LogToFile("Platform version check step completed.\n");

  LogToFile("Starting update step...\n");
  if (!WriteUpdatedFiles(todo.to_update, install_dir, temp_dir))
    return false;
  LogToFile("Update step completed.\n");

  LogToFile("Starting deletion step...\n");
  if (!RemoveDroppedFiles(todo.to_delete, install_dir))
    return false;
  LogToFile("Deletion step completed.\n");

  return true;
}

void RemoveDownloads(const std::string& temp_dir, const TodoList& todo)
{
  // Best effort only.
  for (const TodoList::DownloadOp& download : todo.to_download)
    File::Delete(temp_dir + DIR_SEP + HashName(download.hash));
  File::DeleteDir(temp_dir);
}

struct UpdaterArguments
{
  std::string this_manifest_url;
  std::string next_manifest_url;
  std::string content_store_url;
  std::string install_base_path;
  std::optional<std::string> binary_to_restart;
  std::optional<u32> parent_pid;
  std::optional<std::string> log_file;
};

struct FlagSpec
{
  const char* name;  // Also the destination key.
  const char* metavar;
  const char* help;
  bool is_int;
};

constexpr FlagSpec UPDATER_FLAGS[] = {
    {"this-manifest-url", "URL", "URL to the update manifest for the currently installed version.",
     false},
    {"next-manifest-url", "URL", "URL to the update manifest for the to-be-installed version.",
     false},
    {"content-store-url", "URL",
     "Base URL of the content store where files to download are stored.", false},
    {"install-base-path", "PATH", "Base path of the Dolphin install to be updated.", false},
    {"binary-to-restart", "PATH", "Binary to restart after the update is over.", false},
    {"log-file", "PATH", "File where to log updater debug output.", false},
    {"parent-pid", "PID",
     "(optional) PID of the parent process. The updater will wait for this process to complete "
     "before proceeding.",
     true},
};

// Prints the help and returns nullopt if a required flag is missing.
std::optional<UpdaterArguments> ReadArguments(std::vector<std::string>& args)
{
  optparse::OptionParser parser =
      optparse::OptionParser().prog("Dolphin Updater").description("Dolphin Updater binary");
  for (const FlagSpec& flag : UPDATER_FLAGS)
  {
    optparse::Option& option = parser.add_option(std::string("--") + flag.name).dest(flag.name);
    if (flag.is_int)
      option.type("int");
    option.help(flag.help).metavar(flag.metavar);
  }

  optparse::Values values = parser.parse_args(args);

  for (const char* required :
       {"this-manifest-url", "next-manifest-url", "content-store-url", "install-base-path"})
  {
    if (!values.is_set(required))
    {
      parser.print_help();
      return std::nullopt;
    }
  }

  UpdaterArguments result;
  result.this_manifest_url = values["this-manifest-url"];
  result.next_manifest_url = values["next-manifest-url"];
  result.content_store_url = values["content-store-url"];
  result.install_base_path = values["install-base-path"];
  if (values.is_set("binary-to-restart"))
    result.binary_to_restart = values["binary-to-restart"];
  if (values.is_set("parent-pid"))
    result.parent_pid = static_cast<u32>(values.get("parent-pid"));
  if (values.is_set("log-file"))
    result.log_file = values["log-file"];
  return result;
}

// Fetches both manifests; reports the failure itself.
bool FetchManifests(const UpdaterArguments& arguments, Manifest* current, Manifest* next)
{
  std::optional<Manifest> fetched = DownloadManifest(arguments.this_manifest_url);
  if (!fetched)
  {
    Fail("Could not fetch current manifest. Aborting.");
    return false;
  }
  *current = std::move(*fetched);

  fetched = DownloadManifest(arguments.next_manifest_url);
  if (!fetched)
  {
    Fail("Could not fetch next manifest. Aborting.");
    return false;
  }
  *next = std::move(*fetched);
  return true;
}

bool UpdateWithWindow(const UpdaterArguments& arguments)
{
  if (arguments.log_file)
  {
    // Fall back to stderr if the log cannot be created.
    if (s_log.Open(*arguments.log_file, "w"))
      std::atexit(CloseLog);
    else
      s_log.SetHandle(stderr);
  }

  LogToFile("Updating from: %s\n", arguments.this_manifest_url.c_str());
  LogToFile("Updating to:   %s\n", arguments.next_manifest_url.c_str());
  LogToFile("Install path:  %s\n", arguments.install_base_path.c_str());

  if (!File::IsDirectory(arguments.install_base_path))
  {
    Fail("Cannot find install base path, or not a directory.");
    return false;
  }

  // The program being updated must exit before its files are replaced.
  if (arguments.parent_pid)
  {
    LogToFile("Waiting for parent PID %d to complete...\n", *arguments.parent_pid);
    UI::WaitForPID(*arguments.parent_pid);
    LogToFile("Completed! Proceeding with update.\n");
  }

  UI::SetVisible(true);
  UI::SetDescription("Fetching and parsing manifests...");

  Manifest current;
  Manifest next;
  if (!FetchManifests(arguments, &current, &next))
    return false;

  UI::SetDescription("Computing what to do...");
  const TodoList todo = PlanUpdate(current, next);
  todo.Log();

  const std::string temp_dir = File::CreateTempDir();
  if (temp_dir.empty())
  {
    Fail("Could not create temporary directory. Aborting.");
    return false;
  }

  UI::SetDescription("Performing Update...");
  const bool applied =
      ApplyPlan(todo, arguments.install_base_path, arguments.content_store_url, temp_dir);
  RemoveDownloads(temp_dir, todo);
  if (!applied)
  {
    Fail("Failed to apply the update.");
    return false;
  }

  // Show a full, finished window for a moment.
  UI::ResetCurrentProgress();
  UI::ResetTotalProgress();
  UI::SetCurrentMarquee(false);
  UI::SetTotalMarquee(false);
  UI::SetCurrentProgress(1, 1);
  UI::SetTotalProgress(1, 1);
  UI::SetDescription("Done!");
  UI::Sleep(1);

  if (arguments.binary_to_restart)
    UI::LaunchApplication(*arguments.binary_to_restart);
  return true;
}
}  // namespace

void LogToFile(const char* fmt, ...)
{
  va_list args;
  va_start(args, fmt);
  s_log.WriteString(StringFromFormatV(fmt, args));
  va_end(args);
  s_log.Flush();
}

std::string HexEncode(const u8* buffer, size_t size)
{
  static constexpr char DIGITS[] = "0123456789abcdef";
  std::string hex;
  hex.reserve(size * 2);
  for (const u8 byte : std::span(buffer, size))
  {
    hex.push_back(DIGITS[byte >> 4]);
    hex.push_back(DIGITS[byte & 0x0F]);
  }
  return hex;
}

Manifest::Hash ComputeHash(const std::string& contents)
{
  // The manifest stores the first half of the SHA-256.
  std::array<u8, 32> sha256{};
  mbedtls_sha256_ret(reinterpret_cast<const u8*>(contents.data()), contents.size(), sha256.data(),
                     0);
  Manifest::Hash truncated{};
  std::copy_n(sha256.begin(), truncated.size(), truncated.begin());
  return truncated;
}

void TodoList::Log() const
{
  if (!to_update.empty())
  {
    LogToFile("Updating:\n");
    for (const UpdateOp& op : to_update)
    {
      const std::string before = op.old_hash ? HashName(*op.old_hash) : "(new)";
      LogToFile("  - %s: %s -> %s\n", op.filename.c_str(), before.c_str(),
                HashName(op.new_hash).c_str());
    }
  }

  if (!to_delete.empty())
  {
    LogToFile("Deleting:\n");
    for (const DeleteOp& op : to_delete)
      LogToFile("  - %s (%s)\n", op.filename.c_str(), HashName(op.old_hash).c_str());
  }
}

bool RunUpdater(std::vector<std::string> args)
{
  const std::optional<UpdaterArguments> arguments = ReadArguments(args);
  if (!arguments)
    return false;

  // The window stays hidden while waiting for the parent process.
  UI::Init();
  UI::SetVisible(false);
  const bool updated = UpdateWithWindow(*arguments);
  UI::Stop();
  return updated;
}
