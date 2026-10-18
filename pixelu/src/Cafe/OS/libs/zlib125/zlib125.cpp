// Original code for RetroPixels (PixelU). Not derived from Cemu.
// Written from scratch.
//
// zlib125 is the console's copy of zlib 1.2.5. PixelU runs the host's zlib
// instead and keeps a host z_stream for every game z_stream that was
// initialised: before each call the game-visible fields (buffers, counters)
// are copied to the host stream, afterwards they are copied back. The host
// stream's internal state lives in host memory, so the game's zalloc/zfree
// are never needed; the game's "state" field only holds a non-null marker.

#include "Cafe/OS/common/OSCommon.h"
#include "zlib125.h"
#include "zlib.h"

namespace zlib125_exports
{
	// z_stream as laid out in the game's memory (32-bit, big-endian)
	struct GameZStream
	{
		/* +0x00 */ MEMPTR<uint8> nextIn;
		/* +0x04 */ uint32be availIn;
		/* +0x08 */ uint32be totalIn;
		/* +0x0C */ MEMPTR<uint8> nextOut;
		/* +0x10 */ uint32be availOut;
		/* +0x14 */ uint32be totalOut;
		/* +0x18 */ MEMPTR<char> msg;
		/* +0x1C */ MEMPTR<void> state;
		/* +0x20 */ MEMPTR<void> zalloc;
		/* +0x24 */ MEMPTR<void> zfree;
		/* +0x28 */ MEMPTR<void> opaque;
		/* +0x2C */ uint32be dataType;
		/* +0x30 */ uint32be adler;
		/* +0x34 */ uint32be reserved;
	};
	static_assert(sizeof(GameZStream) == 0x38);

	struct HostStream
	{
		z_stream z{};
		bool compressing = false; // deflate stream (else inflate)
	};

	std::mutex g_streamsLock;
	std::unordered_map<MPTR, std::unique_ptr<HostStream>> g_streams; // keyed by the game stream's address

	MPTR AddressOf(GameZStream* stream)
	{
		return memory_getVirtualOffsetFromPointer(stream);
	}

	z_stream* FindHostStream(GameZStream* stream)
	{
		if (!stream)
			return nullptr;
		std::lock_guard lock(g_streamsLock);
		auto it = g_streams.find(AddressOf(stream));
		return it == g_streams.end() ? nullptr : &it->second->z;
	}

	// A fresh host stream for an *Init call, replacing any stream left at that address.
	z_stream* CreateHostStream(GameZStream* stream, bool compressing)
	{
		auto host = std::make_unique<HostStream>();
		host->compressing = compressing;
		z_stream* raw = &host->z;
		std::lock_guard lock(g_streamsLock);
		auto& slot = g_streams[AddressOf(stream)];
		if (slot)
			cemuLog_logDebug(LogType::Force, "zlib125: stream {:08x} initialised again without End", AddressOf(stream));
		slot = std::move(host);
		return raw;
	}

	void DestroyHostStream(GameZStream* stream)
	{
		std::lock_guard lock(g_streamsLock);
		g_streams.erase(AddressOf(stream));
	}

	void CopyIn(const GameZStream* game, z_stream* host)
	{
		host->next_in = game->nextIn.GetPtr();
		host->avail_in = game->availIn;
		host->total_in = game->totalIn;
		host->next_out = game->nextOut.GetPtr();
		host->avail_out = game->availOut;
		host->total_out = game->totalOut;
		host->data_type = (int)(uint32)game->dataType;
		host->adler = game->adler;
	}

	void CopyOut(const z_stream* host, GameZStream* game, MPTR stateMarker)
	{
		game->nextIn = host->next_in;
		game->availIn = (uint32)host->avail_in;
		game->totalIn = (uint32)host->total_in;
		game->nextOut = host->next_out;
		game->availOut = (uint32)host->avail_out;
		game->totalOut = (uint32)host->total_out;
		game->msg = nullptr; // host error strings are not in game memory
		game->state = MEMPTR<void>(stateMarker);
		game->dataType = (uint32)host->data_type;
		game->adler = (uint32)host->adler;
	}

	// Runs a zlib call on the host stream that belongs to the game stream.
	template<typename F>
	sint32 WithStream(GameZStream* stream, F&& call)
	{
		z_stream* host = FindHostStream(stream);
		if (!host)
			return Z_STREAM_ERROR;
		CopyIn(stream, host);
		const sint32 r = call(host);
		CopyOut(host, stream, AddressOf(stream));
		return r;
	}

	// Shared tail of the four *Init functions.
	template<typename F>
	sint32 InitStream(GameZStream* stream, bool compressing, F&& init)
	{
		if (!stream)
			return Z_STREAM_ERROR;
		z_stream* host = CreateHostStream(stream, compressing);
		CopyIn(stream, host);
		const sint32 r = init(host);
		if (r != Z_OK)
		{
			DestroyHostStream(stream);
			stream->state = nullptr;
			stream->msg = nullptr;
			return r;
		}
		CopyOut(host, stream, AddressOf(stream));
		return r;
	}

	sint32 inflateInit_(GameZStream* stream, const char* version, sint32 streamSize)
	{
		return InitStream(stream, false, [&](z_stream* s) { return ::inflateInit_(s, version, (int)sizeof(z_stream)); });
	}

	sint32 inflateInit2_(GameZStream* stream, sint32 windowBits, const char* version, sint32 streamSize)
	{
		return InitStream(stream, false, [&](z_stream* s) { return ::inflateInit2_(s, windowBits, version, (int)sizeof(z_stream)); });
	}

	sint32 deflateInit_(GameZStream* stream, sint32 level, const char* version, sint32 streamSize)
	{
		return InitStream(stream, true, [&](z_stream* s) { return ::deflateInit_(s, level, version, (int)sizeof(z_stream)); });
	}

	sint32 deflateInit2_(GameZStream* stream, sint32 level, sint32 method, sint32 windowBits, sint32 memLevel, sint32 strategy, const char* version, sint32 streamSize)
	{
		return InitStream(stream, true, [&](z_stream* s) { return ::deflateInit2_(s, level, method, windowBits, memLevel, strategy, version, (int)sizeof(z_stream)); });
	}

	sint32 inflate(GameZStream* stream, sint32 flush)
	{
		return WithStream(stream, [&](z_stream* s) { return ::inflate(s, flush); });
	}

	sint32 inflateReset(GameZStream* stream)
	{
		return WithStream(stream, [&](z_stream* s) { return ::inflateReset(s); });
	}

	sint32 inflateReset2(GameZStream* stream, sint32 windowBits)
	{
		return WithStream(stream, [&](z_stream* s) { return ::inflateReset2(s, windowBits); });
	}

	// Ends the stream and forgets it, whatever zlib answers.
	template<typename F>
	sint32 EndStream(GameZStream* stream, F&& end)
	{
		if (!FindHostStream(stream))
			return Z_STREAM_ERROR;
		const sint32 r = WithStream(stream, end);
		DestroyHostStream(stream);
		stream->state = nullptr;
		return r;
	}

	sint32 inflateEnd(GameZStream* stream)
	{
		return EndStream(stream, [](z_stream* s) { return ::inflateEnd(s); });
	}

	sint32 deflate(GameZStream* stream, sint32 flush)
	{
		return WithStream(stream, [&](z_stream* s) { return ::deflate(s, flush); });
	}

	sint32 deflateEnd(GameZStream* stream)
	{
		return EndStream(stream, [](z_stream* s) { return ::deflateEnd(s); });
	}

	// Works without an initialised stream too (zlib then assumes default settings).
	uint32 deflateBound(GameZStream* stream, uint32 sourceLen)
	{
		z_stream* host = FindHostStream(stream);
		if (!host)
			return (uint32)::deflateBound(nullptr, sourceLen);
		CopyIn(stream, host);
		return (uint32)::deflateBound(host, sourceLen);
	}

	sint32 uncompress(uint8* dest, uint32be* destLen, const uint8* source, uint32 sourceLen)
	{
		uLongf len = *destLen;
		const sint32 r = ::uncompress(dest, &len, source, sourceLen);
		*destLen = (uint32)len;
		return r;
	}

	sint32 compress(uint8* dest, uint32be* destLen, const uint8* source, uint32 sourceLen)
	{
		uLongf len = *destLen;
		const sint32 r = ::compress(dest, &len, source, sourceLen);
		*destLen = (uint32)len;
		return r;
	}

	uint32 compressBound(uint32 sourceLen)
	{
		return (uint32)::compressBound(sourceLen);
	}

	uint32 crc32(uint32 crc, MPTR buffer, uint32 length)
	{
		const uint8* data = buffer ? memory_getPointerFromVirtualOffset(buffer) : nullptr;
		return (uint32)::crc32(crc, data, data ? length : 0);
	}
}

namespace zlib
{
	void load()
	{
	}

	namespace
	{
		class Zlib125Library final : public COSModule
		{
		public:
			std::string_view GetName() override { return "zlib125"; }

			void RPLMapped() override
			{
				constexpr auto lt = LogType::Placeholder;
				cafeExportRegisterFunc(zlib125_exports::inflateInit_, "zlib125", "inflateInit_", lt);
				cafeExportRegisterFunc(zlib125_exports::inflateInit2_, "zlib125", "inflateInit2_", lt);
				cafeExportRegisterFunc(zlib125_exports::inflate, "zlib125", "inflate", lt);
				cafeExportRegisterFunc(zlib125_exports::inflateReset, "zlib125", "inflateReset", lt);
				cafeExportRegisterFunc(zlib125_exports::inflateReset2, "zlib125", "inflateReset2", lt);
				cafeExportRegisterFunc(zlib125_exports::inflateEnd, "zlib125", "inflateEnd", lt);
				cafeExportRegisterFunc(zlib125_exports::deflateInit_, "zlib125", "deflateInit_", lt);
				cafeExportRegisterFunc(zlib125_exports::deflateInit2_, "zlib125", "deflateInit2_", lt);
				cafeExportRegisterFunc(zlib125_exports::deflate, "zlib125", "deflate", lt);
				cafeExportRegisterFunc(zlib125_exports::deflateBound, "zlib125", "deflateBound", lt);
				cafeExportRegisterFunc(zlib125_exports::deflateEnd, "zlib125", "deflateEnd", lt);
				cafeExportRegisterFunc(zlib125_exports::uncompress, "zlib125", "uncompress", lt);
				cafeExportRegisterFunc(zlib125_exports::compress, "zlib125", "compress", lt);
				cafeExportRegisterFunc(zlib125_exports::compressBound, "zlib125", "compressBound", lt);
				cafeExportRegisterFunc(zlib125_exports::crc32, "zlib125", "crc32", lt);
			}

			void RPLUnmapped() override
			{
				using namespace zlib125_exports;
				std::lock_guard lock(g_streamsLock);
				for (auto& [address, host] : g_streams)
				{
					// streams the game never ended
					if (host->compressing)
						::deflateEnd(&host->z);
					else
						::inflateEnd(&host->z);
				}
				g_streams.clear();
			}
		};

		Zlib125Library g_zlib125Library;
	}

	COSModule* GetModule()
	{
		return &g_zlib125Library;
	}
}
