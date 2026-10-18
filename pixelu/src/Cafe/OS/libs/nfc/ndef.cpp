// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Common/precompiled.h"
#include "ndef.h"
#include "stream.h"

namespace nfc::ndef
{
	namespace
	{
		// record header bits
		constexpr uint8 kFlagMessageBegin = 0x80;
		constexpr uint8 kFlagMessageEnd = 0x40;
		constexpr uint8 kFlagChunk = 0x20;
		constexpr uint8 kFlagShort = 0x10;
		constexpr uint8 kFlagHasId = 0x08;
		constexpr uint8 kTnfBits = 0x07;

		constexpr uint32 kMaxPayload = 2 * 1024 * 1024; // sanity limit

		// reads one record; sets isLast from the ME flag
		bool ReadRecord(ByteReader& in, Record& rec, bool& isLast)
		{
			const uint8 header = in.U8();
			const uint8 typeLen = in.U8();
			const uint32 payloadLen = (header & kFlagShort) ? in.U8() : in.U32();
			const uint8 idLen = (header & kFlagHasId) ? in.U8() : 0;
			if (!in.Ok() || payloadLen > kMaxPayload)
				return false;
			rec.tnf = header & kTnfBits;
			in.Bytes(typeLen, rec.type);
			in.Bytes(idLen, rec.id);
			in.Bytes(payloadLen, rec.payload);
			isLast = (header & kFlagMessageEnd) != 0;
			return in.Ok();
		}
	}

	bool ParseMessage(std::span<const uint8> bytes, std::vector<Record>& records)
	{
		records.clear();
		ByteReader in(bytes);
		bool lastHadEnd = false;
		while (in.Left() > 0)
		{
			Record rec;
			bool isLast = false;
			if (!ReadRecord(in, rec, isLast))
			{
				cemuLog_log(LogType::Force, "nfc: NDEF record {} is malformed, ignoring the remaining {} bytes", records.size(), in.Left());
				break;
			}
			records.push_back(std::move(rec));
			lastHadEnd = isLast;
			if (isLast)
			{
				if (in.Left() > 0)
					cemuLog_log(LogType::Force, "nfc: ignoring {} bytes after the NDEF message end", in.Left());
				break;
			}
		}
		if (records.empty() || !lastHadEnd)
		{
			if (!records.empty())
				cemuLog_log(LogType::Force, "nfc: NDEF message has no end record");
			records.clear();
			return false;
		}
		return true;
	}

	std::vector<uint8> BuildMessage(const std::vector<Record>& records)
	{
		std::vector<uint8> out;
		ByteWriter w(out);
		for (size_t i = 0; i < records.size(); i++)
		{
			const Record& rec = records[i];
			const bool isShort = rec.payload.size() < 0xFF;
			uint8 header = rec.tnf & kTnfBits;
			if (i == 0)
				header |= kFlagMessageBegin;
			if (i + 1 == records.size())
				header |= kFlagMessageEnd;
			if (isShort)
				header |= kFlagShort;
			if (!rec.id.empty())
				header |= kFlagHasId;
			w.U8(header);
			w.U8((uint8)rec.type.size());
			if (isShort)
				w.U8((uint8)rec.payload.size());
			else
				w.U32((uint32)rec.payload.size());
			if (!rec.id.empty())
				w.U8((uint8)rec.id.size());
			w.Bytes(rec.type);
			w.Bytes(rec.id);
			w.Bytes(rec.payload);
		}
		return out;
	}

	const Record* FindByTnf(const std::vector<Record>& records, uint8 tnf)
	{
		for (const Record& r : records)
		{
			if (r.tnf == tnf)
				return &r;
		}
		return nullptr;
	}
}
