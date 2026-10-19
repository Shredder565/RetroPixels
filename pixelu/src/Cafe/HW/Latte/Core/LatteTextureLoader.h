// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "util/helpers/ClassWrapper.h"
#include "Cafe/HW/Latte/LatteAddrLib/LatteAddrLib.h"
#include "util/ImageWriter/tga.h"

// State for converting one slice of one mip level between guest memory and a
// linear host buffer. Filled by LatteTextureLoader_begin().
struct LatteTextureLoaderCtx
{
	uint32 physAddress;
	uint32 physMipAddress;
	sint32 width; // level size in pixels
	sint32 height;
	sint32 pitch; // elements (texels or blocks) per row in memory
	uint32 mipLevels;
	uint32 sliceIndex;
	sint32 stepX; // pixels per element: 4 for block compressed formats, else 1
	sint32 stepY;
	uint32 pipeSwizzle;
	uint32 bankSwizzle;
	Latte::E_HWTILEMODE tileMode;
	uint32 bpp; // bits per element
	uint8* inputData; // first byte of the level
	sint32 minOffsetOutdated;
	sint32 maxOffsetOutdated;
	uint32 surfaceInfoHeight;
	uint32 surfaceInfoDepth;
	uint32 levelOffset; // offset of the level relative to physMipAddress
	// size of the linear output in elements, set from the decoder
	sint32 decodedTexelCountX;
	sint32 decodedTexelCountY;
	LatteAddrLib::CachedSurfaceAddrInfo computeAddrInfo;
	// texture dumping
	bool dump;
	uint8* dumpRGBA;
};

// pointer to the element that holds pixel (x, y) of the level
uint8* LatteTextureLoader_GetInput(LatteTextureLoaderCtx* textureLoader, sint32 x, sint32 y);

#include "Cafe/HW/Latte/LatteAddrLib/AddrLibFastDecode.h"

// BCn block decoders. Output is one 4x4 block in row order, as floats:
// RGBA for BC1-BC3, R for BC4, RG for BC5
void decodeBC1Block(uint8* inputData, float* output4x4RGBA);
void decodeBC2Block_UNORM(uint8* inputData, float* imageRGBA);
void decodeBC3Block_UNORM(uint8* inputData, float* imageRGBA);
void decodeBC4Block_UNORM(uint8* blockStorage, float* rOutput);
void decodeBC5Block_UNORM(uint8* blockStorage, float* rgOutput);
void decodeBC5Block_SNORM(uint8* blockStorage, float* rgOutput);

namespace LatteTexDec
{
	inline uint16 Read16(const uint8* p) { uint16 v; memcpy(&v, p, 2); return v; }
	inline uint32 Read32(const uint8* p) { uint32 v; memcpy(&v, p, 4); return v; }
	inline float ReadF32(const uint8* p) { float v; memcpy(&v, p, 4); return v; }
	inline void Write16(uint8* p, uint16 v) { memcpy(p, &v, 2); }
	inline void Write32(uint8* p, uint32 v) { memcpy(p, &v, 4); }
	inline void WriteF32(uint8* p, float v) { memcpy(p, &v, 4); }

	// replicate the top bits of an n bit channel into the low bits of a byte
	inline uint8 Widen4(uint32 v) { return (uint8)((v << 4) | v); }
	inline uint8 Widen5(uint32 v) { return (uint8)((v << 3) | (v >> 2)); }
	inline uint8 Widen6(uint32 v) { return (uint8)((v << 2) | (v >> 4)); }
	inline uint8 SwapNibbles(uint8 v) { return (uint8)((v >> 4) | (v << 4)); }
	// float in 0..1 to byte, truncating
	inline uint8 FloatToByte(float f) { return (uint8)(f * 255.0f); }

	inline void SetRGBA(uint8* out, uint8 r, uint8 g, uint8 b, uint8 a)
	{
		out[0] = r;
		out[1] = g;
		out[2] = b;
		out[3] = a;
	}

	// BC1 colour palette. With fourColor the two extra entries are the 1/3 and
	// 2/3 blends, otherwise entry 2 is the midpoint and entry 3 transparent black.
	struct BC1Palette
	{
		float r[4], g[4], b[4], a[4];

		void Build(uint16 c0, uint16 c1, bool fourColor)
		{
			r[0] = (float)((c0 >> 11) & 0x1F) / 31.0f;
			r[1] = (float)((c1 >> 11) & 0x1F) / 31.0f;
			g[0] = (float)((c0 >> 5) & 0x3F) / 63.0f;
			g[1] = (float)((c1 >> 5) & 0x3F) / 63.0f;
			b[0] = (float)(c0 & 0x1F) / 31.0f;
			b[1] = (float)(c1 & 0x1F) / 31.0f;
			a[0] = a[1] = a[2] = 1.0f;
			if (fourColor)
			{
				r[2] = (2.0f * r[0] + r[1]) / 3.0f;
				g[2] = (2.0f * g[0] + g[1]) / 3.0f;
				b[2] = (2.0f * b[0] + b[1]) / 3.0f;
				r[3] = (r[0] + 2.0f * r[1]) / 3.0f;
				g[3] = (g[0] + 2.0f * g[1]) / 3.0f;
				b[3] = (b[0] + 2.0f * b[1]) / 3.0f;
				a[3] = 1.0f;
			}
			else
			{
				r[2] = (r[0] + r[1]) / 2.0f;
				g[2] = (g[0] + g[1]) / 2.0f;
				b[2] = (b[0] + b[1]) / 2.0f;
				r[3] = g[3] = b[3] = a[3] = 0.0f;
			}
		}

		// BC1 picks the mode from the endpoint order
		void BuildBC1(const uint8* block)
		{
			const uint16 c0 = Read16(block);
			const uint16 c1 = Read16(block + 2);
			Build(c0, c1, c0 > c1);
		}
	};

	// decode a whole block and return pixel (px, py), channel c of 'channels'
	template<int channels>
	inline float BlockSample(void(*blockDecoder)(uint8*, float*), uint8* block, uint8 px, uint8 py, int c)
	{
		float decoded[16 * channels];
		blockDecoder(block, decoded);
		return decoded[(px + py * 4) * channels + c];
	}

	// Visit every element of the level through the generic address function.
	// fn(element pointer, pixel x, pixel y)
	template<typename F>
	inline void ForEachElement(LatteTextureLoaderCtx* ctx, F&& fn)
	{
		for (sint32 y = 0; y < ctx->height; y += ctx->stepY)
			for (sint32 x = 0; x < ctx->width; x += ctx->stepX)
				fn(LatteTextureLoader_GetInput(ctx, x, y), x, y);
	}

	// Convert element by element into a buffer of ctx->width pixels per row.
	// fn(source element, destination texel)
	template<sint32 outBytes, typename F>
	inline void ConvertEach(LatteTextureLoaderCtx* ctx, uint8* out, F&& fn)
	{
		const sint32 rowPixels = ctx->width;
		ForEachElement(ctx, [&](uint8* src, sint32 x, sint32 y) {
			fn(src, out + (x + y * rowPixels) * outBytes);
		});
	}

	// Expand BCn blocks to float texels. Each block decodes to inChannels floats per
	// pixel, written as outChannels floats (missing channels are 0). Pixels outside
	// the level are not written.
	template<int inChannels, int outChannels>
	inline void ExpandBlocks(LatteTextureLoaderCtx* ctx, uint8* out, void(*blockDecoder)(uint8*, float*))
	{
		const sint32 rowPixels = ctx->width;
		ForEachElement(ctx, [&](uint8* src, sint32 x, sint32 y) {
			float decoded[16 * inChannels];
			blockDecoder(src, decoded);
			const sint32 w = std::min<sint32>(4, ctx->width - x);
			const sint32 h = std::min<sint32>(4, ctx->height - y);
			for (sint32 by = 0; by < h; by++)
			{
				for (sint32 bx = 0; bx < w; bx++)
				{
					uint8* dst = out + (x + bx + (y + by) * rowPixels) * (outChannels * 4);
					const float* texel = decoded + (bx + by * 4) * inChannels;
					for (int c = 0; c < outChannels; c++)
						WriteF32(dst + c * 4, c < inChannels ? texel[c] : 0.0f);
				}
			}
		});
	}

	// The 16 bit float formats are dumped by scaling the raw half bits (no real conversion)
	inline uint8 HalfBitsToByte(const uint8* p) { return (uint8)((float)Read16(p) * 255.0f); }
};

inline void BC1_GetPixel(uint8* inputData, sint32 x, sint32 y, uint8 rgba[4])
{
	LatteTexDec::BC1Palette pal;
	pal.BuildBC1(inputData);
	const uint32 i = (inputData[4 + y] >> (x * 2)) & 3;
	LatteTexDec::SetRGBA(rgba, LatteTexDec::FloatToByte(pal.r[i]), LatteTexDec::FloatToByte(pal.g[i]), LatteTexDec::FloatToByte(pal.b[i]), LatteTexDec::FloatToByte(pal.a[i]));
}

// Converts one guest texture format into a linear layout the host API can upload.
class TextureDecoder
{
public:
	virtual sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) = 0;
	virtual sint32 getTexelCountX(LatteTextureLoaderCtx* textureLoader)
	{
		return textureLoader->width;
	}
	virtual sint32 getTexelCountY(LatteTextureLoaderCtx* textureLoader)
	{
		return textureLoader->height;
	}
	virtual sint32 calculateImageSize(LatteTextureLoaderCtx* textureLoader)
	{
		return getTexelCountX(textureLoader) * getTexelCountY(textureLoader) * getBytesPerTexel(textureLoader);
	}
	// writes the converted level to outputData
	virtual void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) = 0;
	// one pixel as RGBA8 (for texture dumps); blockOffset is the pixel position inside a 4x4 block
	virtual void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) = 0;
};

// Formats the host stores exactly like the guest: the data is only untiled.
// unit/unitCount give the element size, blocks marks 4x4 compressed formats.
template<typename unit, int unitCount, bool blocks = false>
class TextureDecoderUntile : public TextureDecoder
{
public:
	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return (sint32)(sizeof(unit) * unitCount);
	}
	sint32 getTexelCountX(LatteTextureLoaderCtx* textureLoader) override
	{
		return blocks ? (textureLoader->width + 3) / 4 : textureLoader->width;
	}
	sint32 getTexelCountY(LatteTextureLoaderCtx* textureLoader) override
	{
		return blocks ? (textureLoader->height + 3) / 4 : textureLoader->height;
	}
	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		optimizedDecodeLoops<unit, unitCount, false, blocks>(textureLoader, outputData);
	}
};

// Formats converted texel by texel. Derived classes provide
// static void Convert(const uint8* src, uint8* dst).
template<typename derived, sint32 outBytes>
class TextureDecoderConvert : public TextureDecoder
{
public:
	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return outBytes;
	}
	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		LatteTexDec::ConvertEach<outBytes>(textureLoader, outputData, [](const uint8* src, uint8* dst) { derived::Convert(src, dst); });
	}
};

// --- raw copies ----------------------------------------------------------

class TextureDecoder_R16_G16_B16_A16_FLOAT : public TextureDecoderUntile<uint64, 1>, public SingletonClass<TextureDecoder_R16_G16_B16_A16_FLOAT>
{
public:
	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		using namespace LatteTexDec;
		SetRGBA(outputPixel, HalfBitsToByte(blockData), HalfBitsToByte(blockData + 2), HalfBitsToByte(blockData + 4), HalfBitsToByte(blockData + 6));
	}
};

class TextureDecoder_R16_G16_FLOAT : public TextureDecoderUntile<uint32, 1>, public SingletonClass<TextureDecoder_R16_G16_FLOAT>
{
public:
	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		using namespace LatteTexDec;
		SetRGBA(outputPixel, HalfBitsToByte(blockData), HalfBitsToByte(blockData + 2), 0, 255);
	}
};

class TextureDecoder_R16_SNORM : public TextureDecoderUntile<uint16, 1>, public SingletonClass<TextureDecoder_R16_SNORM>
{
public:
	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		// upper byte halved and moved up by 128
		using namespace LatteTexDec;
		SetRGBA(outputPixel, (uint8)((Read16(blockData) >> 8) / 2 + 128), 0, 0, 255);
	}
};

class TextureDecoder_R16_FLOAT : public TextureDecoderUntile<uint16, 1>, public SingletonClass<TextureDecoder_R16_FLOAT>
{
public:
	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		using namespace LatteTexDec;
		SetRGBA(outputPixel, HalfBitsToByte(blockData), 0, 0, 255);
	}
};

class TextureDecoder_R32_FLOAT : public TextureDecoderUntile<uint32, 1>, public SingletonClass<TextureDecoder_R32_FLOAT>
{
public:
	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		using namespace LatteTexDec;
		SetRGBA(outputPixel, FloatToByte(ReadF32(blockData)), 0, 0, 255);
	}
};

class TextureDecoder_R32_G32_FLOAT : public TextureDecoderUntile<uint64, 1>, public SingletonClass<TextureDecoder_R32_G32_FLOAT>
{
public:
	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		using namespace LatteTexDec;
		SetRGBA(outputPixel, FloatToByte(ReadF32(blockData)), FloatToByte(ReadF32(blockData + 4)), 0, 255);
	}
};

class TextureDecoder_R32_G32_UINT : public TextureDecoderUntile<uint64, 1>, public SingletonClass<TextureDecoder_R32_G32_UINT>
{
public:
	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		using namespace LatteTexDec;
		SetRGBA(outputPixel, (uint8)(Read32(blockData) >> 24), (uint8)(Read32(blockData + 4) >> 24), 0, 255);
	}
};

class TextureDecoder_R32_UINT : public TextureDecoderUntile<uint32, 1>, public SingletonClass<TextureDecoder_R32_UINT>
{
public:
	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		using namespace LatteTexDec;
		SetRGBA(outputPixel, (uint8)(Read32(blockData) >> 24), 0, 0, 255);
	}
};

class TextureDecoder_R16_UINT : public TextureDecoderUntile<uint16, 1>, public SingletonClass<TextureDecoder_R16_UINT>
{
public:
	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		using namespace LatteTexDec;
		SetRGBA(outputPixel, (uint8)(Read16(blockData) >> 8), 0, 0, 255);
	}
};

class TextureDecoder_R8_UINT : public TextureDecoderUntile<uint8, 1>, public SingletonClass<TextureDecoder_R8_UINT>
{
public:
	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		LatteTexDec::SetRGBA(outputPixel, blockData[0], 0, 0, 255);
	}
};

class TextureDecoder_R32_G32_B32_A32_FLOAT : public TextureDecoderUntile<uint64, 2>, public SingletonClass<TextureDecoder_R32_G32_B32_A32_FLOAT>
{
public:
	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		using namespace LatteTexDec;
		SetRGBA(outputPixel, FloatToByte(ReadF32(blockData)), FloatToByte(ReadF32(blockData + 4)), FloatToByte(ReadF32(blockData + 8)), FloatToByte(ReadF32(blockData + 12)));
	}
};

class TextureDecoder_R32_G32_B32_A32_UINT : public TextureDecoderUntile<uint64, 2>, public SingletonClass<TextureDecoder_R32_G32_B32_A32_UINT>
{
public:
	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		using namespace LatteTexDec;
		SetRGBA(outputPixel, (uint8)(Read32(blockData) >> 24), (uint8)(Read32(blockData + 4) >> 24), (uint8)(Read32(blockData + 8) >> 24), (uint8)(Read32(blockData + 12) >> 24));
	}
};

// four 16 bit channels shown by their upper bytes
class TextureDecoderUntile16x4 : public TextureDecoderUntile<uint64, 1>
{
public:
	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		using namespace LatteTexDec;
		SetRGBA(outputPixel, (uint8)(Read16(blockData) >> 8), (uint8)(Read16(blockData + 2) >> 8), (uint8)(Read16(blockData + 4) >> 8), (uint8)(Read16(blockData + 6) >> 8));
	}
};

class TextureDecoder_R16_G16_B16_A16_UINT : public TextureDecoderUntile16x4, public SingletonClass<TextureDecoder_R16_G16_B16_A16_UINT>
{
};

class TextureDecoder_R16_G16_B16_A16 : public TextureDecoderUntile16x4, public SingletonClass<TextureDecoder_R16_G16_B16_A16>
{
};

class TextureDecoder_R8_G8_B8_A8_UINT : public TextureDecoderUntile<uint32, 1>, public SingletonClass<TextureDecoder_R8_G8_B8_A8_UINT>
{
public:
	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		memcpy(outputPixel, blockData, 4);
	}
};

class TextureDecoder_R8_G8_B8_A8 : public TextureDecoderUntile<uint32, 1>, public SingletonClass<TextureDecoder_R8_G8_B8_A8>
{
public:
	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		memcpy(outputPixel, blockData, 4);
	}
};

class TextureDecoder_D32_S8_UINT_X24 : public TextureDecoderUntile<uint64, 1>, public SingletonClass<TextureDecoder_D32_S8_UINT_X24>
{
public:
	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		// not dumped
	}
};

class TextureDecoder_D24_S8 : public TextureDecoderUntile<uint32, 1>, public SingletonClass<TextureDecoder_D24_S8>
{
public:
	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		// depth as grey (top 8 of its 24 bits), stencil in alpha
		const uint32 v = LatteTexDec::Read32(blockData);
		const uint8 depthHigh = (uint8)((v >> 16) & 0xFF);
		LatteTexDec::SetRGBA(outputPixel, depthHigh, depthHigh, depthHigh, (uint8)(v >> 24));
	}
};

class TextureDecoder_R8 : public TextureDecoderUntile<uint8, 1>, public SingletonClass<TextureDecoder_R8>
{
public:
	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		LatteTexDec::SetRGBA(outputPixel, blockData[0], 0, 0, 255);
	}
};

class TextureDecoder_R8_G8 : public TextureDecoderUntile<uint16, 1>, public SingletonClass<TextureDecoder_R8_G8>
{
public:
	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		LatteTexDec::SetRGBA(outputPixel, blockData[0], blockData[1], 0, 255);
	}
};

// R4G4 byte dumped as low nibble -> red, high nibble -> green
inline void LatteTexDec_DumpR4G4(uint8 v, uint8* outputPixel)
{
	LatteTexDec::SetRGBA(outputPixel, LatteTexDec::Widen4(v & 0xF), LatteTexDec::Widen4(v >> 4), 0, 255);
}

class TextureDecoder_R4_G4 : public TextureDecoderUntile<uint8, 1>, public SingletonClass<TextureDecoder_R4_G4>
{
public:
	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		LatteTexDec_DumpR4G4(blockData[0], outputPixel);
	}
};

class TextureDecoder_R16_UNORM : public TextureDecoderUntile<uint16, 1>, public SingletonClass<TextureDecoder_R16_UNORM>
{
public:
	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		using namespace LatteTexDec;
		SetRGBA(outputPixel, (uint8)(Read16(blockData) >> 8), 0, 0, 255);
	}
};

class TextureDecoder_R16_G16 : public TextureDecoderUntile<uint32, 1>, public SingletonClass<TextureDecoder_R16_G16>
{
public:
	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		using namespace LatteTexDec;
		SetRGBA(outputPixel, (uint8)(Read16(blockData) >> 8), (uint8)(Read16(blockData + 2) >> 8), 0, 255);
	}
};

// 5:6:5 dumped with bits 0-4 as red and bits 11-15 as blue
inline void LatteTexDec_Dump565(const uint8* blockData, uint8* outputPixel)
{
	using namespace LatteTexDec;
	const uint16 v = Read16(blockData);
	SetRGBA(outputPixel, Widen5(v & 0x1F), Widen6((v >> 5) & 0x3F), Widen5((v >> 11) & 0x1F), 255);
}

// 1:5:5:5 with red in the low bits. The dump takes its alpha bit from bit 11
// and shifts it left by 3, so alpha is always 0 or 8.
inline void LatteTexDec_Dump5551(const uint8* blockData, uint8* outputPixel)
{
	using namespace LatteTexDec;
	const uint16 v = Read16(blockData);
	SetRGBA(outputPixel, Widen5(v & 0x1F), Widen5((v >> 5) & 0x1F), Widen5((v >> 10) & 0x1F), (uint8)(((v >> 11) & 1) << 3));
}

// 5:5:5:1 with red in the high bits, alpha in bit 0 (dumped as 0 or 8)
inline void LatteTexDec_Dump1555(const uint8* blockData, uint8* outputPixel)
{
	using namespace LatteTexDec;
	const uint16 v = Read16(blockData);
	SetRGBA(outputPixel, Widen5((v >> 11) & 0x1F), Widen5((v >> 6) & 0x1F), Widen5((v >> 1) & 0x1F), (uint8)((v & 1) << 3));
}

class TextureDecoder_R5_G6_B5 : public TextureDecoderUntile<uint16, 1>, public SingletonClass<TextureDecoder_R5_G6_B5>
{
public:
	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		LatteTexDec_Dump565(blockData, outputPixel);
	}
};

class TextureDecoder_R5_G5_B5_A1_UNORM : public TextureDecoderUntile<uint16, 1>, public SingletonClass<TextureDecoder_R5_G5_B5_A1_UNORM>
{
public:
	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		LatteTexDec_Dump5551(blockData, outputPixel);
	}
};

// 16 bit element that exchanges the 5 bit fields at bit 0 and bit 10 when assigned
// (used as the copy unit of the untile loops)
class uint16_R5_G5_B5_A1_swapRB
{
public:
	void operator=(const uint16_R5_G5_B5_A1_swapRB& v)
	{
		const uint16 s = v.internalVal;
		internalVal = (uint16)((s & 0x83E0) | ((s >> 10) & 0x001F) | ((s & 0x001F) << 10));
	}

	uint16 internalVal;
};

static_assert(sizeof(uint16_R5_G5_B5_A1_swapRB) == 2);

class TextureDecoder_R5_G5_B5_A1_UNORM_swappedRB : public TextureDecoderUntile<uint16_R5_G5_B5_A1_swapRB, 1>, public SingletonClass<TextureDecoder_R5_G5_B5_A1_UNORM_swappedRB>
{
public:
	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		LatteTexDec_Dump5551(blockData, outputPixel);
	}
};

// 16 bit element that turns 1:5:5:5 (red low) into 5:5:5:1 (red high, alpha bit 0) when assigned
class uint16_R5_G5_B5_A1_swapOpenGL
{
public:
	void operator=(const uint16_R5_G5_B5_A1_swapOpenGL& v)
	{
		const uint16 s = v.internalVal;
		internalVal = (uint16)(((s & 0x1F) << 11) | (((s >> 5) & 0x1F) << 6) | (((s >> 10) & 0x1F) << 1) | (s >> 15));
	}
	uint16 internalVal;
};

static_assert(sizeof(uint16_R5_G5_B5_A1_swapOpenGL) == 2);

class TextureDecoder_R5_G5_B5_A1_UNORM_swappedOpenGL : public TextureDecoderUntile<uint16_R5_G5_B5_A1_swapOpenGL, 1>, public SingletonClass<TextureDecoder_R5_G5_B5_A1_UNORM_swappedOpenGL>
{
public:
	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		LatteTexDec_Dump5551(blockData, outputPixel);
	}
};

class TextureDecoder_A1_B5_G5_R5_UNORM : public TextureDecoderUntile<uint16, 1>, public SingletonClass<TextureDecoder_A1_B5_G5_R5_UNORM>
{
public:
	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		LatteTexDec_Dump1555(blockData, outputPixel);
	}
};

class TextureDecoder_R10_G10_B10_A2_UNORM : public TextureDecoderUntile<uint32, 1>, public SingletonClass<TextureDecoder_R10_G10_B10_A2_UNORM>
{
public:
	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		// the dump keeps only the top 4 bits of each 10 bit channel and the raw 2 bit alpha
		const uint32 v = LatteTexDec::Read32(blockData);
		LatteTexDec::SetRGBA(outputPixel, (uint8)((v >> 6) & 0xF), (uint8)((v >> 16) & 0xF), (uint8)((v >> 26) & 0xF), (uint8)(v >> 30));
	}
};

class TextureDecoder_R11_G11_B10_FLOAT : public TextureDecoderUntile<uint32, 1>, public SingletonClass<TextureDecoder_R11_G11_B10_FLOAT>
{
public:
	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		// not dumped
	}
};

// --- texel conversions ---------------------------------------------------

// depth formats the host cannot sample from guest data: uploaded as zero
class TextureDecoder_R24_X8 : public TextureDecoderConvert<TextureDecoder_R24_X8, 4>, public SingletonClass<TextureDecoder_R24_X8>
{
public:
	static void Convert(const uint8* src, uint8* dst) { LatteTexDec::Write32(dst, 0); }
	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override {}
};

class TextureDecoder_X24_G8_UINT : public TextureDecoderConvert<TextureDecoder_X24_G8_UINT, 4>, public SingletonClass<TextureDecoder_X24_G8_UINT>
{
public:
	static void Convert(const uint8* src, uint8* dst) { LatteTexDec::Write32(dst, 0); }
	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override {}
};

// R4G4 into the upper byte of an RGBA4 texel, nibbles exchanged; blue and alpha zero
class TextureDecoder_R4_G4_UNORM_To_RGBA4 : public TextureDecoderConvert<TextureDecoder_R4_G4_UNORM_To_RGBA4, 2>, public SingletonClass<TextureDecoder_R4_G4_UNORM_To_RGBA4>
{
public:
	static void Convert(const uint8* src, uint8* dst)
	{
		dst[0] = 0;
		dst[1] = LatteTexDec::SwapNibbles(src[0]);
	}
	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		LatteTexDec_DumpR4G4(blockData[0], outputPixel);
	}
};

// R4G4 into the upper byte of an ABGR4 texel unchanged
class TextureDecoder_R4_G4_UNORM_To_ABGR4 : public TextureDecoderConvert<TextureDecoder_R4_G4_UNORM_To_ABGR4, 2>, public SingletonClass<TextureDecoder_R4_G4_UNORM_To_ABGR4>
{
public:
	static void Convert(const uint8* src, uint8* dst)
	{
		dst[0] = 0;
		dst[1] = src[0];
	}
	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		LatteTexDec_DumpR4G4(blockData[0], outputPixel);
	}
};

// the RG8 / RGBA8 expansions read red from the high nibble
class TextureDecoder_R4G4_UNORM_To_RGBA8 : public TextureDecoderConvert<TextureDecoder_R4G4_UNORM_To_RGBA8, 4>, public SingletonClass<TextureDecoder_R4G4_UNORM_To_RGBA8>
{
public:
	static void Convert(const uint8* src, uint8* dst)
	{
		LatteTexDec::SetRGBA(dst, LatteTexDec::Widen4(src[0] >> 4), LatteTexDec::Widen4(src[0] & 0xF), 0, 255);
	}
	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		Convert(blockData, outputPixel);
	}
};

class TextureDecoder_R4G4_UNORM_To_RG8 : public TextureDecoderConvert<TextureDecoder_R4G4_UNORM_To_RG8, 2>, public SingletonClass<TextureDecoder_R4G4_UNORM_To_RG8>
{
public:
	static void Convert(const uint8* src, uint8* dst)
	{
		dst[0] = LatteTexDec::Widen4(src[0] >> 4);
		dst[1] = LatteTexDec::Widen4(src[0] & 0xF);
	}
	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		LatteTexDec::SetRGBA(outputPixel, LatteTexDec::Widen4(blockData[0] >> 4), LatteTexDec::Widen4(blockData[0] & 0xF), 0, 255);
	}
};

// four nibbles, low nibble of byte 0 first
inline void LatteTexDec_Expand4444(const uint8* blockData, uint8* outputPixel)
{
	using namespace LatteTexDec;
	SetRGBA(outputPixel, Widen4(blockData[0] & 0xF), Widen4(blockData[0] >> 4), Widen4(blockData[1] & 0xF), Widen4(blockData[1] >> 4));
}

// bytes exchanged and the nibbles of each byte exchanged (full nibble reversal)
class TextureDecoder_R4_G4_B4_A4_UNORM : public TextureDecoderConvert<TextureDecoder_R4_G4_B4_A4_UNORM, 2>, public SingletonClass<TextureDecoder_R4_G4_B4_A4_UNORM>
{
public:
	static void Convert(const uint8* src, uint8* dst)
	{
		dst[0] = LatteTexDec::SwapNibbles(src[1]);
		dst[1] = LatteTexDec::SwapNibbles(src[0]);
	}
	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		LatteTexDec_Expand4444(blockData, outputPixel);
	}
};

class TextureDecoder_R4G4B4A4_UNORM_To_RGBA8 : public TextureDecoderConvert<TextureDecoder_R4G4B4A4_UNORM_To_RGBA8, 4>, public SingletonClass<TextureDecoder_R4G4B4A4_UNORM_To_RGBA8>
{
public:
	static void Convert(const uint8* src, uint8* dst)
	{
		LatteTexDec_Expand4444(src, dst);
	}
	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		LatteTexDec_Expand4444(blockData, outputPixel);
	}
};

// 5:6:5 with the two 5 bit fields exchanged
class TextureDecoder_R5_G6_B5_swappedRB : public TextureDecoderConvert<TextureDecoder_R5_G6_B5_swappedRB, 2>, public SingletonClass<TextureDecoder_R5_G6_B5_swappedRB>
{
public:
	static void Convert(const uint8* src, uint8* dst)
	{
		const uint16 v = LatteTexDec::Read16(src);
		LatteTexDec::Write16(dst, (uint16)((v & 0x07E0) | (v >> 11) | ((v & 0x1F) << 11)));
	}
	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		LatteTexDec_Dump565(blockData, outputPixel);
	}
};

// 5:6:5 to 8 bit channels. The 5 bit channels fill their low bits with v >> 3
// (not the usual v >> 2), which this conversion has always done.
class TextureDecoder_R5G6B5_UNORM_To_RGBA8 : public TextureDecoderConvert<TextureDecoder_R5G6B5_UNORM_To_RGBA8, 4>, public SingletonClass<TextureDecoder_R5G6B5_UNORM_To_RGBA8>
{
public:
	static uint8 Widen5Short(uint32 v) { return (uint8)((v << 3) | (v >> 3)); }
	static void Convert(const uint8* src, uint8* dst)
	{
		const uint16 v = LatteTexDec::Read16(src);
		LatteTexDec::SetRGBA(dst, Widen5Short(v & 0x1F), LatteTexDec::Widen6((v >> 5) & 0x3F), Widen5Short((v >> 11) & 0x1F), 255);
	}
	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		Convert(blockData, outputPixel);
	}
};

// 1:5:5:5 (red low) to RGBA8 with a full alpha
class TextureDecoder_R5_G5_B5_A1_UNORM_swappedRB_To_RGBA8 : public TextureDecoderConvert<TextureDecoder_R5_G5_B5_A1_UNORM_swappedRB_To_RGBA8, 4>, public SingletonClass<TextureDecoder_R5_G5_B5_A1_UNORM_swappedRB_To_RGBA8>
{
public:
	static void Convert(const uint8* src, uint8* dst)
	{
		using namespace LatteTexDec;
		const uint16 v = Read16(src);
		SetRGBA(dst, Widen5(v & 0x1F), Widen5((v >> 5) & 0x1F), Widen5((v >> 10) & 0x1F), (v & 0x8000) ? 0xFF : 0x00);
	}
	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		Convert(blockData, outputPixel);
	}
};

// 5:5:5:1 (red high, alpha bit 0) to 1:5:5:5 (blue low, alpha bit 15)
class TextureDecoder_A1_B5_G5_R5_UNORM_vulkan : public TextureDecoderConvert<TextureDecoder_A1_B5_G5_R5_UNORM_vulkan, 2>, public SingletonClass<TextureDecoder_A1_B5_G5_R5_UNORM_vulkan>
{
public:
	static void Convert(const uint8* src, uint8* dst)
	{
		const uint16 v = LatteTexDec::Read16(src);
		const uint16 r = (v >> 11) & 0x1F;
		const uint16 g = (v >> 6) & 0x1F;
		const uint16 b = (v >> 1) & 0x1F;
		LatteTexDec::Write16(dst, (uint16)(b | (g << 5) | (r << 10) | ((v & 1) << 15)));
	}
	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		LatteTexDec_Dump1555(blockData, outputPixel);
	}
};

// 5:5:5:1 (red high, alpha bit 0) to RGBA8 with a full alpha
class TextureDecoder_A1_B5_G5_R5_UNORM_vulkan_To_RGBA8 : public TextureDecoderConvert<TextureDecoder_A1_B5_G5_R5_UNORM_vulkan_To_RGBA8, 4>, public SingletonClass<TextureDecoder_A1_B5_G5_R5_UNORM_vulkan_To_RGBA8>
{
public:
	static void Convert(const uint8* src, uint8* dst)
	{
		using namespace LatteTexDec;
		const uint16 v = Read16(src);
		SetRGBA(dst, Widen5((v >> 11) & 0x1F), Widen5((v >> 6) & 0x1F), Widen5((v >> 1) & 0x1F), (v & 1) ? 0xFF : 0x00);
	}
	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		LatteTexDec_Dump1555(blockData, outputPixel);
	}
};

// Placeholder for the 10:10:10:2 formats that need a 16 bit per channel host
// format. The conversion is not implemented: only the first 16 bits of each
// output row are cleared and the rest of the buffer is left as it is.
class TextureDecoderRGBA16Placeholder : public TextureDecoder
{
public:
	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return 8;
	}
	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		if (textureLoader->width <= 0)
			return;
		for (sint32 y = 0; y < textureLoader->height; y += textureLoader->stepY)
			LatteTexDec::Write16(outputData + (size_t)(y * textureLoader->width) * 8, 0);
	}
};

class TextureDecoder_R10_G10_B10_A2_SNORM_To_RGBA16 : public TextureDecoderRGBA16Placeholder, public SingletonClass<TextureDecoder_R10_G10_B10_A2_SNORM_To_RGBA16>
{
public:
	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		// top 4 bits of each channel (and the 2 bit alpha), halved and moved up by 128
		const uint32 v = LatteTexDec::Read32(blockData);
		auto bias = [](uint32 c) { return (uint8)(c / 2 + 128); };
		LatteTexDec::SetRGBA(outputPixel, bias((v >> 6) & 0xF), bias((v >> 16) & 0xF), bias((v >> 26) & 0xF), bias(v >> 30));
	}
};

class TextureDecoder_A2_B10_G10_R10_UNORM_To_RGBA16 : public TextureDecoderRGBA16Placeholder, public SingletonClass<TextureDecoder_A2_B10_G10_R10_UNORM_To_RGBA16>
{
public:
	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		// alpha in bits 0-1, then three 10 bit channels starting with red (top 4 bits each)
		const uint32 v = LatteTexDec::Read32(blockData);
		LatteTexDec::SetRGBA(outputPixel, (uint8)((v >> 8) & 0xF), (uint8)((v >> 18) & 0xF), (uint8)((v >> 28) & 0xF), (uint8)(v & 3));
	}
};

// --- zero fill -----------------------------------------------------------

template<sint32 bytesPerTexel>
class TextureDecoderZeroFill : public TextureDecoder
{
public:
	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return bytesPerTexel;
	}
	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		memset(outputData, 0, (size_t)bytesPerTexel * getTexelCountX(textureLoader) * getTexelCountY(textureLoader));
	}
	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
	}
};

class TextureDecoder_NullData32 : public TextureDecoderZeroFill<4>, public SingletonClass<TextureDecoder_NullData32>
{
};

class TextureDecoder_NullData64 : public TextureDecoderZeroFill<8>, public SingletonClass<TextureDecoder_NullData64>
{
};

// --- block compressed ----------------------------------------------------

// dump helpers: one pixel of a decoded block as RGBA8
inline void LatteTexDec_DumpBlockRGBA(void(*blockDecoder)(uint8*, float*), uint8* blockData, uint8* outputPixel, uint8 bx, uint8 by)
{
	float decoded[64];
	blockDecoder(blockData, decoded);
	const float* t = decoded + (bx + by * 4) * 4;
	using namespace LatteTexDec;
	SetRGBA(outputPixel, FloatToByte(t[0]), FloatToByte(t[1]), FloatToByte(t[2]), FloatToByte(t[3]));
}

inline void LatteTexDec_DumpBlockR(void(*blockDecoder)(uint8*, float*), uint8* blockData, uint8* outputPixel, uint8 bx, uint8 by)
{
	using namespace LatteTexDec;
	SetRGBA(outputPixel, FloatToByte(BlockSample<1>(blockDecoder, blockData, bx, by, 0)), 0, 0, 255);
}

inline void LatteTexDec_DumpBlockRG(void(*blockDecoder)(uint8*, float*), uint8* blockData, uint8* outputPixel, uint8 bx, uint8 by)
{
	float decoded[32];
	blockDecoder(blockData, decoded);
	const float* t = decoded + (bx + by * 4) * 2;
	using namespace LatteTexDec;
	SetRGBA(outputPixel, FloatToByte(t[0]), FloatToByte(t[1]), 0, 255);
}

// blocks kept compressed for the host
class TextureDecoder_BC1 : public TextureDecoderUntile<uint64, 1, true>, public SingletonClass<TextureDecoder_BC1>
{
public:
	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		BC1_GetPixel(blockData, blockOffsetX, blockOffsetY, outputPixel);
	}
};

class TextureDecoder_BC2 : public TextureDecoderUntile<uint64, 2, true>, public SingletonClass<TextureDecoder_BC2>
{
public:
	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		LatteTexDec_DumpBlockRGBA(decodeBC2Block_UNORM, blockData, outputPixel, blockOffsetX, blockOffsetY);
	}
};

class TextureDecoder_BC3 : public TextureDecoderUntile<uint64, 2, true>, public SingletonClass<TextureDecoder_BC3>
{
public:
	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		LatteTexDec_DumpBlockRGBA(decodeBC3Block_UNORM, blockData, outputPixel, blockOffsetX, blockOffsetY);
	}
};

class TextureDecoder_BC4 : public TextureDecoderUntile<uint64, 1, true>, public SingletonClass<TextureDecoder_BC4>
{
public:
	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		LatteTexDec_DumpBlockR(decodeBC4Block_UNORM, blockData, outputPixel, blockOffsetX, blockOffsetY);
	}
};

class TextureDecoder_BC5 : public TextureDecoderUntile<uint64, 2, true>, public SingletonClass<TextureDecoder_BC5>
{
public:
	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		LatteTexDec_DumpBlockRG(decodeBC5Block_UNORM, blockData, outputPixel, blockOffsetX, blockOffsetY);
	}
};

// blocks expanded to 32 bit float texels for hosts without BCn support
// (the sRGB variants are expanded without a colour space conversion)
class TextureDecoder_BC1_UNORM_uncompress_generic : public TextureDecoder
{
public:
	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return 16;
	}
	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		LatteTexDec::ExpandBlocks<4, 4>(textureLoader, outputData, decodeBC1Block);
	}
	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		BC1_GetPixel(blockData, blockOffsetX, blockOffsetY, outputPixel);
	}
};

class TextureDecoder_BC1_UNORM_uncompress : public TextureDecoder_BC1_UNORM_uncompress_generic, public SingletonClass<TextureDecoder_BC1_UNORM_uncompress>
{
};

class TextureDecoder_BC1_SRGB_uncompress : public TextureDecoder_BC1_UNORM_uncompress_generic, public SingletonClass<TextureDecoder_BC1_SRGB_uncompress>
{
};

class TextureDecoderBC2Expand : public TextureDecoder
{
public:
	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return 16;
	}
	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		LatteTexDec::ExpandBlocks<4, 4>(textureLoader, outputData, decodeBC2Block_UNORM);
	}
	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		LatteTexDec_DumpBlockRGBA(decodeBC2Block_UNORM, blockData, outputPixel, blockOffsetX, blockOffsetY);
	}
};

class TextureDecoder_BC2_UNORM_uncompress : public TextureDecoderBC2Expand, public SingletonClass<TextureDecoder_BC2_UNORM_uncompress>
{
};

class TextureDecoder_BC2_SRGB_uncompress : public TextureDecoderBC2Expand, public SingletonClass<TextureDecoder_BC2_SRGB_uncompress>
{
};

class TextureDecoder_BC3_uncompress_generic : public TextureDecoder
{
public:
	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return 16;
	}
	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		LatteTexDec::ExpandBlocks<4, 4>(textureLoader, outputData, decodeBC3Block_UNORM);
	}
	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		LatteTexDec_DumpBlockRGBA(decodeBC3Block_UNORM, blockData, outputPixel, blockOffsetX, blockOffsetY);
	}
};

class TextureDecoder_BC3_UNORM_uncompress : public TextureDecoder_BC3_uncompress_generic, public SingletonClass<TextureDecoder_BC3_UNORM_uncompress>
{
};

class TextureDecoder_BC3_SRGB_uncompress : public TextureDecoder_BC3_uncompress_generic, public SingletonClass<TextureDecoder_BC3_SRGB_uncompress>
{
};

// BC4 expands to RG32F with green zero
class TextureDecoder_BC4_UNORM_uncompress : public TextureDecoder, public SingletonClass<TextureDecoder_BC4_UNORM_uncompress>
{
public:
	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return 8;
	}
	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		LatteTexDec::ExpandBlocks<1, 2>(textureLoader, outputData, decodeBC4Block_UNORM);
	}
	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		LatteTexDec_DumpBlockR(decodeBC4Block_UNORM, blockData, outputPixel, blockOffsetX, blockOffsetY);
	}
};

class TextureDecoder_BC5_UNORM_uncompress : public TextureDecoder, public SingletonClass<TextureDecoder_BC5_UNORM_uncompress>
{
public:
	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return 8;
	}
	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		LatteTexDec::ExpandBlocks<2, 2>(textureLoader, outputData, decodeBC5Block_UNORM);
	}
	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		LatteTexDec_DumpBlockRG(decodeBC5Block_UNORM, blockData, outputPixel, blockOffsetX, blockOffsetY);
	}
};

class TextureDecoder_BC5_SNORM_uncompress : public TextureDecoder, public SingletonClass<TextureDecoder_BC5_SNORM_uncompress>
{
public:
	sint32 getBytesPerTexel(LatteTextureLoaderCtx* textureLoader) override
	{
		return 8;
	}
	void decode(LatteTextureLoaderCtx* textureLoader, uint8* outputData) override
	{
		LatteTexDec::ExpandBlocks<2, 2>(textureLoader, outputData, decodeBC5Block_SNORM);
	}
	void decodePixelToRGBA(uint8* blockData, uint8* outputPixel, uint8 blockOffsetX, uint8 blockOffsetY) override
	{
		// -1..1 mapped to 0..255 for the dump
		float decoded[32];
		decodeBC5Block_SNORM(blockData, decoded);
		const float* t = decoded + (blockOffsetX + blockOffsetY * 4) * 2;
		using namespace LatteTexDec;
		SetRGBA(outputPixel, FloatToByte(0.5f + t[0] * 0.5f), FloatToByte(0.5f + t[1] * 0.5f), 0, 255);
	}
};
