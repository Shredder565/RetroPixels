// Original code for RetroPixels (PixelU). Not derived from Cemu.
//
// AES-128 (FIPS-197) in ECB, CBC and CTR mode. The S-boxes are computed at
// compile time from the GF(2^8) definition, so there are no large tables to
// get wrong. A hardware path using AES-NI is picked at start-up when the CPU
// has it; both paths give the same bytes.
//
// Buffers may overlap exactly (output == input) in every function, which the
// filesystem code relies on for in-place decryption.

#include "aes128.h"
#include "Common/cpu_features.h"

namespace
{
	constexpr size_t kBlock = 16;
	constexpr int kRounds = 10;

	// --- GF(2^8) arithmetic with the AES polynomial x^8+x^4+x^3+x+1 ---

	constexpr uint8 gfDouble(uint8 v)
	{
		return (uint8)((v << 1) ^ ((v & 0x80) ? 0x1B : 0x00));
	}

	constexpr uint8 gfMul(uint8 a, uint8 b)
	{
		uint8 product = 0;
		while (b)
		{
			if (b & 1)
				product ^= a;
			a = gfDouble(a);
			b >>= 1;
		}
		return product;
	}

	constexpr uint8 gfInverse(uint8 v)
	{
		if (v == 0)
			return 0;
		// v^254 == v^-1 in GF(2^8)
		uint8 result = 1;
		uint8 base = v;
		uint32 exponent = 254;
		while (exponent)
		{
			if (exponent & 1)
				result = gfMul(result, base);
			base = gfMul(base, base);
			exponent >>= 1;
		}
		return result;
	}

	constexpr uint8 rotl8(uint8 v, int n)
	{
		return (uint8)((v << n) | (v >> (8 - n)));
	}

	struct SBoxes
	{
		uint8 forward[256]{};
		uint8 inverse[256]{};
	};

	constexpr SBoxes buildSBoxes()
	{
		SBoxes boxes{};
		for (int i = 0; i < 256; i++)
		{
			const uint8 inv = gfInverse((uint8)i);
			const uint8 s = (uint8)(inv ^ rotl8(inv, 1) ^ rotl8(inv, 2) ^ rotl8(inv, 3) ^ rotl8(inv, 4) ^ 0x63);
			boxes.forward[i] = s;
			boxes.inverse[s] = (uint8)i;
		}
		return boxes;
	}

	constexpr SBoxes kSBox = buildSBoxes();
	static_assert(kSBox.forward[0x00] == 0x63 && kSBox.forward[0x53] == 0xED && kSBox.inverse[0x63] == 0x00);

	// --- portable implementation ---
	// The state is kept in input byte order: byte (row r, column c) is state[c * 4 + r].

	struct RoundKeys
	{
		uint8 bytes[(kRounds + 1) * kBlock];
	};

	void expandKey(const uint8* key, RoundKeys& rk)
	{
		std::memcpy(rk.bytes, key, kBlock);
		uint8 rcon = 1;
		for (size_t pos = kBlock; pos < sizeof(rk.bytes); pos += 4)
		{
			uint8 word[4] = { rk.bytes[pos - 4], rk.bytes[pos - 3], rk.bytes[pos - 2], rk.bytes[pos - 1] };
			if ((pos % kBlock) == 0)
			{
				// RotWord, SubWord, Rcon
				const uint8 first = word[0];
				word[0] = (uint8)(kSBox.forward[word[1]] ^ rcon);
				word[1] = kSBox.forward[word[2]];
				word[2] = kSBox.forward[word[3]];
				word[3] = kSBox.forward[first];
				rcon = gfDouble(rcon);
			}
			for (int b = 0; b < 4; b++)
				rk.bytes[pos + b] = (uint8)(rk.bytes[pos - kBlock + b] ^ word[b]);
		}
	}

	void xorInto(uint8* dst, const uint8* src, size_t n = kBlock)
	{
		for (size_t i = 0; i < n; i++)
			dst[i] ^= src[i];
	}

	void substituteAndShift(uint8* s, const uint8* box, bool inverseDirection)
	{
		uint8 tmp[kBlock];
		for (int col = 0; col < 4; col++)
		{
			for (int row = 0; row < 4; row++)
			{
				// row r is rotated left by r columns (right for the inverse)
				const int srcCol = inverseDirection ? (col - row + 4) % 4 : (col + row) % 4;
				tmp[col * 4 + row] = box[s[srcCol * 4 + row]];
			}
		}
		std::memcpy(s, tmp, kBlock);
	}

	void mixColumns(uint8* s)
	{
		for (int col = 0; col < 4; col++)
		{
			uint8* c = s + col * 4;
			const uint8 a0 = c[0], a1 = c[1], a2 = c[2], a3 = c[3];
			const uint8 all = (uint8)(a0 ^ a1 ^ a2 ^ a3);
			c[0] = (uint8)(a0 ^ all ^ gfDouble((uint8)(a0 ^ a1)));
			c[1] = (uint8)(a1 ^ all ^ gfDouble((uint8)(a1 ^ a2)));
			c[2] = (uint8)(a2 ^ all ^ gfDouble((uint8)(a2 ^ a3)));
			c[3] = (uint8)(a3 ^ all ^ gfDouble((uint8)(a3 ^ a0)));
		}
	}

	void inverseMixColumns(uint8* s)
	{
		for (int col = 0; col < 4; col++)
		{
			uint8* c = s + col * 4;
			const uint8 a0 = c[0], a1 = c[1], a2 = c[2], a3 = c[3];
			c[0] = (uint8)(gfMul(a0, 14) ^ gfMul(a1, 11) ^ gfMul(a2, 13) ^ gfMul(a3, 9));
			c[1] = (uint8)(gfMul(a0, 9) ^ gfMul(a1, 14) ^ gfMul(a2, 11) ^ gfMul(a3, 13));
			c[2] = (uint8)(gfMul(a0, 13) ^ gfMul(a1, 9) ^ gfMul(a2, 14) ^ gfMul(a3, 11));
			c[3] = (uint8)(gfMul(a0, 11) ^ gfMul(a1, 13) ^ gfMul(a2, 9) ^ gfMul(a3, 14));
		}
	}

	void encryptBlock(const RoundKeys& rk, const uint8* in, uint8* out)
	{
		uint8 s[kBlock];
		std::memcpy(s, in, kBlock);
		xorInto(s, rk.bytes);
		for (int round = 1; round <= kRounds; round++)
		{
			substituteAndShift(s, kSBox.forward, false);
			if (round != kRounds)
				mixColumns(s);
			xorInto(s, rk.bytes + round * kBlock);
		}
		std::memcpy(out, s, kBlock);
	}

	void decryptBlock(const RoundKeys& rk, const uint8* in, uint8* out)
	{
		uint8 s[kBlock];
		std::memcpy(s, in, kBlock);
		xorInto(s, rk.bytes + kRounds * kBlock);
		for (int round = kRounds - 1; round >= 0; round--)
		{
			substituteAndShift(s, kSBox.inverse, true);
			xorInto(s, rk.bytes + round * kBlock);
			if (round != 0)
				inverseMixColumns(s);
		}
		std::memcpy(out, s, kBlock);
	}

	// Runs fn(blockIn, blockOut) over ceil(length / 16) blocks. A trailing
	// partial block is padded with zeros and only its valid bytes are written.
	template<typename TFunc>
	void forEachBlock(uint8* output, const uint8* input, uint32 length, TFunc fn)
	{
		uint32 offset = 0;
		for (; offset + kBlock <= length; offset += kBlock)
			fn(input + offset, output + offset);
		if (offset < length)
		{
			uint8 inPad[kBlock]{};
			uint8 outPad[kBlock];
			std::memcpy(inPad, input + offset, length - offset);
			fn(inPad, outPad);
			std::memcpy(output + offset, outPad, length - offset);
		}
	}

	void softEcbEncrypt(uint8* input, const uint8* key, uint8* output)
	{
		RoundKeys rk;
		expandKey(key, rk);
		encryptBlock(rk, input, output);
	}

	void softCbcDecrypt(uint8* output, uint8* input, uint32 length, const uint8* key, const uint8* iv)
	{
		RoundKeys rk;
		expandKey(key, rk);
		uint8 chain[kBlock]{};
		if (iv)
			std::memcpy(chain, iv, kBlock);
		forEachBlock(output, input, length, [&](const uint8* in, uint8* out) {
			uint8 cipher[kBlock];
			std::memcpy(cipher, in, kBlock); // keep it, out may alias in
			decryptBlock(rk, cipher, out);
			xorInto(out, chain);
			std::memcpy(chain, cipher, kBlock);
		});
	}

#if defined(ARCH_X86_64)
	// --- AES-NI implementation ---

	template<int TRcon>
	ATTRIBUTE_AESNI inline __m128i niNextRoundKey(__m128i prev)
	{
		__m128i assist = _mm_shuffle_epi32(_mm_aeskeygenassist_si128(prev, TRcon), 0xFF);
		// prefix-xor of the four words
		prev = _mm_xor_si128(prev, _mm_slli_si128(prev, 4));
		prev = _mm_xor_si128(prev, _mm_slli_si128(prev, 8));
		return _mm_xor_si128(prev, assist);
	}

	ATTRIBUTE_AESNI void niExpandEncrypt(const uint8* key, __m128i rk[11])
	{
		rk[0] = _mm_loadu_si128((const __m128i*)key);
		rk[1] = niNextRoundKey<0x01>(rk[0]);
		rk[2] = niNextRoundKey<0x02>(rk[1]);
		rk[3] = niNextRoundKey<0x04>(rk[2]);
		rk[4] = niNextRoundKey<0x08>(rk[3]);
		rk[5] = niNextRoundKey<0x10>(rk[4]);
		rk[6] = niNextRoundKey<0x20>(rk[5]);
		rk[7] = niNextRoundKey<0x40>(rk[6]);
		rk[8] = niNextRoundKey<0x80>(rk[7]);
		rk[9] = niNextRoundKey<0x1B>(rk[8]);
		rk[10] = niNextRoundKey<0x36>(rk[9]);
	}

	ATTRIBUTE_AESNI void niEcbEncrypt(uint8* input, const uint8* key, uint8* output)
	{
		__m128i rk[11];
		niExpandEncrypt(key, rk);
		__m128i v = _mm_xor_si128(_mm_loadu_si128((const __m128i*)input), rk[0]);
		for (int r = 1; r < kRounds; r++)
			v = _mm_aesenc_si128(v, rk[r]);
		v = _mm_aesenclast_si128(v, rk[kRounds]);
		_mm_storeu_si128((__m128i*)output, v);
	}

	ATTRIBUTE_AESNI void niCbcDecrypt(uint8* output, uint8* input, uint32 length, const uint8* key, const uint8* iv)
	{
		__m128i enc[11];
		niExpandEncrypt(key, enc);
		// equivalent inverse cipher: reversed keys, InvMixColumns on the inner ones
		__m128i dec[11];
		dec[0] = enc[kRounds];
		for (int r = 1; r < kRounds; r++)
			dec[r] = _mm_aesimc_si128(enc[kRounds - r]);
		dec[kRounds] = enc[0];

		alignas(16) uint8 zeroIv[kBlock]{};
		__m128i chain = _mm_loadu_si128((const __m128i*)(iv ? iv : zeroIv));
		// no lambdas here: GCC does not pass the "aes" target attribute on to them
		for (uint32 offset = 0; offset < length; offset += (uint32)kBlock)
		{
			const uint32 n = std::min<uint32>((uint32)kBlock, length - offset);
			alignas(16) uint8 tmp[kBlock]{};
			std::memcpy(tmp, input + offset, n);
			const __m128i cipher = _mm_load_si128((const __m128i*)tmp);
			__m128i v = _mm_xor_si128(cipher, dec[0]);
			for (int r = 1; r < kRounds; r++)
				v = _mm_aesdec_si128(v, dec[r]);
			v = _mm_aesdeclast_si128(v, dec[kRounds]);
			_mm_store_si128((__m128i*)tmp, _mm_xor_si128(v, chain));
			std::memcpy(output + offset, tmp, n);
			chain = cipher;
		}
	}
#endif
}

// Both entry points start out on the portable code so they are usable even
// before AES128_init() picks the fastest implementation.
void (*AES128_ECB_encrypt)(uint8* input, const uint8* key, uint8* output) = softEcbEncrypt;
void (*AES128_CBC_decrypt)(uint8* output, uint8* input, uint32 length, const uint8* key, const uint8* iv) = softCbcDecrypt;

void AES128_ECB_decrypt(uint8* input, const uint8* key, uint8* output)
{
	RoundKeys rk;
	expandKey(key, rk);
	decryptBlock(rk, input, output);
}

void AES128_CBC_encrypt(uint8* output, uint8* input, uint32 length, const uint8* key, const uint8* iv)
{
	RoundKeys rk;
	expandKey(key, rk);
	uint8 chain[kBlock];
	std::memcpy(chain, iv, kBlock);
	forEachBlock(output, input, length, [&](const uint8* in, uint8* out) {
		uint8 block[kBlock];
		std::memcpy(block, in, kBlock);
		xorInto(block, chain);
		encryptBlock(rk, block, out);
		std::memcpy(chain, out, kBlock);
	});
}

void AES128_CBC_decrypt_updateIV(uint8* output, uint8* input, uint32 length, const uint8* key, uint8* iv)
{
	length &= ~(uint32)(kBlock - 1);
	if (length == 0)
		return;
	uint8 nextIv[kBlock];
	std::memcpy(nextIv, input + length - kBlock, kBlock); // last cipher block, before an in-place decrypt overwrites it
	AES128_CBC_decrypt(output, input, length, key, iv);
	std::memcpy(iv, nextIv, kBlock);
}

void AES128CTR_transform(uint8* data, sint32 length, uint8* key, uint8* nonceIv)
{
	cemu_assert_debug(length >= 0 && (length % (sint32)kBlock) == 0);
	for (sint32 offset = 0; offset < length; offset += (sint32)kBlock)
	{
		uint8 keystream[kBlock];
		AES128_ECB_encrypt(nonceIv, key, keystream);
		xorInto(data + offset, keystream, std::min<size_t>(kBlock, (size_t)(length - offset)));
		// the counter is the whole 16-byte block, big endian
		for (int i = (int)kBlock - 1; i >= 0; i--)
		{
			if (++nonceIv[i] != 0)
				break;
		}
	}
}

void AES128_init()
{
#if defined(ARCH_X86_64)
	if (g_CPUFeatures.x86.aesni)
	{
		AES128_ECB_encrypt = niEcbEncrypt;
		AES128_CBC_decrypt = niCbcDecrypt;
		return;
	}
#endif
	AES128_ECB_encrypt = softEcbEncrypt;
	AES128_CBC_decrypt = softCbcDecrypt;
}
