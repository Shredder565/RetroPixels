// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "Core/HW/GCMemcard/GCMemcard.h"

namespace Memcard
{
// Two saves "are the same file" when game code, maker code and the
// NUL-terminated file name all match.
bool HasSameIdentity(const DEntry& lhs, const DEntry& rhs);

// True if at least two of the given saves share an identity.
bool HasDuplicateIdentity(std::span<const Savefile> savefiles);

enum class ReadSavefileErrorCode
{
  OpenFileFail,
  IOError,
  DataCorrupted,
};

// Loads a single exported save. The container (GCI, GameShark GCS or
// MaxDrive SAV) is recognised from the file size.
std::variant<ReadSavefileErrorCode, Savefile> ReadSavefile(const std::string& filename);

enum class SavefileFormat
{
  GCI,
  GCS,
  SAV,
};

// Stores a save in the chosen container. Returns false on any I/O failure.
bool WriteSavefile(const std::string& filename, const Savefile& savefile, SavefileFormat format);

// "<maker>-<game>-<name>", escaped for use as a file name; no extension.
std::string GenerateFilename(const DEntry& entry);

// File extension for a container, including the dot.
std::string GetDefaultExtension(SavefileFormat format);

// Exports the listed saves from a card. Any failure yields an empty list.
std::vector<Savefile> GetSavefiles(const GCMemcard& card, std::span<const u8> file_indices);

// Sum of the block counts of the given saves.
size_t GetBlockCount(std::span<const Savefile> savefiles);
}  // namespace Memcard
