// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/HW/Latte/Core/LatteConst.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "Cafe/HW/Latte/Core/LattePerformanceMonitor.h"
#include "Common/cpu_features.h"

#if defined(ARCH_X86_64) && defined(__GNUC__)
#include <immintrin.h>
#elif defined(__aarch64__)
#include <arm_neon.h>
#endif

// Index buffer translation for the host renderer.
//
// Latte index buffers are big endian and the GPU knows primitive types that host
// APIs lack (quads, quad strips, line loops and, on Metal, triangle fans). The
// translated buffer lives in renderer memory and the last few translations are
// remembered so that redrawing the same buffer does not convert it again.

namespace
{
	struct TranslatedIndexBuffer
	{
		// what was translated
		const void* source;
		uint32 sourceCount;
		LattePrimitiveMode mode;
		LatteIndexType type;
		uint64 stamp; // smaller = used longer ago
		// what came out
		uint32 maxIndex;
		Renderer::INDEX_TYPE hostType;
		uint32 hostCount;
		Renderer::IndexAllocation alloc;
	};

	std::array<TranslatedIndexBuffer, 8> s_translated{};
	uint64 s_stampCounter = 0;

	void ReleaseSlot(TranslatedIndexBuffer& slot)
	{
		if (slot.source != nullptr)
			g_renderer->indexData_releaseIndexMemory(slot.alloc);
		slot.source = nullptr;
		slot.sourceCount = 0;
	}

	bool IsU16(LatteIndexType t) { return t == LatteIndexType::U16_BE || t == LatteIndexType::U16_LE; }
	bool IsU32(LatteIndexType t) { return t == LatteIndexType::U32_BE || t == LatteIndexType::U32_LE; }

	// true for modes that are rewritten into another primitive type on the host
	bool NeedsRewrite(LattePrimitiveMode mode)
	{
		if (mode == LattePrimitiveMode::QUADS || mode == LattePrimitiveMode::QUAD_STRIP || mode == LattePrimitiveMode::LINE_LOOP)
			return true;
		return mode == LattePrimitiveMode::TRIANGLE_FAN && g_renderer->GetType() == RendererAPI::Metal;
	}

	// number of host indices a translation writes (before choosing the element size)
	uint32 HostIndexCount(LattePrimitiveMode mode, uint32 count)
	{
		switch (mode)
		{
		case LattePrimitiveMode::QUADS:
			return (count / 4) * 6;
		case LattePrimitiveMode::QUAD_STRIP:
			return count < 4 ? 0 : ((count - 2) / 2) * 6;
		case LattePrimitiveMode::LINE_LOOP:
			return count + 1;
		default:
			return count;
		}
	}

	// bytes of renderer memory needed, 0 when the draw can use the game's vertices directly
	uint32 HostBufferSize(LattePrimitiveMode mode, LatteIndexType type, uint32 count)
	{
		if (NeedsRewrite(mode))
		{
			if (mode == LattePrimitiveMode::QUAD_STRIP && count < 4)
				return 0;
			uint32 n = HostIndexCount(mode, count);
			if (type == LatteIndexType::AUTO)
			{
				// the element size is sized for the written count for line loops; generation
				// itself picks 16 bit from the source count, so a 65535 vertex loop reserves 32 bit room
				uint32 sizingCount = mode == LattePrimitiveMode::LINE_LOOP ? n : count;
				return n * (sizingCount <= 0xFFFF ? 2u : 4u);
			}
			if (IsU16(type))
				return n * 2;
			if (IsU32(type))
				return n * 4;
			cemu_assert_suspicious();
			return 0;
		}
		if (IsU16(type))
			return count * 2;
		if (IsU32(type))
			return count * 4;
		return 0;
	}

	// Reads index i of the game's buffer (big endian) or, for AUTO, produces i.
	template<typename T, bool Auto>
	struct IndexSource
	{
		const betype<T>* data;
		uint32 operator[](uint32 i) const
		{
			if constexpr (Auto)
				return i;
			else
				return (uint32)(T)data[i];
		}
	};

	// Writes the host indices for one of the rewritten primitive modes.
	// For AUTO sources the maximum is simply count-1, otherwise it is tracked.
	template<typename T, bool Auto>
	void Rewrite(LattePrimitiveMode mode, const void* input, void* output, uint32 count, uint32& maxIndex)
	{
		IndexSource<T, Auto> in{ (const betype<T>*)input };
		T* out = (T*)output;
		uint32 hi = maxIndex;
		auto put = [&](uint32 v) { *out++ = (T)v; };
		auto track = [&](uint32 v) { if (v > hi) hi = v; };
		switch (mode)
		{
		case LattePrimitiveMode::QUADS:
			// a b c d -> (a b c) (a c d)
			for (uint32 base = 0; base + 4 <= count; base += 4)
			{
				uint32 a = in[base], b = in[base + 1], c = in[base + 2], d = in[base + 3];
				if constexpr (!Auto) { track(a); track(b); track(c); track(d); }
				put(a); put(b); put(c); put(a); put(c); put(d);
			}
			break;
		case LattePrimitiveMode::QUAD_STRIP:
			// a b c d -> (a b c) (c b d), then advance by two vertices
			if (count < 4)
				return;
			for (uint32 q = 0, quads = (count - 2) / 2; q < quads; q++)
			{
				uint32 base = q * 2;
				uint32 a = in[base], b = in[base + 1], c = in[base + 2], d = in[base + 3];
				if constexpr (!Auto) { track(a); track(b); track(c); track(d); }
				put(a); put(b); put(c); put(c); put(b); put(d);
			}
			break;
		case LattePrimitiveMode::LINE_LOOP:
			// line strip plus a closing vertex
			if (count == 0)
				return;
			for (uint32 i = 0; i < count; i++)
			{
				uint32 v = in[i];
				if constexpr (!Auto) track(v);
				put(v);
			}
			put(in[0]);
			break;
		default:
			// triangle fan (Metal): reorder into a strip that alternates between both ends
			for (uint32 i = 0; i < count; i++)
			{
				uint32 pos = (i & 1) ? (count - 1 - i / 2) : (i / 2);
				uint32 v;
				if constexpr (Auto)
					v = (uint32)(T)pos;
				else
					v = in[pos];
				if constexpr (!Auto) track(v);
				put(v);
			}
			break;
		}
		if constexpr (Auto)
			maxIndex = std::max(count, 1u) - 1;
		else
			maxIndex = hi;
	}

	// plain copy with byte swap (BE) or without (LE), tracking the maximum
	template<typename T, bool BigEndian>
	void CopyScalar(const void* input, void* output, uint32 count, uint32& maxIndex)
	{
		T* out = (T*)output;
		uint32 hi = maxIndex;
		for (uint32 i = 0; i < count; i++)
		{
			T v;
			if constexpr (BigEndian)
				v = ((const betype<T>*)input)[i];
			else
				v = ((const T*)input)[i];
			out[i] = v;
			if ((uint32)v > hi)
				hi = (uint32)v;
		}
		maxIndex = hi;
	}

#if defined(ARCH_X86_64)
	// byte shuffle masks that reverse every 16 or 32 bit element
	ATTRIBUTE_SSE41 __m128i SwapMask16() { return _mm_setr_epi8(1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14); }
	ATTRIBUTE_SSE41 __m128i SwapMask32() { return _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12); }

	template<typename T>
	ATTRIBUTE_SSE41 void CopyBE_SSE41(const void* input, void* output, uint32 count, uint32& maxIndex)
	{
		constexpr uint32 perVec = 16 / sizeof(T);
		const __m128i mask = sizeof(T) == 2 ? SwapMask16() : SwapMask32();
		__m128i vmax = _mm_setzero_si128();
		const uint8* src = (const uint8*)input;
		uint8* dst = (uint8*)output;
		uint32 vecs = count / perVec;
		for (uint32 n = 0; n < vecs; n++)
		{
			__m128i v = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(src + n * 16)), mask);
			_mm_storeu_si128((__m128i*)(dst + n * 16), v);
			if constexpr (sizeof(T) == 2)
				vmax = _mm_max_epu16(vmax, v);
			else
				vmax = _mm_max_epu32(vmax, v);
		}
		alignas(16) T lanes[perVec];
		_mm_store_si128((__m128i*)lanes, vmax);
		uint32 hi = maxIndex;
		for (uint32 i = 0; i < perVec; i++)
			hi = std::max(hi, (uint32)lanes[i]);
		uint32 done = vecs * perVec;
		CopyScalar<T, true>(src + done * sizeof(T), dst + done * sizeof(T), count - done, hi);
		maxIndex = hi;
	}

	template<typename T>
	ATTRIBUTE_AVX2 void CopyBE_AVX2(const void* input, void* output, uint32 count, uint32& maxIndex)
	{
		constexpr uint32 perVec = 32 / sizeof(T);
		const __m256i mask = sizeof(T) == 2 ?
			_mm256_setr_epi8(1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14, 1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14) :
			_mm256_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12, 3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
		__m256i vmax = _mm256_setzero_si256();
		const uint8* src = (const uint8*)input;
		uint8* dst = (uint8*)output;
		uint32 vecs = count / perVec;
		for (uint32 n = 0; n < vecs; n++)
		{
			__m256i v = _mm256_shuffle_epi8(_mm256_loadu_si256((const __m256i*)(src + n * 32)), mask);
			_mm256_storeu_si256((__m256i*)(dst + n * 32), v);
			if constexpr (sizeof(T) == 2)
				vmax = _mm256_max_epu16(vmax, v);
			else
				vmax = _mm256_max_epu32(vmax, v);
		}
		alignas(32) T lanes[perVec];
		_mm256_store_si256((__m256i*)lanes, vmax);
		uint32 hi = maxIndex;
		for (uint32 i = 0; i < perVec; i++)
			hi = std::max(hi, (uint32)lanes[i]);
		uint32 done = vecs * perVec;
		CopyScalar<T, true>(src + done * sizeof(T), dst + done * sizeof(T), count - done, hi);
		maxIndex = hi;
	}
#elif defined(__aarch64__)
	template<typename T>
	void CopyBE_NEON(const void* input, void* output, uint32 count, uint32& maxIndex)
	{
		constexpr uint32 perVec = 16 / sizeof(T);
		const uint8* src = (const uint8*)input;
		uint8* dst = (uint8*)output;
		uint32 vecs = count / perVec;
		uint32 hi = maxIndex;
		if constexpr (sizeof(T) == 2)
		{
			uint16x8_t vmax = vdupq_n_u16(0);
			for (uint32 n = 0; n < vecs; n++)
			{
				uint16x8_t v = vreinterpretq_u16_u8(vrev16q_u8(vld1q_u8(src + n * 16)));
				vst1q_u16((uint16*)(dst + n * 16), v);
				vmax = vmaxq_u16(vmax, v);
			}
			hi = std::max(hi, (uint32)vmaxvq_u16(vmax));
		}
		else
		{
			uint32x4_t vmax = vdupq_n_u32(0);
			for (uint32 n = 0; n < vecs; n++)
			{
				uint32x4_t v = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(src + n * 16)));
				vst1q_u32((uint32*)(dst + n * 16), v);
				vmax = vmaxq_u32(vmax, v);
			}
			hi = std::max(hi, (uint32)vmaxvq_u32(vmax));
		}
		uint32 done = vecs * perVec;
		CopyScalar<T, true>(src + done * sizeof(T), dst + done * sizeof(T), count - done, hi);
		maxIndex = hi;
	}
#endif

	template<typename T>
	void CopyBE(const void* input, void* output, uint32 count, uint32& maxIndex)
	{
#if defined(ARCH_X86_64)
		if (g_CPUFeatures.x86.avx2)
			return CopyBE_AVX2<T>(input, output, count, maxIndex);
		if (g_CPUFeatures.x86.sse4_1 && g_CPUFeatures.x86.ssse3)
			return CopyBE_SSE41<T>(input, output, count, maxIndex);
		CopyScalar<T, true>(input, output, count, maxIndex);
#elif defined(__aarch64__)
		CopyBE_NEON<T>(input, output, count, maxIndex);
#else
		CopyScalar<T, true>(input, output, count, maxIndex);
#endif
	}

	// Highest index of the game's buffer that is not the primitive restart index.
	// Only used when the fast scan above ended on the restart value. The first
	// index seeds the result, as the hardware path never sees an empty buffer here.
	template<typename T>
	uint32 MaxIgnoringRestart(const void* input, uint32 count, uint32 restart)
	{
		const betype<T>* idx = (const betype<T>*)input;
		cemu_assert_debug(restart <= std::numeric_limits<T>::max());
		T skip = (T)restart;
		T hi = idx[0];
		for (uint32 i = 0; i < count; i++)
		{
			T v = idx[i];
			if (v != skip && v > hi)
				hi = v;
		}
		return hi;
	}

	void RecomputeMaxIgnoringRestart(const void* input, LatteIndexType type, uint32 count, uint32& maxIndex)
	{
		if (count == 0)
		{
			maxIndex = 0;
			return;
		}
		uint32 restart = LatteGPUState.contextNew.VGT_MULTI_PRIM_IB_RESET_INDX.get_RESTART_INDEX();
		if (type == LatteIndexType::U16_BE)
			maxIndex = MaxIgnoringRestart<uint16>(input, count, restart);
		else if (type == LatteIndexType::U32_BE)
			maxIndex = MaxIgnoringRestart<uint32>(input, count, restart);
		else
			cemu_assert_debug(false);
	}

	// Translate one buffer into renderer memory. Returns the host index type.
	void Translate(const void* input, LatteIndexType type, uint32 count, LattePrimitiveMode mode, void* output, uint32& maxIndex, Renderer::INDEX_TYPE& hostType)
	{
		if (NeedsRewrite(mode))
		{
			if (type == LatteIndexType::AUTO)
			{
				if (count <= 0xFFFF)
				{
					Rewrite<uint16, true>(mode, input, output, count, maxIndex);
					hostType = Renderer::INDEX_TYPE::U16;
				}
				else
				{
					Rewrite<uint32, true>(mode, input, output, count, maxIndex);
					hostType = Renderer::INDEX_TYPE::U32;
				}
			}
			else if (type == LatteIndexType::U16_BE)
				Rewrite<uint16, false>(mode, input, output, count, maxIndex);
			else if (type == LatteIndexType::U32_BE)
				Rewrite<uint32, false>(mode, input, output, count, maxIndex);
			else
				cemu_assert_debug(false); // little endian sources are not rewritten
			return;
		}
		switch (type)
		{
		case LatteIndexType::U16_BE: CopyBE<uint16>(input, output, count, maxIndex); break;
		case LatteIndexType::U32_BE: CopyBE<uint32>(input, output, count, maxIndex); break;
		case LatteIndexType::U16_LE: CopyScalar<uint16, false>(input, output, count, maxIndex); break;
		case LatteIndexType::U32_LE: CopyScalar<uint32, false>(input, output, count, maxIndex); break;
		default: cemu_assert_debug(false); break;
		}
	}
}

void LatteIndices_invalidate(const void* memPtr, uint32 size)
{
	const uint8* begin = (const uint8*)memPtr;
	const uint8* end = begin + size;
	for (auto& slot : s_translated)
	{
		const uint8* p = (const uint8*)slot.source;
		if (p >= begin && p < end)
			ReleaseSlot(slot);
	}
}

void LatteIndices_invalidateAll()
{
	for (auto& slot : s_translated)
		ReleaseSlot(slot);
}

void LatteIndices_decode(const void* indexData, LatteIndexType indexType, uint32 count, LattePrimitiveMode primitiveMode, uint32& indexMax, Renderer::INDEX_TYPE& renderIndexType, uint32& outputCount, Renderer::IndexAllocation& indexAllocation)
{
	// already translated?
	for (auto& slot : s_translated)
	{
		if (slot.source == indexData && slot.sourceCount == count && slot.mode == primitiveMode && slot.type == indexType)
		{
			indexMax = slot.maxIndex;
			renderIndexType = slot.hostType;
			outputCount = slot.hostCount;
			indexAllocation = slot.alloc;
			slot.stamp = s_stampCounter++;
			return;
		}
	}

	outputCount = 0;
	if (indexType == LatteIndexType::AUTO)
		renderIndexType = Renderer::INDEX_TYPE::NONE;
	else if (IsU16(indexType))
		renderIndexType = Renderer::INDEX_TYPE::U16;
	else if (indexType == LatteIndexType::U32_BE)
		renderIndexType = Renderer::INDEX_TYPE::U32;
	else
		cemu_assert_debug(false);

	const uint32 restartIndex = LatteGPUState.contextNew.VGT_MULTI_PRIM_IB_RESET_INDX.get_RESTART_INDEX();
	const uint32 byteSize = HostBufferSize(primitiveMode, indexType, count);
	if (byteSize == 0)
	{
		// draw straight from the vertex stream
		outputCount = count;
		indexMax = std::max(count, 1u) - 1;
		renderIndexType = Renderer::INDEX_TYPE::NONE;
		indexAllocation = {};
		return;
	}

	indexAllocation = g_renderer->indexData_reserveIndexMemory(byteSize);
	indexMax = 0;
	Translate(indexData, indexType, count, primitiveMode, indexAllocation.mem, indexMax, renderIndexType);
	outputCount = HostIndexCount(primitiveMode, count);

	// the scan above does not skip the restart index; if that is what it found, scan again without it
	if (indexMax == restartIndex)
		RecomputeMaxIgnoringRestart(indexData, indexType, count, indexMax);

	g_renderer->indexData_uploadIndexMemory(indexAllocation);
	performanceMonitor.cycle[performanceMonitor.cycleIndex].indexDataUploaded += byteSize;

	// remember the result in the slot that was used longest ago
	TranslatedIndexBuffer* victim = &s_translated[0];
	for (auto& slot : s_translated)
	{
		if (slot.stamp < victim->stamp)
			victim = &slot;
	}
	if (victim->source != nullptr)
		g_renderer->indexData_releaseIndexMemory(victim->alloc);
	victim->source = indexData;
	victim->sourceCount = count;
	victim->mode = primitiveMode;
	victim->type = indexType;
	victim->maxIndex = indexMax;
	victim->hostType = renderIndexType;
	victim->hostCount = outputCount;
	victim->alloc = indexAllocation;
	victim->stamp = s_stampCounter++;
}
