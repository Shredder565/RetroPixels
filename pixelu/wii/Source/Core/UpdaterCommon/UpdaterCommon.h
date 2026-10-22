// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"

// A build's file list: each installed file's path (relative to the install folder) and the first
// 16 bytes of the SHA-256 of its contents.
struct Manifest
{
  using Filename = std::string;
  using Hash = std::array<u8, 16>;
  std::map<Filename, Hash> entries;
};

// What moving from one manifest to the next requires.
struct TodoList
{
  // Content to fetch from the content store, named by its hash.
  struct DownloadOp
  {
    Manifest::Filename filename;
    Manifest::Hash hash{};
  };
  std::vector<DownloadOp> to_download;

  // A file to write; old_hash is empty for a file that is new in this build.
  struct UpdateOp
  {
    Manifest::Filename filename;
    std::optional<Manifest::Hash> old_hash;
    Manifest::Hash new_hash{};
  };
  std::vector<UpdateOp> to_update;

  // A file the new build no longer has.
  struct DeleteOp
  {
    Manifest::Filename filename;
    Manifest::Hash old_hash{};
  };
  std::vector<DeleteOp> to_delete;

  void Log() const;
};

// printf-style logging to the --log-file given on the command line.
void LogToFile(const char* fmt, ...);
// Lower-case hex.
std::string HexEncode(const u8* buffer, size_t size);
Manifest::Hash ComputeHash(const std::string& contents);
// The updater's whole run, from its command-line arguments; false on any failure.
bool RunUpdater(std::vector<std::string> args);
