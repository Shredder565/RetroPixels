// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <SFML/Network/Packet.hpp>

#include <chrono>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/SyncIdentifier.h"

struct GCPadStatus;

namespace NetPlay
{
using namespace std::chrono_literals;
// How long sent packets may go unacknowledged before a peer counts as disconnected.
constexpr std::chrono::milliseconds PEER_TIMEOUT = 30s;

// This machine's public IPv4 address as a web service sees it, or "" on failure.
std::string GetExternalIPAddress();

// Files, folder trees and buffers travel LZO-compressed: a u64 size, then chunks (u32 length plus
// that many bytes, each the compression of up to 64 KiB) and a u32 0. A folder is a bool (whether
// it exists), then per level a u64 entry count and for each entry its name, an is-folder bool and
// its contents.
bool CompressFileIntoPacket(const std::string& file_path, sf::Packet& packet);
bool CompressFolderIntoPacket(const std::string& folder_path, sf::Packet& packet);
bool CompressBufferIntoPacket(std::span<const u8> in_buffer, sf::Packet& packet);
bool DecompressPacketIntoFile(sf::Packet& packet, const std::string& file_path);
bool DecompressPacketIntoFolder(sf::Packet& packet, const std::string& folder_path);
std::optional<std::vector<u8>> DecompressPacketIntoBuffer(sf::Packet& packet);

// A game as the host names it: the DOL/ELF size as a u64, the game ID, revision, disc number,
// Datel flag and the 20-byte sync hash.
void WriteSyncIdentifier(sf::Packet& packet, const SyncIdentifier& identifier);
void ReadSyncIdentifier(sf::Packet& packet, SyncIdentifier* identifier);

// One GC pad state after its port number: the buttons and, for a port without a GBA, the analog
// values and the connected flag.
void WritePadState(sf::Packet& packet, const GCPadStatus& pad, bool buttons_only);
void ReadPadState(sf::Packet& packet, GCPadStatus* pad, bool buttons_only);
}  // namespace NetPlay
