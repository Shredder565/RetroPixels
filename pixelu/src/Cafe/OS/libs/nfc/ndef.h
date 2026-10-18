// Original code for RetroPixels (PixelU). Not derived from Cemu.
// NDEF messages (NFC Data Exchange Format): a list of records, each with a
// type name format, a type, an optional id and a payload.
#pragma once

#include <span>
#include <vector>

namespace nfc::ndef
{
	// type name formats
	constexpr uint8 kTnfEmpty = 0;
	constexpr uint8 kTnfWellKnown = 1;
	constexpr uint8 kTnfMedia = 2;
	constexpr uint8 kTnfUri = 3;
	constexpr uint8 kTnfExternal = 4;
	constexpr uint8 kTnfUnknown = 5; // the Wii U keeps its tag payload in a record of this format
	constexpr uint8 kTnfUnchanged = 6;

	struct Record
	{
		uint8 tnf = kTnfEmpty;
		std::vector<uint8> type;
		std::vector<uint8> id;
		std::vector<uint8> payload;
	};

	// Parses a whole message. Fails if no record could be read or the last
	// record read does not carry the message-end flag.
	bool ParseMessage(std::span<const uint8> bytes, std::vector<Record>& records);

	// Serialises records as one message (first gets MB, last gets ME; short
	// record form for payloads under 255 bytes).
	std::vector<uint8> BuildMessage(const std::vector<Record>& records);

	// first record with the given type name format, or nullptr
	const Record* FindByTnf(const std::vector<Record>& records, uint8 tnf);
}
