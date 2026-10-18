// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Common/precompiled.h"
#include "TLV.h"
#include "stream.h"

namespace nfc::tlv
{
	bool Parse(std::span<const uint8> area, std::vector<Entry>& entries)
	{
		entries.clear();
		ByteReader in(area);
		while (in.Left() > 0)
		{
			Entry e;
			e.type = in.U8();
			if (e.type == kNull)
				continue;
			if (e.type == kTerminator)
			{
				entries.push_back(std::move(e));
				break;
			}
			uint32 len = in.U8();
			if (len == 0xFF)
				len = in.U16();
			in.Bytes(len, e.value);
			if (!in.Ok())
			{
				cemuLog_log(LogType::Force, "nfc: TLV list runs past the end of the tag data area");
				entries.clear();
				return false;
			}
			entries.push_back(std::move(e));
		}
		// a missing terminator is accepted (NTAG-style tags often have none)
		return true;
	}

	void Append(const Entry& entry, std::vector<uint8>& out)
	{
		ByteWriter w(out);
		w.U8(entry.type);
		if (entry.type == kNull || entry.type == kTerminator)
			return;
		cemu_assert(entry.value.size() <= 0xFFFF);
		if (entry.value.size() < 0xFF)
			w.U8((uint8)entry.value.size());
		else
		{
			w.U8(0xFF);
			w.U16((uint16)entry.value.size());
		}
		w.Bytes(entry.value);
	}
}
