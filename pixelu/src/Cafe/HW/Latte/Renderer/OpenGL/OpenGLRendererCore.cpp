// Original code for RetroPixels (PixelU). Not derived from Cemu.
// OpenGL draw path: index cache, vertex attribute setup, RECT emulation, per-draw uniforms and the GL
// sampler state that lives in the texture objects.
#include "Common/GLInclude/GLInclude.h"

#include "Cafe/HW/Latte/Core/LatteDraw.h"
#include "Cafe/HW/Latte/Core/LattePerformanceMonitor.h"
#include "Cafe/HW/Latte/Core/LatteShader.h"
#include "Cafe/HW/Latte/Core/FetchShader.h"
#include "Cafe/HW/Latte/Core/LatteBufferCache.h"

#include "Cafe/HW/Latte/Renderer/OpenGL/OpenGLRenderer.h"
#include "Cafe/HW/Latte/Renderer/OpenGL/LatteTextureGL.h"
#include "Cafe/HW/Latte/Renderer/OpenGL/LatteTextureViewGL.h"
#include "Cafe/HW/Latte/Renderer/OpenGL/RendererShaderGL.h"
#include "Cafe/HW/Latte/Renderer/RendererCore.h"

#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "Cafe/OS/libs/gx2/GX2.h"
#include "util/VirtualHeap/VirtualHeap.h"

#include <list>
#include <unordered_map>

using IndexType = Latte::LATTE_VGT_DMA_INDEX_TYPE::E_INDEX_TYPE;
using PrimType = Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE;

// GL primitive of the draw currently being issued
GLenum sGLActiveDrawMode = 0;

// set by LatteMRT when the current FBO has at least one usable attachment
extern bool hasValidFramebufferAttached;

// ---------------------------------------------------------------------------------------------------------------
// Index cache
//
// GPU7 indices are big endian, GL wants them native. Converted index lists stay in one large element array buffer.
// Entries are found by (address, count, restart index, primitive), revalidated with a cheap checksum and evicted
// when they were not used for 30 frames or when the buffer is full (least recently used first).
// ---------------------------------------------------------------------------------------------------------------

namespace
{
	constexpr uint32 kIndexBufferSize = 18 * 1024 * 1024;
	constexpr uint32 kIndexConversionScratchSize = 8 * 1024 * 1024;
	constexpr uint32 kIndexEntryMaxAgeFrames = 30;

	struct IndexKey
	{
		MPTR address;
		sint32 count;
		uint32 restartIndex;
		uint32 primitive;

		bool operator==(const IndexKey& other) const
		{
			return address == other.address && count == other.count && restartIndex == other.restartIndex && primitive == other.primitive;
		}
	};

	struct IndexKeyHasher
	{
		size_t operator()(const IndexKey& k) const
		{
			return (size_t)(((uint32)(k.address + k.count) ^ (k.address >> 16)) * 31u + k.primitive) ^ ((size_t)k.restartIndex << 7);
		}
	};

	struct IndexEntry
	{
		IndexKey key;
		IndexType type;
		uint32 sourceBytes;
		uint32 checksum;
		uint32 lowest;
		uint32 highest;
		uint32 lastUsedFrame;
		VirtualBufferHeapEntry_t* allocation;
	};

	// one cached index list as seen by the draw call
	struct ActiveIndices
	{
		uint32 lowest = 0;
		uint32 highest = 0;
		const void* bufferOffset = nullptr; // offset into the element array buffer, as GL expects it
	};

	class GLIndexCache
	{
	public:
		bool IsReady() const { return m_buffer != 0; }

		void Create()
		{
			glGenBuffers(1, &m_buffer);
			glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_buffer);
			glBufferData(GL_ELEMENT_ARRAY_BUFFER, kIndexBufferSize, nullptr, GL_DYNAMIC_DRAW);
			glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
			m_scratch.resize(kIndexConversionScratchSize);
			m_heap = virtualBufferHeap_create(kIndexBufferSize);
		}

		GLuint Buffer() const { return m_buffer; }

		void Prepare(MPTR address, IndexType type, sint32 count, uint32 primitive, ActiveIndices& out)
		{
			if (type == IndexType::AUTO)
			{
				// generated indices, the element array binding is left alone
				out.lowest = 0;
				out.highest = count - 1;
				return;
			}
			OpenGLRenderer::SetArrayElementBuffer(m_buffer);
			const IndexKey key{ address, count, LatteGPUState.contextNew.VGT_MULTI_PRIM_IB_RESET_INDX.get_RESTART_INDEX(), primitive };
			auto found = m_lookup.find(key);
			if (found != m_lookup.end())
			{
				IndexEntry& entry = *found->second;
				// the draw keeps the index range from before a re-conversion (long-standing behaviour, kept as is)
				Publish(entry, out);
				entry.lastUsedFrame = LatteGPUState.frameCounter;
				const uint32 checksum = Checksum(memory_getPointerFromPhysicalOffset(address), entry.sourceBytes);
				if (checksum != entry.checksum)
				{
					cemuLog_logDebug(LogType::Force, "IndexData hash changed");
					Convert(entry);
					entry.checksum = checksum;
				}
				m_usage.splice(m_usage.end(), m_usage, found->second); // now most recently used
				return;
			}

			const bool isShort = type == IndexType::U16_BE || type == IndexType::U16_LE;
			const uint32 byteSize = (uint32)count * (isShort ? 2u : 4u);
			VirtualBufferHeapEntry_t* allocation = virtualBufferHeap_allocate(m_heap, byteSize);
			while (allocation == nullptr)
			{
				EvictOldest(10);
				allocation = virtualBufferHeap_allocate(m_heap, byteSize);
				if (allocation == nullptr && m_usage.empty())
				{
					cemuLog_log(LogType::Force, "Unable to allocate entry in index cache");
					assert_dbg();
				}
			}
			IndexEntry entry{};
			entry.key = key;
			entry.type = type;
			entry.sourceBytes = byteSize;
			entry.checksum = Checksum(memory_getPointerFromPhysicalOffset(address), byteSize);
			entry.lastUsedFrame = LatteGPUState.frameCounter;
			entry.allocation = allocation;
			auto it = m_usage.insert(m_usage.end(), entry);
			m_lookup.emplace(key, it);
			Convert(*it);
			Publish(*it, out);
		}

		void DropUnused(uint32 currentFrame)
		{
			while (!m_usage.empty() && (currentFrame - m_usage.front().lastUsedFrame) >= kIndexEntryMaxAgeFrames)
				EvictOldest(1);
		}

	private:
		static void Publish(const IndexEntry& entry, ActiveIndices& out)
		{
			out.lowest = entry.lowest;
			out.highest = entry.highest;
			out.bufferOffset = (const void*)(size_t)entry.allocation->startOffset;
		}

		void EvictOldest(sint32 count)
		{
			for (; count > 0 && !m_usage.empty(); count--)
			{
				IndexEntry& oldest = m_usage.front();
				virtualBufferHeap_free(m_heap, oldest.allocation);
				m_lookup.erase(oldest.key);
				m_usage.pop_front();
			}
		}

		static uint32 LoadU32(const uint8* p)
		{
			uint32 v;
			memcpy(&v, p, sizeof(v));
			return v;
		}

		// sums four words at the start and four at the (word aligned) end; short lists sum their bytes
		static uint32 Checksum(const uint8* data, uint32 size)
		{
			uint32 sum = 0;
			if (size < 16)
			{
				for (uint32 i = 0; i < size; i++)
					sum += data[i];
				return sum;
			}
			const uint8* tail = data + ((size - 16) & ~3u);
			for (uint32 i = 0; i < 16; i += 4)
				sum += LoadU32(data + i) + LoadU32(tail + i);
			return sum;
		}

		// byte swaps the guest indices into scratch memory, records the index range and uploads the result
		void Convert(IndexEntry& entry)
		{
			const uint32 count = (uint32)entry.key.count;
			const uint32 restart = entry.key.restartIndex;
			const uint8* src = memory_getPointerFromPhysicalOffset(entry.key.address);
			cemu_assert_debug(count != 0);
			if (entry.type == IndexType::U16_BE)
			{
				const uint16be* in = reinterpret_cast<const uint16be*>(src);
				uint16* out = reinterpret_cast<uint16*>(m_scratch.data());
				const bool hasRestart = restart <= 0xFFFF;
				uint16 lo = 0xFFFF, hi = 0;
				for (uint32 i = 0; i < count; i++)
				{
					const uint16 v = in[i];
					out[i] = v;
					if (hasRestart && v == (uint16)restart)
						continue;
					lo = std::min(lo, v);
					hi = std::max(hi, v);
				}
				entry.lowest = lo;
				entry.highest = hi;
				glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, entry.allocation->startOffset, count * sizeof(uint16), m_scratch.data());
				performanceMonitor.cycle[performanceMonitor.cycleIndex].indexDataUploaded += count * sizeof(uint16);
			}
			else if (entry.type == IndexType::U32_BE)
			{
				const uint32be* in = reinterpret_cast<const uint32be*>(src);
				uint32* out = reinterpret_cast<uint32*>(m_scratch.data());
				// the range starts out at the first index, even when that one is the restart marker
				uint32 lo = in[0], hi = in[0];
				for (uint32 i = 0; i < count; i++)
				{
					const uint32 v = in[i];
					out[i] = v;
					if (v == restart)
						continue;
					lo = std::min(lo, v);
					hi = std::max(hi, v);
				}
				entry.lowest = lo;
				entry.highest = hi;
				glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, entry.allocation->startOffset, count * sizeof(uint32), m_scratch.data());
				performanceMonitor.cycle[performanceMonitor.cycleIndex].indexDataUploaded += count * sizeof(uint32);
			}
			else
			{
				cemu_assert_debug(false); // little endian index lists are not expected from GX2
			}
		}

		GLuint m_buffer = 0;
		VirtualBufferHeap_t* m_heap = nullptr;
		std::vector<uint8> m_scratch;
		std::list<IndexEntry> m_usage; // front = least recently used
		std::unordered_map<IndexKey, std::list<IndexEntry>::iterator, IndexKeyHasher> m_lookup;
	};

	GLIndexCache s_indexCache;
	ActiveIndices s_drawIndices;
	GLuint s_boundElementBuffer = 0;
}

void OpenGLRenderer::SetArrayElementBuffer(GLuint arrayElementBuffer)
{
	if (s_boundElementBuffer == arrayElementBuffer)
		return;
	s_boundElementBuffer = arrayElementBuffer;
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, arrayElementBuffer);
}

void LatteDraw_cleanupAfterFrame()
{
	if (g_renderer->GetType() != RendererAPI::OpenGL)
		return;
	s_indexCache.DropUnused(LatteGPUState.frameCounter);
}

// ---------------------------------------------------------------------------------------------------------------
// Vertex attributes
// ---------------------------------------------------------------------------------------------------------------

namespace
{
	// what was last passed to glVertexAttribIPointer for each attribute slot
	struct AttribPointerState
	{
		uint8* offset;
		uint32 stride;
		Latte::E_HWFMT format;
		uint8 nfa;
		bool isSigned;
	};
	AttribPointerState s_attribPointers[LATTE_VS_ATTRIBUTE_LIMIT]{};

	struct AttribArrayState
	{
		bool enabled = false;
		sint32 divisor = 0;
	};
	AttribArrayState s_attribArrays[GPU_GL_MAX_NUM_ATTRIBUTE];

	// vertex buffer offsets inside the buffer cache, set through buffer_bindVertexBuffers
	uint32 s_vertexBufferOffsets[Latte::GPU_LIMITS::NUM_VERTEX_BUFFERS]{};

	struct IntegerAttribLayout
	{
		GLint components;
		GLenum type;
	};

	// all attributes are fetched as raw integers, the shader does the format conversion
	bool GetIntegerAttribLayout(Latte::E_HWFMT format, IntegerAttribLayout& out)
	{
		using F = Latte::E_HWFMT;
		switch (format)
		{
		case F::HWFMT_32_32_32_32_FLOAT: case F::HWFMT_32_32_32_32: out = { 4, GL_UNSIGNED_INT }; return true;
		case F::HWFMT_32_32_32_FLOAT: case F::HWFMT_32_32_32: out = { 3, GL_UNSIGNED_INT }; return true;
		case F::HWFMT_32_32_FLOAT: case F::HWFMT_32_32: out = { 2, GL_UNSIGNED_INT }; return true;
		case F::HWFMT_32_FLOAT: case F::HWFMT_32: out = { 1, GL_UNSIGNED_INT }; return true;
		case F::HWFMT_2_10_10_10: out = { 1, GL_UNSIGNED_INT }; return true;
		case F::HWFMT_8_8_8_8: out = { 4, GL_UNSIGNED_BYTE }; return true;
		case F::HWFMT_8_8: out = { 2, GL_UNSIGNED_BYTE }; return true;
		case F::HWFMT_8: out = { 1, GL_UNSIGNED_BYTE }; return true;
		case F::HWFMT_16_16_16_16_FLOAT: case F::HWFMT_16_16_16_16: out = { 4, GL_UNSIGNED_SHORT }; return true;
		case F::HWFMT_16_16_FLOAT: case F::HWFMT_16_16: out = { 2, GL_UNSIGNED_SHORT }; return true;
		case F::HWFMT_16_FLOAT: case F::HWFMT_16: out = { 1, GL_UNSIGNED_SHORT }; return true;
		default: return false;
		}
	}

	void ApplyAttribPointer(uint32 location, const LatteParsedFetchShaderAttribute& attrib, uint8* offset, uint32 stride)
	{
		AttribPointerState& cached = s_attribPointers[location];
		const bool isSigned = attrib.isSigned != 0;
		if (cached.offset == offset && cached.stride == stride && cached.format == attrib.format && cached.nfa == attrib.nfa && cached.isSigned == isSigned)
			return;
		cached = { offset, stride, attrib.format, attrib.nfa, isSigned };

		IntegerAttribLayout layout;
		if (!GetIntegerAttribLayout(attrib.format, layout))
		{
			debug_printf("ApplyAttribPointer(): Unsupported format %d\n", (int)attrib.format);
			cemu_assert_unimplemented();
			return;
		}
		// AMD drivers need two-byte attributes to be 4 byte aligned; fetch the surrounding word instead
		if (attrib.format == Latte::E_HWFMT::HWFMT_8_8 && ((uint32)(size_t)offset & 3) == 2 && LatteGPUState.glVendor == GLVENDOR_AMD)
		{
			glVertexAttribIPointer(location, 4, GL_UNSIGNED_BYTE, stride, offset - 2);
			return;
		}
		glVertexAttribIPointer(location, layout.components, layout.type, stride, offset);
	}
}

void LatteDraw_resetAttributePointerCache()
{
	for (auto& p : s_attribPointers)
	{
		p.offset = (uint8*)-1;
		p.stride = (uint32)-1;
	}
}

void OpenGLRenderer::SetAttributeArrayState(uint32 index, bool isEnabled, sint32 aluDivisor)
{
	cemu_assert_debug(index < GPU_GL_MAX_NUM_ATTRIBUTE);
	catchOpenGLError();
	AttribArrayState& state = s_attribArrays[index];
	if (state.enabled != isEnabled)
	{
		if (isEnabled)
			glEnableVertexAttribArray(index);
		else
			glDisableVertexAttribArray(index);
		state.enabled = isEnabled;
		catchOpenGLError();
	}
	if (state.divisor != aluDivisor)
	{
		glVertexAttribDivisor(index, aluDivisor > 0 ? aluDivisor : 0);
		state.divisor = aluDivisor;
		catchOpenGLError();
	}
}

void OpenGLRenderer::buffer_bindVertexBuffers(std::span<BindBufferParam> bindings)
{
	for (const BindBufferParam& b : bindings)
		s_vertexBufferOffsets[b.index] = b.bindOffset;
}

void OpenGLRenderer::buffer_bindUniformBuffer(LatteConst::ShaderType shaderType, uint32 bufferIndex, uint32 offset, uint32 size)
{
	// binding points: vertex 0-31, pixel 32-63, geometry 64-95
	uint32 bindingPoint = bufferIndex;
	if (shaderType == LatteConst::ShaderType::Pixel)
		bindingPoint += 32;
	else if (shaderType == LatteConst::ShaderType::Geometry)
		bindingPoint += 64;
	// GL does not accept empty ranges, an unset buffer gets 32 bytes of whatever is at the start of the cache
	if (offset == 0 && size == 0)
		size = 32;
	glBindBufferRange(GL_UNIFORM_BUFFER, bindingPoint, glAttributeCacheAB, offset, size);
}

void OpenGLRenderer::_setupVertexAttributes()
{
	LatteDecompilerShader* vertexShader = LatteSHRC_GetActiveVertexShader();
	catchOpenGLError();
	attributeStream_bindVertexCacheBuffer();
	catchOpenGLError();

	LatteFetchShader* fetchShader = LatteSHRC_GetActiveFetchShader();
	bool slotUsed[32]{};
	for (auto& group : fetchShader->bufferGroups)
	{
		const uint32 regBase = mmSQ_VTX_ATTRIBUTE_BLOCK_START + group.attributeBufferIndex * 7;
		const MPTR bufferAddress = LatteGPUState.contextRegister[regBase + 0];
		const uint32 bufferStride = (LatteGPUState.contextRegister[regBase + 2] >> 11) & 0xFFFF;
		if (bufferAddress == MPTR_NULL)
		{
			// no buffer bound: the group's attributes count as unused (indexed by semantic id)
			for (sint32 i = 0; i < group.attribCount; i++)
			{
				const uint32 semantic = group.attrib[i].semanticId;
				if (semantic < std::size(slotUsed))
					slotUsed[semantic] = false;
			}
			continue;
		}
		const uint32 groupOffset = s_vertexBufferOffsets[group.attributeBufferIndex];
		for (sint32 i = 0; i < group.attribCount; i++)
		{
			const LatteParsedFetchShaderAttribute& attrib = group.attrib[i];
			const sint32 location = vertexShader->resourceMapping.getAttribHostShaderIndex(attrib.semanticId);
			if (location < 0 || location >= GPU_GL_MAX_NUM_ATTRIBUTE)
				continue;
			if (slotUsed[location])
				debug_printf("Fetch shader attribute is bound multiple times\n");
			cemu_assert_debug(attrib.attributeBufferIndex < 0x10);
			cemu_assert_debug(attrib.fetchType == LatteConst::VERTEX_DATA || attrib.fetchType == LatteConst::INSTANCE_DATA);
			// a stride of 0 means every vertex reads the same element, which a huge divisor emulates
			SetAttributeArrayState(location, true, bufferStride == 0 ? 99999999 : attrib.aluDivisor);
			ApplyAttribPointer(location, attrib, (uint8*)(size_t)(groupOffset + attrib.offset), bufferStride);
			slotUsed[location] = true;
			catchOpenGLError();
		}
	}
	for (uint32 i = 0; i < GPU_GL_MAX_NUM_ATTRIBUTE; i++)
	{
		if (!slotUsed[i] && s_attribArrays[i].enabled)
			SetAttributeArrayState(i, false, -1);
	}
}

// ---------------------------------------------------------------------------------------------------------------
// RECT primitives
// ---------------------------------------------------------------------------------------------------------------

namespace
{
	std::unordered_map<uint64, RendererShaderGL*> s_rectGeometryShaders;

	void AppendRectInterface(std::string& src, LatteDecompilerShader* vertexShader, LatteShaderPSInputTable* psInputTable, bool outputs)
	{
		const uint32 mask = vertexShader->outputParameterMask;
		for (uint32 i = 0; i < 32; i++)
		{
			if ((mask & (1u << i)) == 0)
				continue;
			const sint32 semantic = psInputTable->getVertexShaderOutParamSemanticId(LatteGPUState.contextRegister, i);
			if (semantic < 0)
				continue;
			const auto* psImport = psInputTable->getPSImportBySemanticId(semantic);
			if (!psImport)
				continue;
			src += fmt::format("layout(location = {}) ", psInputTable->getPSImportLocationBySemanticId(semantic));
			if (psImport->isFlat)
				src += "flat ";
			if (psImport->isNoPerspective)
				src += "noperspective ";
			if (outputs)
				src += fmt::format("out vec4 passParameterSem{}Out;\r\n", semantic);
			else
				src += fmt::format("in vec4 passParameterSem{}In[];\r\n", semantic);
		}
	}

	RendererShaderGL* BuildRectGeometryShader(LatteDecompilerShader* vertexShader, LatteShaderPSInputTable* psInputTable)
	{
		std::string src =
			"#version 450\r\n"
			"layout(triangles) in;\r\n"
			"layout(triangle_strip) out;\r\n"
			"layout(max_vertices = 4) out;\r\n"
			"in gl_PerVertex {\r\n"
			"vec4 gl_Position;\r\n"
			"} gl_in[];\r\n"
			"out gl_PerVertex {\r\n"
			"vec4 gl_Position;\r\n"
			"};\r\n";
		AppendRectInterface(src, vertexShader, psInputTable, false);
		AppendRectInterface(src, vertexShader, psInputTable, true);
		// the fourth corner, for the three ways the input triangle can sit in the rectangle
		src +=
			"vec4 gen4thVertexA(vec4 a, vec4 b, vec4 c)\r\n{\r\nreturn b - (c - a);\r\n}\r\n"
			"vec4 gen4thVertexB(vec4 a, vec4 b, vec4 c)\r\n{\r\nreturn c - (b - a);\r\n}\r\n"
			"vec4 gen4thVertexC(vec4 a, vec4 b, vec4 c)\r\n{\r\nreturn c + (b - a);\r\n}\r\n"
			"void main()\r\n{\r\n"
			// the longest edge of the triangle is the rectangle's diagonal
			"float dist0_1 = length(gl_in[1].gl_Position.xy - gl_in[0].gl_Position.xy);\r\n"
			"float dist0_2 = length(gl_in[2].gl_Position.xy - gl_in[0].gl_Position.xy);\r\n"
			"float dist1_2 = length(gl_in[2].gl_Position.xy - gl_in[1].gl_Position.xy);\r\n"
			"if(dist0_1 > dist0_2 && dist0_1 > dist1_2)\r\n{\r\n";
		rectsEmulationGS_outputVerticesCode(src, vertexShader, psInputTable, 2, 1, 0, 3, "A", LatteGPUState.contextNew);
		src += "} else if ( dist0_2 > dist0_1 && dist0_2 > dist1_2 ) {\r\n";
		rectsEmulationGS_outputVerticesCode(src, vertexShader, psInputTable, 1, 2, 0, 3, "B", LatteGPUState.contextNew);
		src += "} else {\r\n";
		rectsEmulationGS_outputVerticesCode(src, vertexShader, psInputTable, 0, 1, 2, 3, "C", LatteGPUState.contextNew);
		src += "}\r\n}\r\n";

		auto* shader = new RendererShaderGL(RendererShader::ShaderType::kGeometry, 0, 0, false, false, src);
		shader->PreponeCompilation(true);
		return shader;
	}

	RendererShaderGL* GetRectGeometryShader(LatteDecompilerShader* vertexShader)
	{
		LatteShaderPSInputTable* psInputTable = LatteSHRC_GetPSInputTable();
		const uint64 key = vertexShader->baseHash + psInputTable->key;
		RendererShaderGL*& slot = s_rectGeometryShaders[key];
		if (!slot)
			slot = BuildRectGeometryShader(vertexShader, psInputTable);
		return slot;
	}

	// false for primitive types the GL backend cannot draw (GL_POINTS is 0, so the mode itself cannot signal that)
	bool ToGLPrimitive(PrimType mode, GLenum& glMode)
	{
		switch (mode)
		{
		case PrimType::POINTS: glMode = GL_POINTS; return true;
		case PrimType::LINES: glMode = GL_LINES; return true;
		case PrimType::LINE_STRIP: glMode = GL_LINE_STRIP; return true;
		case PrimType::LINE_LOOP: glMode = GL_LINE_LOOP; return true;
		case PrimType::TRIANGLES: glMode = GL_TRIANGLES; return true;
		case PrimType::TRIANGLE_STRIP: glMode = GL_TRIANGLE_STRIP; return true;
		case PrimType::TRIANGLE_FAN: glMode = GL_TRIANGLE_FAN; return true;
		case PrimType::QUADS: glMode = GL_QUADS; return true;
		case PrimType::QUAD_STRIP: glMode = GL_QUAD_STRIP; return true;
		case PrimType::RECTS: glMode = GL_TRIANGLES; return true; // expanded by the geometry shader
		default: return false;
		}
	}

	void IssueDraw(IndexType indexType, uint32 baseVertex, uint32 baseInstance, uint32 instanceCount, uint32 count)
	{
		const GLenum mode = sGLActiveDrawMode;
		const bool instanced = instanceCount > 1 || baseInstance != 0;
		switch (indexType)
		{
		case IndexType::U16_BE:
			if (instanced)
				glDrawElementsInstancedBaseVertexBaseInstance(mode, count, GL_UNSIGNED_SHORT, s_drawIndices.bufferOffset, instanceCount, baseVertex, baseInstance);
			else if (baseVertex != 0)
				glDrawRangeElementsBaseVertex(mode, s_drawIndices.lowest, s_drawIndices.highest, count, GL_UNSIGNED_SHORT, s_drawIndices.bufferOffset, baseVertex);
			else
				glDrawRangeElements(mode, s_drawIndices.lowest, s_drawIndices.highest, count, GL_UNSIGNED_SHORT, s_drawIndices.bufferOffset);
			break;
		case IndexType::U32_BE:
			if (instanced)
				glDrawElementsInstancedBaseVertexBaseInstance(mode, count, GL_UNSIGNED_INT, s_drawIndices.bufferOffset, instanceCount, baseVertex, baseInstance);
			else
				glDrawRangeElementsBaseVertex(mode, s_drawIndices.lowest, s_drawIndices.highest, count, GL_UNSIGNED_INT, s_drawIndices.bufferOffset, baseVertex);
			break;
		case IndexType::AUTO:
			cemu_assert_debug(baseInstance == 0);
			if (instanceCount > 1)
				glDrawArraysInstanced(mode, baseVertex, count, instanceCount);
			else
				glDrawArrays(mode, baseVertex, count);
			break;
		default:
			cemu_assert_debug(false);
			break;
		}
	}

	// ---- per draw uniforms that are not part of the uniform buffers ----

	float CurrentPointSizeUniform()
	{
		float size = (float)LatteGPUState.contextNew.PA_SU_POINT_SIZE.get_WIDTH() / 8.0f;
		return size == 0.0f ? 1.0f / 8.0f : size;
	}

	void UpdateTexRescaleUniforms(LatteDecompilerShader* shader, LatteConst::ShaderType stage, GLuint program)
	{
		for (auto& entry : shader->uniform.list_ufTexRescale)
		{
			float* scale = LatteTexture_getEffectiveTextureScale(stage, entry.texUnit);
			if (memcmp(entry.currentValue, scale, sizeof(float) * 2) == 0)
				continue;
			memcpy(entry.currentValue, scale, sizeof(float) * 2);
			glProgramUniform2fv(program, entry.uniformLocation, 1, scale);
		}
	}

	void UpdateVertexStageUniforms(LatteDecompilerShader* vs)
	{
		const GLuint program = static_cast<RendererShaderGL*>(vs->shader)->GetProgram();
		if (vs->uniform.loc_windowSpaceToClipSpaceTransform >= 0)
		{
			sint32 vpWidth, vpHeight;
			LatteRenderTarget_GetCurrentVirtualViewportSize(&vpWidth, &vpHeight); // valid after LatteRenderTarget_updateViewport()
			const float transform[2] = { 2.0f / (float)vpWidth, 2.0f / (float)vpHeight };
			glProgramUniform2fv(program, vs->uniform.loc_windowSpaceToClipSpaceTransform, 1, transform);
		}
		UpdateTexRescaleUniforms(vs, LatteConst::ShaderType::Vertex, program);
		if (vs->uniform.loc_pointSize >= 0)
		{
			const float pointSize = CurrentPointSizeUniform();
			glProgramUniform1fv(program, vs->uniform.loc_pointSize, 1, &pointSize);
		}
	}

	void UpdateGeometryStageUniforms(LatteDecompilerShader* gs)
	{
		const GLuint program = static_cast<RendererShaderGL*>(gs->shader)->GetProgram();
		UpdateTexRescaleUniforms(gs, LatteConst::ShaderType::Geometry, program);
		if (gs->uniform.loc_pointSize >= 0)
		{
			const float pointSize = CurrentPointSizeUniform();
			glProgramUniform1fv(program, gs->uniform.loc_pointSize, 1, &pointSize);
		}
	}

	void UpdatePixelStageUniforms(LatteDecompilerShader* ps)
	{
		const GLuint program = static_cast<RendererShaderGL*>(ps->shader)->GetProgram();
		if (ps->uniform.loc_alphaTestRef >= 0)
		{
			const float ref = LatteGPUState.contextNew.SX_ALPHA_REF.get_ALPHA_TEST_REF();
			if (ps->uniform.ufCurrentValueAlphaTestRef != ref)
			{
				glProgramUniform1fv(program, ps->uniform.loc_alphaTestRef, 1, &ref);
				ps->uniform.ufCurrentValueAlphaTestRef = ref;
			}
		}
		if (ps->uniform.loc_fragCoordScale >= 0)
		{
			float scale[4];
			LatteMRT::GetCurrentFragCoordScale(scale);
			float* current = ps->uniform.ufCurrentValueFragCoordScale;
			if (current[0] != scale[0] || current[1] != scale[1])
			{
				glProgramUniform2fv(program, ps->uniform.loc_fragCoordScale, 1, scale);
				current[0] = scale[0];
				current[1] = scale[1];
			}
		}
		UpdateTexRescaleUniforms(ps, LatteConst::ShaderType::Pixel, program);
	}

	uint32 s_lastReadbackPollDrawcall = 0;
}

// ---------------------------------------------------------------------------------------------------------------
// Draw calls
// ---------------------------------------------------------------------------------------------------------------

template<bool TIsMinimal, bool THasProfiling>
void OpenGLRenderer::draw_genericDrawHandler(uint32 baseVertex, uint32 baseInstance, uint32 instanceCount, uint32 count, MPTR indexDataMPTR, Latte::LATTE_VGT_DMA_INDEX_TYPE::E_INDEX_TYPE indexType)
{
	ReleaseBufferCacheEntries();
	catchOpenGLError();
	auto& ctx = LatteGPUState.contextNew;
	const PrimType primitiveMode = ctx.VGT_PRIMITIVE_TYPE.get_PRIMITIVE_MODE();

	if (ctx.GetSpecialStateValues()[8] != 0)
	{
		LatteDraw_handleSpecialState8_clearAsDepth();
		LatteGPUState.drawCallCounter++;
		return;
	}

	if constexpr (!TIsMinimal)
	{
		beginPerfMonProfiling(performanceMonitor.gpuTime_dcStageShaderAndUniformMgr);
		LatteSHRC_UpdateActiveShaders();
		LatteDecompilerShader* vs = LatteSHRC_GetActiveVertexShader();
		LatteDecompilerShader* gs = LatteSHRC_GetActiveGeometryShader();
		LatteDecompilerShader* ps = LatteSHRC_GetActivePixelShader();
		vs ? shader_bind(vs->shader) : shader_unbind(RendererShader::ShaderType::kVertex);
		// with streamout active nothing is rasterized by this pass
		(ps && LatteGPUState.contextRegister[mmVGT_STRMOUT_EN] == 0) ? shader_bind(ps->shader) : shader_unbind(RendererShader::ShaderType::kFragment);
		gs ? shader_bind(gs->shader) : shader_unbind(RendererShader::ShaderType::kGeometry);
		endPerfMonProfiling(performanceMonitor.gpuTime_dcStageShaderAndUniformMgr);
	}
	if (LatteGPUState.activeShaderHasError)
	{
		cemuLog_logDebugOnce(LogType::Force, "Skipped drawcall due to shader error\n");
		return;
	}

	// rasterization kill only applies when the viewport transform is enabled
	const bool rasterizerEnable = !ctx.PA_CL_CLIP_CNTL.get_DX_RASTERIZATION_KILL() || !ctx.PA_CL_VTE_CNTL.get_VPORT_X_OFFSET_ENA();
	const bool streamoutEnable = LatteGPUState.contextRegister[mmVGT_STRMOUT_EN] != 0;
	if (streamoutEnable && glBeginTransformFeedback == nullptr)
	{
		cemu_assert_debug(false);
		return;
	}
	if (!rasterizerEnable && !streamoutEnable)
	{
		LatteGPUState.drawCallCounter++; // the draw has no visible effect
		return;
	}
	if (!ToGLPrimitive(primitiveMode, sGLActiveDrawMode))
	{
		cemu_assert_debug(false); // unsupported primitive type
		LatteGPUState.drawCallCounter++;
		return;
	}

	if constexpr (!TIsMinimal)
	{
		LatteGPUState.requiresTextureBarrier = false;
		beginPerfMonProfiling(performanceMonitor.gpuTime_dcStageTextures);
		do
		{
			LatteGPUState.repeatTextureInitialization = false;
			if (!streamoutEnable)
			{
				// render targets only matter when something is rasterized
				if (!LatteMRT::UpdateCurrentFBO())
					return;
				if (!hasValidFramebufferAttached)
					return;
			}
			LatteTexture_updateTextures(); // texture bindings must not change after this
			if (LatteGPUState.repeatTextureInitialization)
				catchOpenGLError();
		} while (LatteGPUState.repeatTextureInitialization);
		endPerfMonProfiling(performanceMonitor.gpuTime_dcStageTextures);

		beginPerfMonProfiling(performanceMonitor.gpuTime_dcStageMRT);
		LatteMRT::ApplyCurrentState();
		endPerfMonProfiling(performanceMonitor.gpuTime_dcStageMRT);

		// the draw samples a texture it also renders to
		if (LatteGPUState.requiresTextureBarrier && glTextureBarrier)
			glTextureBarrier();
	}
	catchOpenGLError();

	const bool isRects = primitiveMode == PrimType::RECTS;
	if (isRects)
		shader_bind(GetRectGeometryShader(LatteSHRC_GetActiveVertexShader()));

	beginPerfMonProfiling(performanceMonitor.gpuTime_dcStageIndexMgr);
	s_indexCache.Prepare(indexDataMPTR, indexType, (sint32)count, (uint32)primitiveMode, s_drawIndices);
	endPerfMonProfiling(performanceMonitor.gpuTime_dcStageIndexMgr);

	uint8 stageUniformModifiedMask = 0;
	LatteBufferCache_Sync(s_drawIndices.highest + baseVertex, baseInstance, instanceCount, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, stageUniformModifiedMask);
	_setupVertexAttributes();

	LatteRenderTarget_updateViewport();
	LatteRenderTarget_updateScissorBox();
	renderstate_updateBlendingAndColorControl();
	catchOpenGLError();

	// special state 5: copy the depth buffer into colour buffer 0 instead of drawing
	if (ctx.GetSpecialStateValues()[5] != 0)
	{
		debug_printf("GPU7 special state 5 used\n");
		LatteTextureView* color = LatteMRT::GetColorAttachment(0);
		LatteTextureView* depth = LatteMRT::GetDepthAttachment();
		if (!color || !depth)
		{
			cemuLog_log(LogType::Force, "GPU7 special state 5 used but render target not setup correctly");
			return;
		}
		surfaceCopy_copySurfaceWithFormatConversion(depth->baseTexture, depth->firstMip, depth->firstSlice, color->baseTexture, color->firstMip, color->firstSlice, depth->baseTexture->width, depth->baseTexture->height);
		LatteGPUState.drawCallCounter++;
		return;
	}

	beginPerfMonProfiling(performanceMonitor.gpuTime_dcStageShaderAndUniformMgr);
	uniformData_update();
	endPerfMonProfiling(performanceMonitor.gpuTime_dcStageShaderAndUniformMgr);
	catchOpenGLError();

	LatteDecompilerShader* vertexShader = LatteSHRC_GetActiveVertexShader();
	LatteDecompilerShader* pixelShader = LatteSHRC_GetActivePixelShader();
	LatteDecompilerShader* geometryShader = LatteSHRC_GetActiveGeometryShader();
	if (vertexShader)
		UpdateVertexStageUniforms(vertexShader);
	if (geometryShader)
		UpdateGeometryStageUniforms(geometryShader);
	if (pixelShader)
		UpdatePixelStageUniforms(pixelShader);
	catchOpenGLError();

	LatteStreamout_PrepareDrawcall(count, instanceCount);
	if (streamoutEnable && rasterizerEnable && primitiveMode == PrimType::QUADS)
		sGLActiveDrawMode = GL_POINTS;
	catchOpenGLError();

	beginPerfMonProfiling(performanceMonitor.gpuTime_dcStageDrawcallAPI);
	IssueDraw(indexType, baseVertex, baseInstance, instanceCount, count);
	endPerfMonProfiling(performanceMonitor.gpuTime_dcStageDrawcallAPI);

	if (pixelShader)
		LatteRenderTarget_trackUpdates();
	LatteStreamout_FinishDrawcall(false);
	catchOpenGLError();
	if (isRects)
		shader_unbind(RendererShader::ShaderType::kGeometry);
	LatteGPUState.drawCallCounter++;

	if (streamoutEnable && rasterizerEnable)
	{
		// the streamout pass is done, draw again for the rasterizer
		const uint32 savedStreamoutEnable = LatteGPUState.contextRegister[mmVGT_STRMOUT_EN];
		LatteGPUState.contextRegister[mmVGT_STRMOUT_EN] = 0;
		draw_genericDrawHandler<false, THasProfiling>(baseVertex, baseInstance, instanceCount, count, indexDataMPTR, indexType);
		LatteGPUState.contextRegister[mmVGT_STRMOUT_EN] = savedStreamoutEnable;
		return;
	}

	LatteTextureReadback_Update();
	if (LatteGPUState.drawCallCounter - s_lastReadbackPollDrawcall >= 150)
	{
		LatteTextureReadback_UpdateFinishedTransfers(false);
		s_lastReadbackPollDrawcall = LatteGPUState.drawCallCounter;
	}
	catchOpenGLError();
}

template void OpenGLRenderer::draw_genericDrawHandler<false, false>(uint32, uint32, uint32, uint32, MPTR, Latte::LATTE_VGT_DMA_INDEX_TYPE::E_INDEX_TYPE);
template void OpenGLRenderer::draw_genericDrawHandler<true, false>(uint32, uint32, uint32, uint32, MPTR, Latte::LATTE_VGT_DMA_INDEX_TYPE::E_INDEX_TYPE);

void OpenGLRenderer::draw_beginSequence()
{
}

void OpenGLRenderer::draw_execute(uint32 baseVertex, uint32 baseInstance, uint32 instanceCount, uint32 count, MPTR indexDataMPTR, Latte::LATTE_VGT_DMA_INDEX_TYPE::E_INDEX_TYPE indexType, const LatteDrawcallContext& drawcallContext)
{
	// follow-up draws of a sequence keep the shaders, textures and render targets of the first one
	if (drawcallContext.isFirst)
		draw_genericDrawHandler<false, false>(baseVertex, baseInstance, instanceCount, count, indexDataMPTR, indexType);
	else
		draw_genericDrawHandler<true, false>(baseVertex, baseInstance, instanceCount, count, indexDataMPTR, indexType);
}

void OpenGLRenderer::draw_endSequence()
{
}

void OpenGLRenderer::draw_init()
{
	if (!s_indexCache.IsReady())
		s_indexCache.Create();
}

void OpenGLRenderer::bufferCache_upload(uint8* buffer, sint32 size, uint32 bufferOffset)
{
	attributeStream_bindVertexCacheBuffer();
	glBufferSubData(GL_ARRAY_BUFFER, bufferOffset, size, buffer);
}

void OpenGLRenderer::bufferCache_copy(uint32 srcOffset, uint32 dstOffset, uint32 size)
{
	attributeStream_bindVertexCacheBuffer();
	glCopyBufferSubData(GL_ARRAY_BUFFER, GL_ARRAY_BUFFER, srcOffset, dstOffset, size);
}

// ---------------------------------------------------------------------------------------------------------------
// Texture parameters (GL keeps sampler state in the texture object, one view per sampler configuration)
// ---------------------------------------------------------------------------------------------------------------

namespace
{
	// SQ_TEX_CLAMP -> GL wrap mode
	constexpr GLint kWrapModes[8] =
	{
		GL_REPEAT,                      // WRAP
		GL_MIRRORED_REPEAT,             // MIRROR
		GL_CLAMP_TO_EDGE,               // CLAMP_LAST_TEXEL
		GL_MIRROR_CLAMP_TO_EDGE,        // MIRROR_ONCE_LAST_TEXEL
		GL_CLAMP_TO_EDGE,               // CLAMP_HALF_BORDER
		GL_MIRROR_CLAMP_TO_BORDER_EXT,  // MIRROR_ONCE_HALF_BORDER
		GL_CLAMP_TO_BORDER,             // CLAMP_BORDER
		GL_MIRROR_CLAMP_TO_BORDER_EXT,  // MIRROR_ONCE_BORDER
	};

	// SQ_SEL -> GL swizzle source
	constexpr GLint kSwizzleSources[8] = { GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA, GL_ZERO, GL_ONE, 0, 0 };

	constexpr GLint kCompareFuncs[8] = { GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS };

	// Some host formats return channels in a different place than GPU7 does; adjust the swizzle source for that
	uint32 AdjustSwizzleForHostFormat(Latte::E_GX2SURFFMT format, uint32 sel)
	{
		using F = Latte::E_GX2SURFFMT;
		const bool isColorChannel = sel <= 3;
		switch (format)
		{
		case F::R8_UNORM:
		case F::R8_SNORM:
		case F::BC4_UNORM:
		case F::BC4_SNORM:
			// one channel formats replicate red, GL would give 0/1 for the others
			return (sel >= 1 && sel <= 3) ? 0 : sel;
		case F::A1_B5_G5_R5_UNORM:
		case F::A2_B10_G10_R10_UNORM:
			// stored as ABGR on GPU7, the host format is RGBA
			return isColorChannel ? 3 - sel : sel;
		case F::BC5_UNORM:
		case F::BC5_SNORM:
			return sel == 3 ? 1 : sel; // alpha reads green
		case F::X24_G8_UINT:
			return isColorChannel ? 3 : sel;
		default:
			return sel;
		}
	}

	GLint ToGLMinFilter(Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_XY_FILTER xy, Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_Z_FILTER mip)
	{
		using XY = Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_XY_FILTER;
		using Z = Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_Z_FILTER;
		const bool nearest = xy == XY::POINT || xy == XY::ANISO_POINT;
		if (mip == Z::NONE)
			return nearest ? GL_NEAREST : GL_LINEAR;
		if (mip == Z::POINT)
			return nearest ? GL_NEAREST_MIPMAP_NEAREST : GL_LINEAR_MIPMAP_NEAREST;
		return nearest ? GL_NEAREST_MIPMAP_LINEAR : GL_LINEAR_MIPMAP_LINEAR;
	}
}

void OpenGLRenderer::renderstate_updateTextureSettingsGL(LatteDecompilerShader* shaderContext, LatteTextureView* _hostTextureView, uint32 hostTextureUnit, const Latte::LATTE_SQ_TEX_RESOURCE_WORD4_N texUnitWord4, uint32 texUnitIndex, bool isDepthSampler)
{
	auto* view = static_cast<LatteTextureViewGL*>(_hostTextureView);
	LatteTexture* baseTexture = view->baseTexture;
	const GLenum target = view->glTexTarget;
	catchOpenGLError();

	// the texture is only bound once something actually has to change
	bool isBound = false;
	auto bindOnce = [&]()
	{
		if (isBound)
			return;
		texture_bindAndActivate(view, hostTextureUnit);
		isBound = true;
	};
	auto setInt = [&](GLenum pname, GLint value)
	{
		bindOnce();
		glTexParameteri(target, pname, value);
	};
	auto setFloat = [&](GLenum pname, float value)
	{
		bindOnce();
		glTexParameterf(target, pname, value);
	};

	// swizzle
	const uint32 sel[4] =
	{
		AdjustSwizzleForHostFormat(view->format, (uint32)texUnitWord4.get_DST_SEL_X()),
		AdjustSwizzleForHostFormat(view->format, (uint32)texUnitWord4.get_DST_SEL_Y()),
		AdjustSwizzleForHostFormat(view->format, (uint32)texUnitWord4.get_DST_SEL_Z()),
		AdjustSwizzleForHostFormat(view->format, (uint32)texUnitWord4.get_DST_SEL_W()),
	};
	uint8* swizzleCache[4] = { &view->swizzleR, &view->swizzleG, &view->swizzleB, &view->swizzleA };
	constexpr GLenum kSwizzleParams[4] = { GL_TEXTURE_SWIZZLE_R, GL_TEXTURE_SWIZZLE_G, GL_TEXTURE_SWIZZLE_B, GL_TEXTURE_SWIZZLE_A };
	for (int c = 0; c < 4; c++)
	{
		if (*swizzleCache[c] == sel[c])
			continue;
		setInt(kSwizzleParams[c], kSwizzleSources[sel[c]]);
		*swizzleCache[c] = (uint8)sel[c];
	}
	catchOpenGLError();

	const uint32 stageSamplerIndex = shaderContext->textureUnitSamplerAssignment[texUnitIndex];
	if (stageSamplerIndex == LATTE_DECOMPILER_SAMPLER_NONE)
		return;
	const _LatteRegisterSetSampler& sampler = LatteGPUState.contextNew.SQ_TEX_SAMPLER[stageSamplerIndex + LatteDecompiler_getTextureSamplerBaseIndex(shaderContext->shaderType)];
	LatteSamplerState& cache = view->samplerState;

	const uint32 minFilter = (uint32)ToGLMinFilter(sampler.WORD0.get_XY_MIN_FILTER(), sampler.WORD0.get_MIP_FILTER());
	const auto magFilterReg = sampler.WORD0.get_XY_MAG_FILTER();
	const uint32 magFilter = (magFilterReg == Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_XY_FILTER::POINT || magFilterReg == Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_XY_FILTER::ANISO_POINT) ? GL_NEAREST : GL_LINEAR;
	catchOpenGLError();

	// wrap modes
	const uint32 wrap[3] = { (uint32)sampler.WORD0.get_CLAMP_X(), (uint32)sampler.WORD0.get_CLAMP_Y(), (uint32)sampler.WORD0.get_CLAMP_Z() };
	uint8* wrapCache[3] = { &cache.clampS, &cache.clampT, &cache.clampR };
	constexpr GLenum kWrapParams[3] = { GL_TEXTURE_WRAP_S, GL_TEXTURE_WRAP_T, GL_TEXTURE_WRAP_R };
	for (int c = 0; c < 3; c++)
	{
		if (*wrapCache[c] == wrap[c])
			continue;
		setInt(kWrapParams[c], kWrapModes[wrap[c]]);
		*wrapCache[c] = (uint8)wrap[c];
	}
	catchOpenGLError();

	// anisotropy (graphic packs can override it), filters, mip range
	uint32 anisoLog2 = (uint32)sampler.WORD0.get_MAX_ANISO_RATIO();
	if (baseTexture->overwriteInfo.anisotropicLevel >= 0)
		anisoLog2 = baseTexture->overwriteInfo.anisotropicLevel;
	if (cache.maxAniso != anisoLog2)
	{
		setFloat(GL_TEXTURE_MAX_ANISOTROPY_EXT, (float)(1 << anisoLog2));
		cache.maxAniso = (uint8)anisoLog2;
		catchOpenGLError();
	}
	if (cache.filterMin != minFilter)
	{
		setInt(GL_TEXTURE_MIN_FILTER, minFilter);
		cache.filterMin = minFilter;
		catchOpenGLError();
	}
	if (cache.filterMag != magFilter)
	{
		setInt(GL_TEXTURE_MAG_FILTER, magFilter);
		cache.filterMag = magFilter;
		catchOpenGLError();
	}
	if (cache.maxMipLevels != view->numMip)
	{
		setInt(GL_TEXTURE_MAX_LEVEL, std::max(view->numMip, 1) - 1);
		cache.maxMipLevels = (uint8)view->numMip;
		catchOpenGLError();
	}

	// LOD in 1/64 steps; graphic packs can add to the bias or replace it
	const uint32 minLod = sampler.WORD1.get_MIN_LOD();
	const uint32 maxLod = sampler.WORD1.get_MAX_LOD();
	sint32 lodBias = sampler.WORD1.get_LOD_BIAS();
	if (baseTexture->overwriteInfo.hasRelativeLodBias)
		lodBias += baseTexture->overwriteInfo.relativeLodBias;
	if (baseTexture->overwriteInfo.hasLodBias)
		lodBias = baseTexture->overwriteInfo.lodBias;
	if (cache.minLod != minLod)
	{
		setFloat(GL_TEXTURE_MIN_LOD, (float)minLod / 64.0f);
		cache.minLod = (uint16)minLod;
	}
	if (cache.maxLod != maxLod)
	{
		setFloat(GL_TEXTURE_MAX_LOD, (float)maxLod / 64.0f);
		cache.maxLod = (uint16)maxLod;
	}
	if (cache.lodBias != lodBias)
	{
		setFloat(GL_TEXTURE_LOD_BIAS, (float)lodBias / 64.0f);
		cache.lodBias = (sint16)lodBias;
	}

	// shadow sampling
	const uint32 compareFunc = (uint32)sampler.WORD0.get_DEPTH_COMPARE_FUNCTION();
	if (cache.depthCompareFunc != compareFunc)
	{
		setInt(GL_TEXTURE_COMPARE_FUNC, kCompareFuncs[compareFunc]);
		cache.depthCompareFunc = (uint8)compareFunc;
	}
	const uint8 compareMode = isDepthSampler ? 1 : 0;
	if (cache.depthCompareMode != compareMode)
	{
		setInt(GL_TEXTURE_COMPARE_MODE, compareMode ? GL_COMPARE_REF_TO_TEXTURE : GL_NONE);
		cache.depthCompareMode = compareMode;
	}
	catchOpenGLError();

	// border colour; register colours can change without the type changing, so they are always compared
	using BorderType = Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_BORDER_COLOR_TYPE;
	const BorderType borderType = sampler.WORD0.get_BORDER_COLOR_TYPE();
	if (cache.borderType != (uint8)borderType || borderType == BorderType::REGISTER)
	{
		GLfloat color[4];
		switch (borderType)
		{
		case BorderType::TRANSPARENT_BLACK:
			color[0] = color[1] = color[2] = color[3] = 0.0f;
			break;
		case BorderType::OPAQUE_BLACK:
			color[0] = color[1] = color[2] = 0.0f;
			color[3] = 1.0f;
			break;
		case BorderType::OPAQUE_WHITE:
			color[0] = color[1] = color[2] = color[3] = 1.0f;
			break;
		default:
		{
			auto& c = LatteGPUState.contextNew;
			const _LatteRegisterSetSamplerBorderColor* reg;
			switch (shaderContext->shaderType)
			{
			case LatteConst::ShaderType::Vertex:
			case LatteConst::ShaderType::Compute:
				reg = c.TD_VS_SAMPLER_BORDER_COLOR + stageSamplerIndex;
				break;
			case LatteConst::ShaderType::Pixel:
				reg = c.TD_PS_SAMPLER_BORDER_COLOR + stageSamplerIndex;
				break;
			default:
				reg = c.TD_GS_SAMPLER_BORDER_COLOR + stageSamplerIndex;
				break;
			}
			color[0] = reg->red.get_channelValue();
			color[1] = reg->green.get_channelValue();
			color[2] = reg->blue.get_channelValue();
			color[3] = reg->alpha.get_channelValue();
			break;
		}
		}
		// compared as floats on purpose: -0.0 matches 0.0
		bool colorChanged = false;
		for (int c = 0; c < 4; c++)
			colorChanged |= cache.borderColor[c] != color[c];
		if (colorChanged)
		{
			bindOnce();
			glTexParameterfv(target, GL_TEXTURE_BORDER_COLOR, color);
			for (int c = 0; c < 4; c++)
				cache.borderColor[c] = color[c];
		}
		cache.borderType = (uint8)borderType;
	}
	catchOpenGLError();
}
