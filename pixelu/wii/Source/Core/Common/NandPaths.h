// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "Common/CommonTypes.h"

namespace Common
{
// Which emulated NAND root a path refers to.
enum class FromWhichRoot
{
  Configured,  // the configured Wii NAND (D_WIIROOT_IDX)
  Session,     // the running game's NAND (D_SESSION_WIIROOT_IDX)
  Banners,     // NAND searched for save banners (D_BANNERS_WIIROOT_IDX)
};

// The host directory of that NAND root, without a trailing separator.
std::string RootUserPath(FromWhichRoot from);

// NAND paths. Without a root they are NAND-relative ("/title/..."); with one, the host
// directory of that root is prepended.

// /import/%08x/%08x (used by ES while a title is being installed).
std::string GetImportTitlePath(u64 title_id, std::optional<FromWhichRoot> from = {});

std::string GetTicketFileName(u64 title_id, std::optional<FromWhichRoot> from = {});
std::string GetV1TicketFileName(u64 title_id, std::optional<FromWhichRoot> from = {});
std::string GetTitlePath(u64 title_id, std::optional<FromWhichRoot> from = {});
std::string GetTitleDataPath(u64 title_id, std::optional<FromWhichRoot> from = {});
std::string GetTitleContentPath(u64 title_id, std::optional<FromWhichRoot> from = {});
std::string GetTMDFileName(u64 title_id, std::optional<FromWhichRoot> from = {});
std::string GetMiiDatabasePath(std::optional<FromWhichRoot> from = {});

// Whether path lies inside /title/<hi>/<lo>/ of that root; optionally returns the title ID.
bool IsTitlePath(const std::string& path, std::optional<FromWhichRoot> from = {},
                 u64* title_id = nullptr);

// NAND file names may contain characters host file systems reject or treat specially. They
// are stored on the host as "__xx__" (two hex digits); "__" itself becomes "__5f____5f__",
// and a name made only of dots has every dot escaped.
std::string EscapeFileName(const std::string& filename);
// EscapeFileName applied to every '/'-separated component.
std::string EscapePath(const std::string& path);
// Undoes EscapeFileName.
std::string UnescapeFileName(const std::string& filename);
// Whether a name needs no escaping and is not empty or all dots.
bool IsFileNameSafe(const std::string_view filename);
}  // namespace Common
