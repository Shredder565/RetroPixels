// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/HW/Latte/Core/LatteConst.h"
#include "Cafe/HW/Latte/Core/LatteShaderAssembly.h"
#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LatteDraw.h"
#include "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompiler.h"
#include "Cafe/HW/Latte/Core/FetchShader.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "util/helpers/StringBuf.h"

// Vertex attribute unpacking for the GLSL vertex shader.
//
// The raw attribute arrives as uvec4 attrDataSem<semanticId> holding the
// unconverted 8/16/32-bit lanes as the vertex input stage fetched them. The
// generated code converts it into the 32-bit register image that the
// Latte vertex fetch would produce and leaves it in 'attrDecoder' (uvec4):
//  - optional byte swap (the console stores most vertex data big endian)
//  - optional sign extension of the narrow lanes
//  - number format: NFA 0 = normalized, 1 = integer, 2 = scaled (int -> float)

namespace
{
	// note: always append through string_view, StringBuf only grows on that path
	constexpr std::string_view kLineEnd = "\r\n";
	constexpr const char kLane[4] = { 'x', 'y', 'z', 'w' };

	// swizzle selecting the first n lanes
	std::string_view lanes(uint32 n)
	{
		static constexpr std::string_view kMasks[5] = { "", "x", "xy", "xyz", "xyzw" };
		return kMasks[n];
	}

	enum class SignReq { Any, Unsigned, Signed, ExactlyOne };

	bool signMatches(SignReq req, uint8 isSigned)
	{
		switch (req)
		{
		case SignReq::Unsigned: return isSigned == 0;
		case SignReq::Signed: return isSigned != 0;
		case SignReq::ExactlyOne: return isSigned == 1;
		default: return true;
		}
	}

	class AttrCodeWriter
	{
	public:
		AttrCodeWriter(StringBuf* out, uint32 semanticId) : m_out(out), m_sem(semanticId) {}

		void line(std::string_view text)
		{
			m_out->add(text);
			m_out->add(kLineEnd);
		}

		template<typename... T>
		void linef(fmt::format_string<T...> f, T&&... args)
		{
			line(fmt::format(f, std::forward<T>(args)...));
		}

		// --- loads of the raw lanes ---

		// whole vector copied as-is
		void copyAll() { linef("attrDecoder = attrDataSem{};", m_sem); }

		// first n lanes copied, the rest zeroed, as one constructor expression
		void copyPadded(uint32 n)
		{
			std::string zeros;
			for (uint32 i = n; i < 4; i++)
				zeros.append(",0");
			linef("attrDecoder = uvec4(attrDataSem{}.{}{});", m_sem, lanes(n), zeros);
		}

		// lanes assigned through a swizzle: attrDecoder.<dst> = attrDataSem.<src>
		void copySwizzled(std::string_view dst, std::string_view src) { linef("attrDecoder.{} = attrDataSem{}.{};", dst, m_sem, src); }

		// --- byte order ---

		void swap32(std::string_view sel)
		{
			std::string v = sel.empty() ? std::string("attrDecoder") : fmt::format("attrDecoder.{}", sel);
			linef("{0} = ({0}>>24)|(({0}>>8)&0xFF00)|(({0}<<8)&0xFF0000)|(({0}<<24));", v);
		}

		void swap16(std::string_view sel)
		{
			std::string v = sel.empty() ? std::string("attrDecoder") : fmt::format("attrDecoder.{}", sel);
			linef("{0} = (({0}>>8)&0xFF)|(({0}<<8)&0xFF00);", v);
		}

		// zero lanes [first, 4), one statement each
		void zeroFrom(uint32 first)
		{
			for (uint32 i = first; i < 4; i++)
				linef("attrDecoder.{} = 0;", kLane[i]);
		}

		// big endian 32-bit lanes: n lanes loaded and swapped, rest cleared
		void bigEndian32(uint32 n)
		{
			if (n == 4)
			{
				copyAll();
				swap32("");
				return;
			}
			copySwizzled(lanes(n), lanes(n));
			swap32(lanes(n));
			zeroFrom(n);
		}

		// big endian 16-bit lanes
		void bigEndian16(uint32 n)
		{
			if (n == 4)
			{
				copySwizzled("xyzw", "xyzw");
				swap16("");
			}
			else if (n == 2)
			{
				copySwizzled("xy", "xy");
				swap16("xy");
				zeroFrom(2);
			}
			else
			{
				copySwizzled("xy", "xy");
				swap16("x");
				zeroFrom(1);
			}
		}

		// --- per-lane conversions on lanes [0, n) ---

		// sign extension of a 'bits' wide value
		void signExtend(uint32 n, uint32 bits)
		{
			const uint32 signBit = 1u << (bits - 1);
			const uint32 fill = ~((1u << bits) - 1u);
			for (uint32 i = 0; i < n; i++)
				linef("if( (attrDecoder.{0}&0x{1:X}) != 0 ) attrDecoder.{0} |= 0x{2:X};", kLane[i], signBit, fill);
		}

		// signed normalized: value / max, clamped to -1
		void snorm(uint32 n, std::string_view divisor)
		{
			for (uint32 i = 0; i < n; i++)
				linef("attrDecoder.{0} = floatBitsToUint(max(float(int(attrDecoder.{0}))/{1},-1.0));", kLane[i], divisor);
		}

		// unsigned normalized through an int conversion
		void unormViaInt(uint32 n, std::string_view divisor)
		{
			for (uint32 i = 0; i < n; i++)
				linef("attrDecoder.{0} = floatBitsToUint(float(int(attrDecoder.{0}))/{1});", kLane[i], divisor);
		}

		// signed integer to float
		void intToFloat(uint32 n)
		{
			for (uint32 i = 0; i < n; i++)
				linef("attrDecoder.{0} = floatBitsToUint(float(int(attrDecoder.{0})));", kLane[i]);
		}

		// splits a packed 10:10:10:2 value in lane x into four lanes
		void unpack1010102()
		{
			line("attrDecoder.xyzw = uvec4((attrDecoder.x>>0)&0x3FF,(attrDecoder.x>>10)&0x3FF,(attrDecoder.x>>20)&0x3FF,(attrDecoder.x>>30)&0x3);");
		}

		void normalizeAlpha2Bit() { line("attrDecoder.w = floatBitsToUint(float(attrDecoder.w) * (1.0 / 3.0));"); }

		void halfFloat4() { line("attrDecoder.xyzw = floatBitsToInt(vec4(unpackHalf2x16(attrDecoder.x|(attrDecoder.y<<16)),unpackHalf2x16(attrDecoder.z|(attrDecoder.w<<16))));"); }

		void halfFloat2()
		{
			line("attrDecoder.xy = floatBitsToUint(unpackHalf2x16(attrDecoder.x|(attrDecoder.y<<16)));");
			line("attrDecoder.zw = uvec2(0);");
		}

		uint32 sem() const { return m_sem; }

	private:
		StringBuf* m_out;
		uint32 m_sem;
	};

	using Writer = void (*)(AttrCodeWriter& w, const LatteParsedFetchShaderAttribute& a);

	struct FormatRule
	{
		Latte::E_HWFMT format;
		uint8 nfa;
		SignReq sign;
		Writer emit;
	};

	using F = Latte::E_HWFMT;

	// ---- 32-bit byte swapped data (the common case for console vertex data) ----
	const FormatRule kRulesSwap32[] =
	{
		{ F::HWFMT_32_32_32_32_FLOAT, 2, SignReq::Any, [](AttrCodeWriter& w, const LatteParsedFetchShaderAttribute&) { w.bigEndian32(4); } },
		{ F::HWFMT_32_32_32_FLOAT, 2, SignReq::Any, [](AttrCodeWriter& w, const LatteParsedFetchShaderAttribute&) { w.bigEndian32(3); } },
		{ F::HWFMT_32_32_FLOAT, 2, SignReq::Any, [](AttrCodeWriter& w, const LatteParsedFetchShaderAttribute&) { w.bigEndian32(2); } },
		{ F::HWFMT_32_FLOAT, 2, SignReq::Any, [](AttrCodeWriter& w, const LatteParsedFetchShaderAttribute&) { w.bigEndian32(1); } },
		{ F::HWFMT_2_10_10_10, 0, SignReq::Any, [](AttrCodeWriter& w, const LatteParsedFetchShaderAttribute& a) {
			w.bigEndian32(1);
			w.unpack1010102();
			if (a.isSigned != 0)
			{
				w.signExtend(3, 10);
				w.snorm(3, "511.0");
			}
			else
				w.snorm(3, "1023.0");
			w.normalizeAlpha2Bit();
		} },
		{ F::HWFMT_32_32_32_32, 1, SignReq::Unsigned, [](AttrCodeWriter& w, const LatteParsedFetchShaderAttribute&) { w.bigEndian32(4); } },
		{ F::HWFMT_32_32_32, 1, SignReq::Unsigned, [](AttrCodeWriter& w, const LatteParsedFetchShaderAttribute&) { w.bigEndian32(3); } },
		{ F::HWFMT_32_32, 1, SignReq::Unsigned, [](AttrCodeWriter& w, const LatteParsedFetchShaderAttribute&) { w.bigEndian32(2); } },
		{ F::HWFMT_32, 1, SignReq::Unsigned, [](AttrCodeWriter& w, const LatteParsedFetchShaderAttribute&) { w.bigEndian32(1); } },
		// a signed 32-bit integer needs no extension, the bit pattern is used as-is
		{ F::HWFMT_32, 1, SignReq::ExactlyOne, [](AttrCodeWriter& w, const LatteParsedFetchShaderAttribute&) { w.bigEndian32(1); } },
		// byte data in a 32-bit swapped stream: the swap reverses the lane order
		{ F::HWFMT_8_8_8_8, 0, SignReq::Unsigned, [](AttrCodeWriter& w, const LatteParsedFetchShaderAttribute&) {
			w.linef("attrDecoder.xyzw = floatBitsToUint(vec4(attrDataSem{}.wzyx)/255.0);", w.sem());
		} },
		{ F::HWFMT_8_8_8_8, 0, SignReq::Signed, [](AttrCodeWriter& w, const LatteParsedFetchShaderAttribute&) {
			w.copySwizzled("xyzw", "wzyx");
			w.signExtend(4, 8);
			w.snorm(4, "127.0");
		} },
		{ F::HWFMT_8_8_8_8, 1, SignReq::Unsigned, [](AttrCodeWriter& w, const LatteParsedFetchShaderAttribute&) { w.copySwizzled("xyzw", "wzyx"); } },
		{ F::HWFMT_8_8_8_8, 2, SignReq::Unsigned, [](AttrCodeWriter& w, const LatteParsedFetchShaderAttribute&) {
			w.linef("attrDecoder.xyzw = floatBitsToUint(vec4(attrDataSem{}.wzyx));", w.sem());
		} },
	};

	// two-byte attributes at offset 2 of a dword are mis-fetched by AMD's GL
	// driver; the lanes then show up in .zw of the raw vector
	bool needsAmdByteOffsetWorkaround(const LatteParsedFetchShaderAttribute& a)
	{
		return (a.offset & 3) == 2 && LatteGPUState.glVendor == GLVENDOR_AMD && g_renderer->GetType() == RendererAPI::OpenGL;
	}

	std::string_view rg8Source(const LatteParsedFetchShaderAttribute& a)
	{
		return needsAmdByteOffsetWorkaround(a) ? "zw" : "xy";
	}

	// ---- native (little endian) data ----
	const FormatRule kRulesNoSwap[] =
	{
		{ F::HWFMT_32_32_32_32_FLOAT, 2, SignReq::Any, [](AttrCodeWriter& w, const LatteParsedFetchShaderAttribute&) { w.copyAll(); } },
		{ F::HWFMT_32_32_32_FLOAT, 2, SignReq::Any, [](AttrCodeWriter& w, const LatteParsedFetchShaderAttribute&) { w.copyPadded(3); } },
		{ F::HWFMT_32_32_FLOAT, 2, SignReq::Any, [](AttrCodeWriter& w, const LatteParsedFetchShaderAttribute&) { w.copyPadded(2); } },
		{ F::HWFMT_32, 1, SignReq::Unsigned, [](AttrCodeWriter& w, const LatteParsedFetchShaderAttribute&) { w.copyPadded(1); } },
		{ F::HWFMT_2_10_10_10, 0, SignReq::Unsigned, [](AttrCodeWriter& w, const LatteParsedFetchShaderAttribute&) {
			w.copyPadded(1);
			w.unpack1010102();
			w.snorm(3, "1023.0");
			w.normalizeAlpha2Bit();
		} },
		{ F::HWFMT_16_16_16_16, 0, SignReq::Signed, [](AttrCodeWriter& w, const LatteParsedFetchShaderAttribute&) {
			w.copyAll();
			w.signExtend(4, 16);
			w.snorm(4, "32767.0");
		} },
		{ F::HWFMT_16_16_16_16, 2, SignReq::ExactlyOne, [](AttrCodeWriter& w, const LatteParsedFetchShaderAttribute&) {
			w.copyAll();
			w.signExtend(4, 16);
			w.line("attrDecoder.xyzw = floatBitsToUint(vec4(ivec4(attrDecoder)));");
		} },
		{ F::HWFMT_16_16_16_16_FLOAT, 2, SignReq::Any, [](AttrCodeWriter& w, const LatteParsedFetchShaderAttribute&) {
			w.copyAll();
			w.halfFloat4();
		} },
		{ F::HWFMT_16_16, 0, SignReq::Signed, [](AttrCodeWriter& w, const LatteParsedFetchShaderAttribute&) {
			w.copyPadded(2);
			w.signExtend(2, 16);
			w.snorm(2, "32767.0");
		} },
		{ F::HWFMT_16_16_FLOAT, 2, SignReq::Any, [](AttrCodeWriter& w, const LatteParsedFetchShaderAttribute&) {
			w.copyPadded(2);
			w.halfFloat2();
		} },
		{ F::HWFMT_8_8_8_8, 0, SignReq::Unsigned, [](AttrCodeWriter& w, const LatteParsedFetchShaderAttribute&) {
			w.linef("attrDecoder.xyzw = floatBitsToUint(vec4(attrDataSem{}.xyzw)/255.0);", w.sem());
		} },
		{ F::HWFMT_8_8_8_8, 0, SignReq::Signed, [](AttrCodeWriter& w, const LatteParsedFetchShaderAttribute&) {
			w.copySwizzled("xyzw", "xyzw");
			w.signExtend(4, 8);
			w.snorm(4, "127.0");
		} },
		{ F::HWFMT_8_8_8_8, 1, SignReq::Unsigned, [](AttrCodeWriter& w, const LatteParsedFetchShaderAttribute&) { w.copySwizzled("xyzw", "xyzw"); } },
		{ F::HWFMT_8_8_8_8, 1, SignReq::Signed, [](AttrCodeWriter& w, const LatteParsedFetchShaderAttribute&) {
			w.copySwizzled("xyzw", "xyzw");
			w.signExtend(4, 8);
		} },
		{ F::HWFMT_8_8_8_8, 2, SignReq::Unsigned, [](AttrCodeWriter& w, const LatteParsedFetchShaderAttribute&) {
			w.linef("attrDecoder.xyzw = floatBitsToInt(vec4(attrDataSem{}.xyzw));", w.sem());
		} },
		{ F::HWFMT_8_8, 0, SignReq::Unsigned, [](AttrCodeWriter& w, const LatteParsedFetchShaderAttribute& a) {
			w.linef("attrDecoder.xy = floatBitsToUint(vec2(attrDataSem{}.{})/255.0);", w.sem(), rg8Source(a));
			w.line("attrDecoder.zw = uvec2(0);");
		} },
		{ F::HWFMT_8_8, 2, SignReq::Unsigned, [](AttrCodeWriter& w, const LatteParsedFetchShaderAttribute& a) {
			w.linef("attrDecoder.xy = floatBitsToUint(vec2(attrDataSem{}.{}));", w.sem(), rg8Source(a));
			w.line("attrDecoder.zw = uvec2(0);");
		} },
		{ F::HWFMT_8_8, 0, SignReq::Signed, [](AttrCodeWriter& w, const LatteParsedFetchShaderAttribute& a) {
			w.copySwizzled("xy", rg8Source(a));
			w.signExtend(2, 8);
			w.snorm(2, "127.0");
			w.line("attrDecoder.zw = uvec2(0);");
		} },
		{ F::HWFMT_8_8, 1, SignReq::Unsigned, [](AttrCodeWriter& w, const LatteParsedFetchShaderAttribute& a) {
			w.linef("attrDecoder.xyzw = uvec4(attrDataSem{}.{},0,0);", w.sem(), rg8Source(a));
		} },
		{ F::HWFMT_8, 0, SignReq::Unsigned, [](AttrCodeWriter& w, const LatteParsedFetchShaderAttribute&) {
			w.linef("attrDecoder.x = floatBitsToUint(float(attrDataSem{}.x)/255.0);", w.sem());
			w.line("attrDecoder.yzw = uvec3(0);");
		} },
		{ F::HWFMT_8, 1, SignReq::Unsigned, [](AttrCodeWriter& w, const LatteParsedFetchShaderAttribute&) {
			w.linef("attrDecoder.xyzw = uvec4(attrDataSem{}.x,0,0,0);", w.sem());
		} },
	};

	// ---- 16-bit byte swapped data ----
	const FormatRule kRulesSwap16[] =
	{
		{ F::HWFMT_16_16_16_16_FLOAT, 2, SignReq::Any, [](AttrCodeWriter& w, const LatteParsedFetchShaderAttribute&) {
			w.bigEndian16(4);
			w.halfFloat4();
		} },
		{ F::HWFMT_16_16_16_16, 0, SignReq::Signed, [](AttrCodeWriter& w, const LatteParsedFetchShaderAttribute&) {
			w.bigEndian16(4);
			w.signExtend(4, 16);
			w.snorm(4, "32767.0");
		} },
		{ F::HWFMT_16_16_16_16, 0, SignReq::Unsigned, [](AttrCodeWriter& w, const LatteParsedFetchShaderAttribute&) {
			w.bigEndian16(4);
			w.unormViaInt(4, "65535.0");
		} },
		{ F::HWFMT_16_16_16_16, 2, SignReq::Signed, [](AttrCodeWriter& w, const LatteParsedFetchShaderAttribute&) {
			w.bigEndian16(4);
			w.signExtend(4, 16);
			w.intToFloat(4);
		} },
		{ F::HWFMT_16_16_16_16, 1, SignReq::Signed, [](AttrCodeWriter& w, const LatteParsedFetchShaderAttribute&) {
			w.bigEndian16(4);
			w.signExtend(4, 16);
		} },
		{ F::HWFMT_16_16_16_16, 1, SignReq::Unsigned, [](AttrCodeWriter& w, const LatteParsedFetchShaderAttribute&) { w.bigEndian16(4); } },
		{ F::HWFMT_16_16_FLOAT, 2, SignReq::Any, [](AttrCodeWriter& w, const LatteParsedFetchShaderAttribute&) {
			w.bigEndian16(2);
			w.halfFloat2();
		} },
		{ F::HWFMT_16_16, 0, SignReq::Unsigned, [](AttrCodeWriter& w, const LatteParsedFetchShaderAttribute&) {
			w.bigEndian16(2);
			w.line("attrDecoder.xy = floatBitsToUint(vec2(float(attrDecoder.x), float(attrDecoder.y))/65535.0);");
			w.line("attrDecoder.zw = uvec2(0);");
		} },
		{ F::HWFMT_16_16, 0, SignReq::Signed, [](AttrCodeWriter& w, const LatteParsedFetchShaderAttribute&) {
			w.bigEndian16(2);
			w.signExtend(2, 16);
			w.snorm(2, "32767.0");
			w.line("attrDecoder.zw = uvec2(0);");
		} },
		{ F::HWFMT_16_16, 1, SignReq::Unsigned, [](AttrCodeWriter& w, const LatteParsedFetchShaderAttribute&) { w.bigEndian16(2); } },
		{ F::HWFMT_16_16, 1, SignReq::Signed, [](AttrCodeWriter& w, const LatteParsedFetchShaderAttribute&) {
			w.bigEndian16(2);
			w.signExtend(2, 16);
			w.line("attrDecoder.zw = uvec2(0);");
		} },
		{ F::HWFMT_16_16, 2, SignReq::Unsigned, [](AttrCodeWriter& w, const LatteParsedFetchShaderAttribute&) {
			w.bigEndian16(2);
			w.line("attrDecoder.xy = floatBitsToUint(vec2(float(attrDecoder.x), float(attrDecoder.y)));");
			w.line("attrDecoder.zw = uvec2(0);");
		} },
		{ F::HWFMT_16_16, 2, SignReq::Signed, [](AttrCodeWriter& w, const LatteParsedFetchShaderAttribute&) {
			w.bigEndian16(2);
			w.signExtend(2, 16);
			w.line("attrDecoder.xy = floatBitsToUint(vec2(float(int(attrDecoder.x)), float(int(attrDecoder.y))));");
			w.line("attrDecoder.zw = uvec2(0);");
		} },
		{ F::HWFMT_16, 1, SignReq::Unsigned, [](AttrCodeWriter& w, const LatteParsedFetchShaderAttribute&) { w.bigEndian16(1); } },
		{ F::HWFMT_16, 0, SignReq::Unsigned, [](AttrCodeWriter& w, const LatteParsedFetchShaderAttribute&) {
			w.bigEndian16(1);
			w.unormViaInt(1, "65535.0");
		} },
	};

	template<size_t N>
	bool applyRule(const FormatRule (&rules)[N], AttrCodeWriter& w, const LatteParsedFetchShaderAttribute& a)
	{
		for (const FormatRule& r : rules)
		{
			if (r.format == a.format && r.nfa == a.nfa && signMatches(r.sign, a.isSigned))
			{
				r.emit(w, a);
				return true;
			}
		}
		return false;
	}
}

void LatteDecompiler_emitAttributeDecodeGLSL(LatteDecompilerShader* shaderContext, StringBuf* src, LatteParsedFetchShaderAttribute* attrib)
{
	if (attrib->attributeBufferIndex >= Latte::GPU_LIMITS::NUM_VERTEX_BUFFERS)
	{
		// attribute reads from a buffer slot that does not exist
		src->add(std::string_view("attrDecoder = ivec4(0);"));
		src->add(kLineEnd);
		return;
	}
	AttrCodeWriter w(src, attrib->semanticId);
	switch (attrib->endianSwap)
	{
	case LatteConst::VertexFetchEndianMode::SWAP_U32:
		if (!applyRule(kRulesSwap32, w, *attrib))
		{
			cemuLog_log(LogType::Force, "_emitAttributeDecodeGLSL(): Unsupported fmt {:02x} nfa {} signed {} endian {}\n", attrib->format, attrib->nfa, attrib->isSigned, attrib->endianSwap);
			cemu_assert_unimplemented();
		}
		break;
	case LatteConst::VertexFetchEndianMode::SWAP_NONE:
		if (!applyRule(kRulesNoSwap, w, *attrib))
		{
			cemuLog_log(LogType::Force, "_emitAttributeDecodeGLSL(): Unsupported fmt {:02x} nfa {} signed {} endian {}\n", attrib->format, attrib->nfa, attrib->isSigned, attrib->endianSwap);
			cemu_assert_debug(false);
		}
		break;
	case LatteConst::VertexFetchEndianMode::SWAP_U16:
		if (!applyRule(kRulesSwap16, w, *attrib))
			cemuLog_logDebug(LogType::Force, "_emitAttributeDecodeGLSL(): Unsupported fmt {:02x} nfa {} signed {} endian {}", attrib->format, attrib->nfa, attrib->isSigned, attrib->endianSwap);
		break;
	default:
		cemu_assert_debug(false);
		break;
	}
}
