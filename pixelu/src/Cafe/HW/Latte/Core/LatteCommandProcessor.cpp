// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "Cafe/OS/libs/gx2/GX2.h"
#include "Cafe/OS/libs/gx2/GX2_Event.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LatteShader.h"
#include "Cafe/HW/Latte/Core/LatteAsyncCommands.h"
#include "Cafe/HW/Latte/Core/LattePerformanceMonitor.h"
#include "Cafe/HW/Latte/Core/LatteIndices.h"
#include "Cafe/HW/Latte/Core/LatteBufferCache.h"
#include "Cafe/HW/Latte/Core/LattePM4.h"
#include "Cafe/HW/Latte/Core/LatteSurfaceCopy.h"
#include "Cafe/OS/libs/coreinit/coreinit_Time.h"
#include "Cafe/OS/libs/TCL/TCL.h"
#include "Cafe/CafeSystem.h"
#include <boost/container/small_vector.hpp>

// Latte command processor.
//
// Commands reach the GPU thread through the TCL ring buffer, one dword at a time.
// Most of what games submit is in indirect buffers (display lists and GX2's own
// command buffers) that the ring buffer points at; those are walked in place.
//
// Draws inside indirect buffers get a fast path: after a draw the walker keeps
// going in "draw pass" mode, where only packets that leave the pipeline mostly
// intact (vertex/uniform buffer changes, ALU constants, unchanged state, more
// draws) are accepted. The renderer is told which buffers changed between the
// draws of a pass so it can skip the full state setup. Any other packet ends
// the pass and is handled by the general walker.

void LatteThread_HandleOSScreen();
void LatteThread_Exit();

namespace
{
	using CmdPtr = uint32be*;

	// PM4 header fields
	struct PacketHeader
	{
		uint32 raw;
		uint32 type() const { return raw >> 30; }
		uint32 opcode() const { return (raw >> 8) & 0xFF; } // type 3
		uint32 count() const { return ((raw >> 16) & 0x3FFF) + 1; } // type 0 and 3: dwords following the header
		uint32 regBase() const { return raw & 0xFFFF; } // type 0
	};

	// reads consecutive dwords of a packet body
	struct BodyReader
	{
		CmdPtr p;
		uint32 next() { return (uint32)*p++; }
		void skip(uint32 n) { p += n; }
	};

	// position inside one command buffer
	struct BufferCursor
	{
		CmdPtr pos;
		CmdPtr begin;
		CmdPtr end;
	};

	bool s_predicationActive = false;
	MPTR s_immediateIndexBuffer = MPTR_NULL; // guest memory for DRAW_INDEX_IMMD indices
	constexpr uint32 kImmediateIndexBufferWords = 0x4000;

	// state of one walk through indirect buffers: the cursor stack and the current draw pass
	class Walker
	{
	public:
		void Push(CmdPtr pos, CmdPtr begin, CmdPtr end)
		{
			m_stack.push_back({ pos, begin, end });
		}

		bool Pop(BufferCursor& c)
		{
			if (m_stack.empty())
				return false;
			c = m_stack.back();
			m_stack.pop_back();
			return true;
		}

		bool InPass() const { return m_inPass; }

		void BeginPass()
		{
			m_inPass = true;
			ResetDirty();
			m_ctx.isFirst = true;
			g_renderer->draw_beginSequence();
		}

		void EndPass()
		{
			g_renderer->draw_endSequence();
			m_inPass = false;
		}

		// issues one draw with the current register state
		void Draw(uint32 count, bool autoIndex, MPTR indices)
		{
			const uint32 baseVertex = LatteGPUState.contextRegister[mmSQ_VTX_BASE_VTX_LOC];
			const uint32 baseInstance = LatteGPUState.contextRegister[mmSQ_VTX_START_INST_LOC];
			const uint32 instances = LatteGPUState.contextNew.VGT_DMA_NUM_INSTANCES.get_NUM_INSTANCES();
			if (autoIndex)
				g_renderer->draw_execute(baseVertex, baseInstance, instances, count, MPTR_NULL, Latte::LATTE_VGT_DMA_INDEX_TYPE::E_INDEX_TYPE::AUTO, m_ctx);
			else
			{
				cemu_assert_debug(indices != MPTR_NULL);
				if (indices == MPTR_NULL)
					return;
				g_renderer->draw_execute(baseVertex, baseInstance, instances, count, indices, LatteGPUState.contextNew.VGT_DMA_INDEX_TYPE.get_INDEX_TYPE(), m_ctx);
			}
			auto& stats = performanceMonitor.cycle[performanceMonitor.cycleIndex];
			stats.drawCallCounter++;
			if (!m_ctx.isFirst)
				stats.fastDrawCallCounter++;
			m_ctx.isFirst = false;
			ResetDirty();
		}

		void VertexBufferChanged(uint32 index) { m_ctx.vertexBufferDirtyMask |= (1u << index); }
		void VSUniformBufferChanged(uint32 index) { m_ctx.vsUniformBufferDirtyMask |= (1u << index); }
		void PSUniformBufferChanged(uint32 index) { m_ctx.psUniformBufferDirtyMask |= (1u << index); }
		void GSUniformBufferChanged(uint32 index) { m_ctx.gsUniformBufferDirtyMask |= (1u << index); }
		void VSConstantsChanged() { m_ctx.aluConstVSDirty = true; }
		void PSConstantsChanged() { m_ctx.aluConstPSDirty = true; }

	private:
		void ResetDirty()
		{
			m_ctx.vertexBufferDirtyMask = 0;
			m_ctx.vsUniformBufferDirtyMask = 0;
			m_ctx.psUniformBufferDirtyMask = 0;
			m_ctx.gsUniformBufferDirtyMask = 0;
			m_ctx.aluConstVSDirty = false;
			m_ctx.aluConstPSDirty = false;
		}

		bool m_inPass = false;
		LatteDrawcallContext m_ctx{};
		boost::container::small_vector<BufferCursor, 8> m_stack;
	};

	// The GPU is about to wait (fence, semaphore, flip). Games don't replace buffer
	// contents in the middle of an uninterrupted run of draws, so translated index
	// data is only dropped at these points.
	void OnEnterWait()
	{
		LatteIndices_invalidateAll();
	}

	// next dword from the TCL ring buffer; idles (and keeps timers going) while it is empty
	uint32 ReadRingWord()
	{
		uint32 word;
		while (true)
		{
			if (TCL::TCLGPUReadRBWord(word))
				return word;
			g_renderer->NotifyLatteCommandProcessorIdle();
			performanceMonitor.gpuTime_idleTime.beginMeasuring();
			for (sint32 i = 0; i < 80; i++)
				_mm_pause();
			LatteThread_HandleOSScreen();
			if (TCL::TCLGPUReadRBWord(word))
				return word;
			if (Latte_GetStopSignal())
				LatteThread_Exit();
			LatteTiming_HandleTimedVsync();
			LatteAsyncCommands_checkAndExecute();
			std::this_thread::yield();
			performanceMonitor.gpuTime_idleTime.endMeasuring();
		}
	}

	// ---- register writes ----

	// Writes `count` register values starting at `first`. With state shadowing enabled
	// (CONTEXT_CONTROL 0x80000077) every register that has a shadow address set by a
	// LOAD_* packet is also written back to guest memory. Returns true if any value changed.
	bool WriteRegisters(uint32 first, BodyReader body, uint32 count)
	{
		if (first >= LATTE_MAX_REGISTER)
			return false;
		count = std::min<uint32>(count, LATTE_MAX_REGISTER - first); // malformed packets don't write past the register file
		uint32* regs = LatteGPUState.contextRegister + first;
		bool changed = false;
		if (LatteGPUState.contextControl0 == 0x80000077)
		{
			const MPTR* shadow = LatteGPUState.contextRegisterShadowAddr + first;
			for (uint32 i = 0; i < count; i++)
			{
				const uint32 v = body.next();
				if (shadow[i] != MPTR_NULL)
					*(uint32*)(memory_base + shadow[i]) = _swapEndianU32(v);
				changed |= regs[i] != v;
				regs[i] = v;
			}
		}
		else
		{
			for (uint32 i = 0; i < count; i++)
			{
				const uint32 v = body.next();
				changed |= regs[i] != v;
				regs[i] = v;
			}
		}
		return changed;
	}

	// SET_*_REG packet body: register offset, then values
	void SetRegisters(uint32 base, CmdPtr body, uint32 nWords)
	{
		BodyReader r{ body };
		const uint32 first = base + r.next();
		WriteRegisters(first, r, nWords - 1);
	}

	// same, also reports the written range [first, last] and whether anything changed
	template<typename TOnRange>
	bool SetRegistersTracked(uint32 base, CmdPtr body, uint32 nWords, TOnRange onRange)
	{
		BodyReader r{ body };
		const uint32 first = base + r.next();
		const uint32 values = nWords - 1;
		const bool changed = WriteRegisters(first, r, values);
		onRange(first, first + values - 1, changed);
		return changed;
	}

	// LOAD_*_REG: (address, flags, {offset, count}...) - registers are loaded from guest
	// memory and remember their address for state shadowing
	void LoadRegisters(CmdPtr body, uint32 nWords, uint32 base)
	{
		if (nWords < 2 || (nWords & 1) != 0)
		{
			cemuLog_logDebug(LogType::Force, "LatteCP: malformed LOAD_REG packet ({} dwords)", nWords);
			return;
		}
		BodyReader r{ body };
		MPTR addr = r.next();
		r.next(); // wait-for-idle flag
		for (uint32 n = (nWords - 2) / 2; n > 0; n--)
		{
			uint32 reg = base + r.next();
			uint32 count = r.next();
			cemu_assert_debug(count != 0);
			for (; count > 0; count--, reg++, addr += 4)
			{
				if (reg >= LATTE_MAX_REGISTER)
					continue;
				LatteGPUState.contextRegisterShadowAddr[reg] = addr;
				LatteGPUState.contextRegister[reg] = memory_read<uint32>(addr);
			}
		}
	}

	// ---- packet handlers shared by the ring buffer and indirect buffers ----

	void CmdSurfaceSync(CmdPtr body)
	{
		BodyReader r{ body };
		const auto flags = static_cast<Latte::E_COHER_CNTL>(r.next());
		const uint32 size = r.next() << 8;
		const MPTR addr = r.next() << 8;
		r.next(); // poll interval
		if (static_cast<uint32>(flags & (Latte::E_COHER_CNTL::CB_ACTION_ENA | Latte::E_COHER_CNTL::CB_ALL_DEST_BASE_ENA)) != 0)
			g_renderer->SurfaceSync(flags, addr, size);
		// whole-memory invalidations are ignored, they would flush everything
		if (addr == MPTR_NULL || size == 0xFFFFFFFF)
			return;
		if (HAS_FLAG(flags, Latte::E_COHER_CNTL::TC_ACTION_ENA))
		{
			LatteBufferCache_invalidate(addr, size);
			LatteTexture_Invalidate(addr, size);
		}
	}

	void CmdStreamoutBufferUpdate(CmdPtr body, uint32 nWords)
	{
		cemu_assert_debug(nWords == 5);
		BodyReader r{ body };
		const uint32 control = r.next();
		const MPTR writeAddr = r.next();
		r.next();
		r.next(); // read address (reloading a stored offset is not supported)
		const uint32 mode = (control >> 1) & 3;
		const uint32 buffer = (control >> 8) & 3;
		uint32& offsetReg = LatteGPUState.contextRegister[mmVGT_STRMOUT_BUFFER_OFFSET_0 + 4 * buffer];
		if (mode == 0)
			offsetReg = 0;
		else if (mode == 3)
			memory_writeU32(memory_physicalToVirtual(writeAddr), offsetReg);
		else
			cemu_assert_unimplemented();
	}

	void CmdIndexType(CmdPtr body, uint32 nWords)
	{
		cemu_assert_debug(nWords == 1);
		LatteGPUState.contextNew.VGT_DMA_INDEX_TYPE.set_INDEX_TYPE((Latte::LATTE_VGT_DMA_INDEX_TYPE::E_INDEX_TYPE)(uint32)body[0]);
	}

	void CmdNumInstances(CmdPtr body, uint32 nWords)
	{
		cemu_assert_debug(nWords == 1);
		const uint32 n = body[0];
		LatteGPUState.contextNew.VGT_DMA_NUM_INSTANCES.set_NUM_INSTANCES(n == 0 ? 1 : n);
	}

	bool WaitConditionMet(uint32 op, uint32 value, uint32 reference)
	{
		switch (op)
		{
		case 0: return true; // always
		case 1: return value < reference;
		case 2: return value <= reference;
		case 3: return value == reference;
		case 4: return value != reference;
		case 5: return value >= reference;
		case 6: return value > reference;
		default:
			cemuLog_logOnce(LogType::Force, "LatteCP: WAIT_REG_MEM with compare function 'never'");
			return true;
		}
	}

	void CmdWaitRegMem(CmdPtr body, uint32 nWords)
	{
		cemu_assert_debug(nWords == 6);
		BodyReader r{ body };
		const uint32 control = r.next();
		const uint32 addr = r.next() & ~3u;
		r.next();
		const uint32 reference = r.next();
		const uint32 mask = r.next();
		r.next(); // poll interval
		const uint32* fence = (const uint32*)memory_getPointerFromPhysicalOffset(addr);
		OnEnterWait();
		if ((control & 0x10) == 0)
		{
			// waiting on a register is not used by GX2
			debugBreakpoint();
			return;
		}
		performanceMonitor.gpuTime_fenceTime.beginMeasuring();
		bool notified = false;
		while (!WaitConditionMet(control & 7, _swapEndianU32(*fence) & mask, reference))
		{
			if (!notified)
			{
				g_renderer->NotifyLatteCommandProcessorIdle();
				notified = true;
			}
			LatteTiming_HandleTimedVsync();
			LatteAsyncCommands_checkAndExecute();
		}
		performanceMonitor.gpuTime_fenceTime.endMeasuring();
	}

	void CmdMemWrite(CmdPtr body, uint32 nWords)
	{
		cemu_assert_debug(nWords == 4);
		BodyReader r{ body };
		const MPTR addr = r.next() & ~3u;
		const uint32 control = r.next();
		const uint32 lo = r.next();
		const uint32 hi = r.next();
		if (addr == MPTR_NULL)
		{
			cemuLog_log(LogType::Force, "LatteCP: MEM_WRITE to address 0 ignored");
			return;
		}
		void* dst = memory_getPointerFromPhysicalOffset(addr);
		switch (control)
		{
		case 0x40000: // 32 bit
			stdx::atomic_ref<uint32be>(*(uint32be*)dst).store(lo);
			break;
		case 0x00000: // 64 bit, first dword in the high half
			stdx::atomic_ref<uint64be>(*(uint64be*)dst).store(((uint64)lo << 32) | hi);
			break;
		case 0x20000: // 64 bit, little endian
			stdx::atomic_ref<uint64le>(*(uint64le*)dst).store(((uint64)hi << 32) | lo);
			break;
		default:
			cemu_assert_unimplemented();
			break;
		}
	}

	void CmdEventWriteEOP(CmdPtr body, uint32 nWords)
	{
		cemu_assert_debug(nWords == 5);
		BodyReader r{ body };
		const uint32 event = r.next();
		const uint32 addr = r.next();
		const uint32 control = r.next();
		const uint32 lo = r.next();
		const uint32 hi = r.next();
		cemu_assert_debug(control == 0x40000000 || control == 0x42000000);
		// GX2 uses this to write retirement timestamps
		if (event == 0x504 && (control & 0x40000000) != 0)
			stdx::atomic_ref<uint64be>(*(uint64be*)memory_getPointerFromPhysicalOffset(addr)).store(((uint64)hi << 32) | lo);
		else
			cemu_assert_unimplemented();
		TCL::TCLGPUNotifyNewRetirementTimestamp();
	}

	void CmdMemSemaphore(CmdPtr body, uint32 nWords)
	{
		cemu_assert_debug(nWords == 2);
		const MPTR addr = body[0];
		const uint32 action = ((uint32)body[1] >> 29) & 7;
		static_assert(sizeof(std::atomic<uint64le>) == sizeof(uint64le));
		std::atomic<uint64le>* sem = _rawPtrToAtomic((uint64le*)memory_getPointerFromPhysicalOffset(addr));
		if (action == 6)
		{
			sem->fetch_add(1);
		}
		else if (action == 7)
		{
			OnEnterWait();
			size_t emptyPolls = 0;
			while (true)
			{
				uint64le v = sem->load();
				if (v == 0)
				{
					// spin for a while, then start giving up the time slice
					if (++emptyPolls > 2000)
						std::this_thread::yield();
					continue;
				}
				if (sem->compare_exchange_strong(v, v - 1))
					break;
			}
		}
		else
			cemu_assert_debug(false);
	}

	void CmdContextControl(CmdPtr body, uint32 nWords)
	{
		cemu_assert_debug(nWords == 2);
		LatteGPUState.contextControl0 = body[0];
		LatteGPUState.contextControl1 = body[1];
	}

	void CmdSetPredication(CmdPtr body, uint32 nWords)
	{
		cemu_assert_debug(nWords == 2);
		// predicated rendering is tracked but not applied
		const bool enable = (((uint32)body[1] >> 13) & 7) != 0;
		if (enable == s_predicationActive)
			debug_printf(enable ? "LatteCP: predication already enabled\n" : "LatteCP: predication already disabled\n");
		s_predicationActive = enable;
	}

	void CmdDrawIndex2(CmdPtr body, uint32 nWords, Walker& w)
	{
		cemu_assert_debug(nWords == 5);
		const MPTR indices = body[1];
		const uint32 count = body[3];
		LatteGPUState.currentDrawCallTick = GetTickCount();
		w.Draw(count, false, indices);
	}

	void CmdDrawIndexAuto(CmdPtr body, uint32 nWords, Walker& w)
	{
		cemu_assert_debug(nWords == 2);
		const uint32 count = body[0];
		LatteGPUState.currentDrawCallTick = GetTickCount();
		// GX2 dispatches compute work as an auto draw with this shader priority order
		auto& sq = LatteGPUState.contextNew.SQ_CONFIG;
		if (sq.get_PS_PRIO() == 0 && sq.get_VS_PRIO() == 1 && sq.get_GS_PRIO() == 2 && sq.get_ES_PRIO() == 3)
		{
			cemuLog_logDebug(LogType::Force, "LatteCP: compute dispatch ({} threads, program {:08x}, size {:08x}) is not supported", count,
				(LatteGPUState.contextRegister[mmSQ_PGM_START_ES] & 0xFFFFFF) << 8, LatteGPUState.contextRegister[mmSQ_PGM_START_ES + 1] << 3);
			return;
		}
		w.Draw(count, true, MPTR_NULL);
	}

	// indices are embedded in the packet; they are copied to guest memory so the draw can use them like any index buffer
	void CmdDrawImmediate(CmdPtr body, uint32 nWords, Walker& w)
	{
		BodyReader r{ body };
		const uint32 count = r.next();
		r.next();
		if (s_immediateIndexBuffer == MPTR_NULL)
			s_immediateIndexBuffer = coreinit_allocFromSysArea(kImmediateIndexBufferWords * sizeof(uint32), 0x4);
		LatteGPUState.currentDrawCallTick = GetTickCount();
		uint32* dst = (uint32*)memory_getPointerFromVirtualOffset(s_immediateIndexBuffer);
		const auto type = LatteGPUState.contextNew.VGT_DMA_INDEX_TYPE.get_INDEX_TYPE();
		uint32 words;
		if (type == Latte::LATTE_VGT_DMA_INDEX_TYPE::E_INDEX_TYPE::U16_BE)
			words = (count + 1) / 2;
		else if (type == Latte::LATTE_VGT_DMA_INDEX_TYPE::E_INDEX_TYPE::U32_BE)
		{
			cemu_assert_debug(false); // not seen in games yet
			words = count;
		}
		else
		{
			cemuLog_log(LogType::Force, "LatteCP: DRAW_INDEX_IMMD with unsupported index type");
			return;
		}
		const uint32 copyWords = std::min(words, kImmediateIndexBufferWords);
		memcpy(dst, r.p, copyWords * sizeof(uint32));
		if (type == Latte::LATTE_VGT_DMA_INDEX_TYPE::E_INDEX_TYPE::U16_BE)
		{
			// the packet stores the first index of a pair in the low half of the dword
			for (uint32 i = 0; i < copyWords; i++)
				dst[i] = std::rotl<uint32>(dst[i], 16);
		}
		LatteIndices_invalidate(dst, words * sizeof(uint32));
		cemu_assert_debug(nWords == 2 + words);
		w.Draw(count, false, s_immediateIndexBuffer);
	}

	void CmdHLESampleTimer(CmdPtr body, uint32 nWords)
	{
		cemu_assert_debug(nWords == 1);
		memory_writeU64((MPTR)(uint32)body[0], coreinit::OSGetSystemTime());
	}

	void CmdHLESpecialState(CmdPtr body, uint32 nWords)
	{
		cemu_assert_debug(nWords == 2);
		const uint32 id = body[0];
		if (id >= GX2_SPECIAL_STATE_COUNT)
		{
			cemu_assert_suspicious();
			return;
		}
		LatteGPUState.contextNew.GetSpecialStateValues()[id] = body[1];
	}

	void CmdHLEBottomOfPipe(CmdPtr body, uint32 nWords)
	{
		cemu_assert_debug(nWords == 3);
		const MPTR addr = body[0];
		*(uint32be*)memory_getPointerFromPhysicalOffset(addr) = (uint32)body[1];
		*(uint32be*)memory_getPointerFromPhysicalOffset(addr + 4) = (uint32)body[2];
		GX2::__GX2NotifyEvent(GX2::GX2CallbackEventType::TIMESTAMP_BOTTOM);
	}

	void ReadSurfaceCopyParam(BodyReader& r, LatteSurfaceCopyParam& p)
	{
		p.physDataAddr = r.next();
		p.swizzle = r.next();
		p.surfaceFormat = (Latte::E_GX2SURFFMT)r.next();
		p.pitch = r.next();
		p.heightInTexels = r.next();
		p.sliceIndex = r.next();
		p.dim = (Latte::E_DIM)r.next();
		p.tilemode = (Latte::E_GX2TILEMODE)r.next();
		p.aa = r.next();
	}

	// GX2CopySurface(Ex)
	void CmdHLECopySurface(CmdPtr body, uint32 nWords)
	{
		cemu_assert_debug(nWords == 4 + 9 * 2);
		BodyReader r{ body };
		LatteSurfaceCopyRect rect;
		rect.x = r.next();
		rect.y = r.next();
		rect.width = r.next();
		rect.height = r.next();
		LatteSurfaceCopyParam src{}, dst{};
		ReadSurfaceCopyParam(r, src);
		ReadSurfaceCopyParam(r, dst);
		LatteSurfaceCopy_copySurfaceNew(src, dst, rect);
	}

	void CmdHLEClear(CmdPtr body, uint32 nWords)
	{
		cemu_assert_debug(nWords == 23);
		BodyReader r{ body };
		const uint32 mask = r.next(); // 1 = color, 2 = depth, 4 = stencil
		const MPTR cAddr = r.next();
		const auto cFormat = (Latte::E_GX2SURFFMT)r.next();
		const auto cTileMode = (Latte::E_HWTILEMODE)r.next();
		const uint32 cWidth = r.next();
		const uint32 cHeight = r.next();
		const uint32 cPitch = r.next();
		const uint32 cFirstSlice = r.next();
		const uint32 cNumSlices = r.next();
		const MPTR dAddr = r.next();
		const auto dFormat = (Latte::E_GX2SURFFMT)r.next();
		const auto dTileMode = (Latte::E_HWTILEMODE)r.next();
		const uint32 dWidth = r.next();
		const uint32 dHeight = r.next();
		const uint32 dPitch = r.next();
		const uint32 dFirstSlice = r.next();
		const uint32 dNumSlices = r.next();
		float rgba[4];
		for (float& c : rgba)
			c = (float)r.next() / 255.0f;
		const uint32 depthBits = r.next();
		float depth;
		memcpy(&depth, &depthBits, sizeof(depth));
		const uint32 stencil = r.next();
		LatteRenderTarget_itHLEClearColorDepthStencil(mask,
			cAddr, cFormat, cTileMode, cWidth, cHeight, cPitch, cFirstSlice, cNumSlices,
			dAddr, dFormat, dTileMode, dWidth, dHeight, dPitch, dFirstSlice, dNumSlices,
			rgba[0], rgba[1], rgba[2], rgba[3], depth, stencil);
	}

	void CmdHLEWaitForFlip()
	{
		const uint32 flips = LatteGPUState.flipCounter;
		do
		{
			_mm_pause();
			if (flips != LatteGPUState.flipCounter)
				break;
			LatteTiming_HandleTimedVsync();
			std::this_thread::yield();
		} while (true);
	}

	void CmdHLECopyToScanBuffer(CmdPtr body)
	{
		BodyReader r{ body };
		const MPTR addr = r.next();
		const uint32 width = r.next();
		const uint32 height = r.next();
		const uint32 pitch = r.next();
		const auto tileMode = (Latte::E_HWTILEMODE)r.next();
		const uint32 swizzle = r.next();
		const uint32 slice = r.next();
		const uint32 format = r.next();
		const uint32 target = r.next();
		LatteRenderTarget_itHLECopyColorBufferToScanBuffer(addr, width, height, slice, format, pitch, tileMode, swizzle, target);
	}

	// Packets that behave the same wherever they appear. Returns false for anything else.
	bool ExecuteCommon(uint32 op, CmdPtr body, uint32 nWords)
	{
		switch (op)
		{
		case IT_SET_CONTEXT_REG:
		case IT_SET_ALL_CONTEXTS: SetRegisters(LATTE_REG_BASE_CONTEXT, body, nWords); return true;
		case IT_SET_RESOURCE: SetRegisters(LATTE_REG_BASE_RESOURCE, body, nWords); return true;
		case IT_SET_ALU_CONST: SetRegisters(LATTE_REG_BASE_ALU_CONST, body, nWords); return true;
		case IT_SET_CTL_CONST: SetRegisters(mmSQ_VTX_BASE_VTX_LOC, body, nWords); return true;
		case IT_SET_SAMPLER: SetRegisters(LATTE_REG_BASE_SAMPLER, body, nWords); return true;
		case IT_SET_CONFIG_REG: SetRegisters(LATTE_REG_BASE_CONFIG, body, nWords); return true;
		case IT_SET_LOOP_CONST: return true; // loop constants are not used by the shader translators
		case IT_SURFACE_SYNC: CmdSurfaceSync(body); return true;
		case IT_STRMOUT_BUFFER_UPDATE: CmdStreamoutBufferUpdate(body, nWords); return true;
		case IT_INDEX_TYPE: CmdIndexType(body, nWords); return true;
		case IT_NUM_INSTANCES: CmdNumInstances(body, nWords); return true;
		case IT_MEM_WRITE: CmdMemWrite(body, nWords); return true;
		case IT_CONTEXT_CONTROL: CmdContextControl(body, nWords); return true;
		case IT_MEM_SEMAPHORE: CmdMemSemaphore(body, nWords); return true;
		case IT_LOAD_CONFIG_REG: LoadRegisters(body, nWords, LATTE_REG_BASE_CONFIG); return true;
		case IT_LOAD_CONTEXT_REG: LoadRegisters(body, nWords, LATTE_REG_BASE_CONTEXT); return true;
		case IT_LOAD_ALU_CONST: LoadRegisters(body, nWords, LATTE_REG_BASE_ALU_CONST); return true;
		case IT_LOAD_LOOP_CONST: LoadRegisters(body, nWords, LATTE_REG_BASE_LOOP_CONST); return true;
		case IT_LOAD_RESOURCE: LoadRegisters(body, nWords, LATTE_REG_BASE_RESOURCE); return true;
		case IT_LOAD_SAMPLER: LoadRegisters(body, nWords, LATTE_REG_BASE_SAMPLER); return true;
		case IT_SET_PREDICATION: CmdSetPredication(body, nWords); return true;
		case IT_HLE_COPY_COLORBUFFER_TO_SCANBUFFER: CmdHLECopyToScanBuffer(body); return true;
		case IT_HLE_TRIGGER_SCANBUFFER_SWAP:
			cemu_assert_debug(nWords == 1);
			OnEnterWait();
			LatteRenderTarget_itHLESwapScanBuffer();
			return true;
		case IT_HLE_WAIT_FOR_FLIP:
			cemu_assert_debug(nWords == 1);
			OnEnterWait();
			CmdHLEWaitForFlip();
			return true;
		case IT_HLE_REQUEST_SWAP_BUFFERS:
			cemu_assert_debug(nWords == 1);
			LatteGPUState.flipRequestCount.fetch_add(1); // completed by the next flip
			return true;
		case IT_HLE_CLEAR_COLOR_DEPTH_STENCIL: CmdHLEClear(body, nWords); return true;
		case IT_HLE_COPY_SURFACE_NEW: CmdHLECopySurface(body, nWords); return true;
		case IT_HLE_SAMPLE_TIMER: CmdHLESampleTimer(body, nWords); return true;
		case IT_HLE_SPECIAL_STATE: CmdHLESpecialState(body, nWords); return true;
		case IT_HLE_BEGIN_OCCLUSION_QUERY:
			cemu_assert_debug(nWords == 1);
			LatteQuery_BeginOcclusionQuery((MPTR)(uint32)body[0]);
			return true;
		case IT_HLE_END_OCCLUSION_QUERY:
			cemu_assert_debug(nWords == 1);
			LatteQuery_EndOcclusionQuery((MPTR)(uint32)body[0]);
			return true;
		case IT_HLE_BOTTOM_OF_PIPE_CB: CmdHLEBottomOfPipe(body, nWords); return true;
		case IT_HLE_SYNC_ASYNC_OPERATIONS:
			LatteTextureReadback_UpdateFinishedTransfers(true);
			LatteQuery_UpdateFinishedQueriesForceFinishAll();
			return true;
		default:
			return false;
		}
	}

	// INDIRECT_BUFFER_PRIV body: address, high address (unused), size in dwords
	bool ReadIndirectBuffer(CmdPtr body, uint32 nWords, CmdPtr& begin, CmdPtr& end)
	{
		cemu_assert_debug(nWords == 3);
		const uint32 addr = body[0];
		const uint32 dwords = body[2];
		if (dwords == 0)
			return false;
		begin = MEMPTR<uint32be>(addr).GetPtr();
		end = begin + dwords;
		return true;
	}

	void LogBufferError(CmdPtr begin, CmdPtr end, CmdPtr at)
	{
		cemuLog_log(LogType::Force, "LatteCP: invalid packet in command buffer {:08x}-{:08x} at {:08x}",
			memory_getVirtualOffsetFromPointer(begin), memory_getVirtualOffsetFromPointer(end), memory_getVirtualOffsetFromPointer(at));
		for (CmdPtr p = begin; p < end; p += 4)
		{
			const bool here = at >= p && at < p + 4;
			cemuLog_log(LogType::Force, "{:08x}: {:08x} {:08x} {:08x} {:08x}{}", memory_getVirtualOffsetFromPointer(p),
				(uint32)p[0], (uint32)p[1], (uint32)p[2], (uint32)p[3], here ? " <--" : "");
		}
		cemuLog_waitForFlush();
		cemu_assert_debug(false);
	}

	bool IsTextureResourceRange(uint32 reg)
	{
		constexpr uint32 span = Latte::GPU_LIMITS::NUM_TEXTURES_PER_STAGE * 7;
		for (uint32 stageStart : { (uint32)Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS, (uint32)Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_VS, (uint32)Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_GS })
		{
			if (reg >= stageStart && reg < stageStart + span)
				return true;
		}
		return false;
	}

	// Continues the current draw pass after a draw. Returns once the pass ends. If it
	// ended on a packet the pass can't handle, the cursor to resume at (that packet
	// or the one after) is on the stack; if all buffers ran out the stack is empty.
	void ContinueDrawPass(Walker& w)
	{
		cemu_assert_debug(w.InPass());
		// streamout needs the full setup for every draw
		if (LatteGPUState.contextRegister[mmVGT_STRMOUT_EN] != 0)
		{
			w.EndPass();
			return;
		}
		BufferCursor c;
		while (w.Pop(c))
		{
			while (c.pos < c.end)
			{
				CmdPtr packetStart = c.pos;
				const PacketHeader h{ (uint32)*c.pos++ };
				if (h.type() == 2)
					continue;
				// leave the pass, resuming at `resumeAt`
				auto leave = [&](CmdPtr resumeAt)
				{
					w.EndPass();
					w.Push(resumeAt, c.begin, c.end);
				};
				if (h.type() != 3)
				{
					leave(packetStart);
					return;
				}
				const uint32 nWords = h.count();
				CmdPtr body = c.pos;
				c.pos += nWords;
				switch (h.opcode())
				{
				case IT_SET_RESOURCE:
				{
					SetRegistersTracked(LATTE_REG_BASE_RESOURCE, body, nWords, [&w](uint32 first, uint32 last, bool changed)
					{
						if (!changed)
							return;
						if (IsTextureResourceRange(first))
							w.EndPass(); // new textures need the full setup
						else if (first >= mmSQ_VTX_ATTRIBUTE_BLOCK_START && last <= mmSQ_VTX_ATTRIBUTE_BLOCK_END)
							w.VertexBufferChanged((first - mmSQ_VTX_ATTRIBUTE_BLOCK_START) / 7);
						else if (first >= mmSQ_VTX_UNIFORM_BLOCK_START && last <= mmSQ_VTX_UNIFORM_BLOCK_END)
							w.VSUniformBufferChanged((first - mmSQ_VTX_UNIFORM_BLOCK_START) / 7);
						else if (first >= mmSQ_PS_UNIFORM_BLOCK_START && last <= mmSQ_PS_UNIFORM_BLOCK_END)
							w.PSUniformBufferChanged((first - mmSQ_PS_UNIFORM_BLOCK_START) / 7);
						else if (first >= mmSQ_GS_UNIFORM_BLOCK_START && last <= mmSQ_GS_UNIFORM_BLOCK_END)
							w.GSUniformBufferChanged((first - mmSQ_GS_UNIFORM_BLOCK_START) / 7);
					});
					if (!w.InPass())
					{
						w.Push(c.pos, c.begin, c.end);
						return;
					}
					break;
				}
				case IT_SET_ALU_CONST:
					SetRegistersTracked(LATTE_REG_BASE_ALU_CONST, body, nWords, [&w](uint32 first, uint32 last, bool changed)
					{
						if (!changed)
							return;
						// the first 256 vec4 constants belong to the pixel shader, the next 256 to the vertex shader
						if (first >= mmSQ_ALU_CONSTANT0_0 + 0x400)
							w.VSConstantsChanged();
						else
							w.PSConstantsChanged();
					});
					break;
				case IT_SET_CTL_CONST:
					SetRegisters(mmSQ_VTX_BASE_VTX_LOC, body, nWords);
					break;
				case IT_SET_CONFIG_REG:
					SetRegisters(LATTE_REG_BASE_CONFIG, body, nWords);
					break;
				case IT_INDEX_TYPE:
					CmdIndexType(body, nWords);
					break;
				case IT_NUM_INSTANCES:
					CmdNumInstances(body, nWords);
					break;
				case IT_DRAW_INDEX_2:
					CmdDrawIndex2(body, nWords, w);
					break;
				case IT_SET_CONTEXT_REG:
				case IT_SET_SAMPLER:
				{
					// only a real change of context or sampler state ends the pass
					const uint32 base = h.opcode() == IT_SET_CONTEXT_REG ? LATTE_REG_BASE_CONTEXT : LATTE_REG_BASE_SAMPLER;
					if (SetRegistersTracked(base, body, nWords, [](uint32, uint32, bool) {}))
					{
						leave(c.pos);
						return;
					}
					break;
				}
				case IT_INDIRECT_BUFFER_PRIV:
				{
					// descend into the sub buffer, the pass carries on inside it
					CmdPtr subBegin, subEnd;
					if (ReadIndirectBuffer(body, nWords, subBegin, subEnd))
					{
						w.Push(c.pos, c.begin, c.end);
						c = { subBegin, subBegin, subEnd };
					}
					break;
				}
				default:
					leave(packetStart);
					return;
				}
			}
		}
		w.EndPass();
	}

	// Walks the buffers on the walker's stack until all of them are done.
	void WalkBuffers(Walker& w)
	{
		BufferCursor c;
		while (w.Pop(c))
		{
			while (c.pos < c.end)
			{
				const PacketHeader h{ (uint32)*c.pos++ };
				if (h.type() == 3)
				{
					const uint32 op = h.opcode();
					const uint32 nWords = h.count();
					CmdPtr body = c.pos;
					c.pos += nWords;
					if (ExecuteCommon(op, body, nWords))
						continue;
					switch (op)
					{
					case IT_INDIRECT_BUFFER_PRIV:
					{
						CmdPtr subBegin, subEnd;
						if (ReadIndirectBuffer(body, nWords, subBegin, subEnd))
						{
							w.Push(c.pos, c.begin, c.end);
							c = { subBegin, subBegin, subEnd };
						}
						break;
					}
					case IT_DRAW_INDEX_2:
					case IT_DRAW_INDEX_AUTO:
						w.BeginPass();
						if (op == IT_DRAW_INDEX_2)
							CmdDrawIndex2(body, nWords, w);
						else
							CmdDrawIndexAuto(body, nWords, w);
						w.Push(c.pos, c.begin, c.end);
						ContinueDrawPass(w);
						cemu_assert_debug(!w.InPass());
						if (!w.Pop(c))
							return; // the pass consumed every buffer
						break;
					case IT_DRAW_INDEX_IMMD:
					{
						// its own pass, separate from this walk's
						Walker single;
						single.BeginPass();
						CmdDrawImmediate(body, nWords, single);
						single.EndPass();
						break;
					}
					case IT_WAIT_REG_MEM:
						CmdWaitRegMem(body, nWords);
						LatteTiming_HandleTimedVsync();
						LatteAsyncCommands_checkAndExecute();
						break;
					default:
						debug_printf("LatteCP: unhandled packet %02x in command buffer\n", op);
						cemu_assert_debug(false);
						break;
					}
				}
				else if (h.type() == 2)
				{
					// padding
				}
				else if (h.type() == 0)
				{
					// register writes by address; only the two GX2 timestamp registers are expected
					if (h.regBase() == 0x304A)
					{
						GX2::__GX2NotifyEvent(GX2::GX2CallbackEventType::TIMESTAMP_TOP);
						c.pos += h.count();
					}
					else if (h.regBase() == 0x304B)
						c.pos += h.count();
					else
					{
						LogBufferError(c.begin, c.end, c.pos);
						cemu_assert_debug(false);
					}
				}
				else
				{
					debug_printf("LatteCP: type 1 packet in command buffer\n");
					LogBufferError(c.begin, c.end, c.pos);
					cemu_assert_debug(false);
				}
			}
			cemu_assert_debug(c.pos == c.end);
		}
	}

	// how much a ring buffer packet counts toward the next timer check (1024 = check now)
	sint32 RingPacketCost(uint32 op)
	{
		switch (op)
		{
		case IT_INDEX_TYPE:
		case IT_NUM_INSTANCES:
			return 1;
		case IT_SURFACE_SYNC:
		case IT_SET_CONTEXT_REG:
		case IT_SET_ALL_CONTEXTS:
		case IT_SET_RESOURCE:
		case IT_SET_ALU_CONST:
		case IT_SET_CTL_CONST:
		case IT_SET_SAMPLER:
		case IT_SET_CONFIG_REG:
		case IT_INDIRECT_BUFFER_PRIV:
		case IT_STRMOUT_BUFFER_UPDATE:
		case IT_DRAW_INDEX_AUTO:
		case IT_SET_PREDICATION:
		case IT_HLE_SAMPLE_TIMER:
		case IT_HLE_SPECIAL_STATE:
		case IT_HLE_BEGIN_OCCLUSION_QUERY:
		case IT_HLE_END_OCCLUSION_QUERY:
			return 2;
		case IT_MEM_WRITE:
		case IT_CONTEXT_CONTROL:
		case IT_MEM_SEMAPHORE:
		case IT_HLE_CLEAR_COLOR_DEPTH_STENCIL:
		case IT_HLE_COPY_SURFACE_NEW:
			return 8;
		case IT_DRAW_INDEX_2:
		case IT_DRAW_INDEX_IMMD:
		case IT_LOAD_CONFIG_REG:
		case IT_LOAD_CONTEXT_REG:
		case IT_LOAD_ALU_CONST:
		case IT_LOAD_LOOP_CONST:
		case IT_LOAD_RESOURCE:
		case IT_LOAD_SAMPLER:
		case IT_HLE_COPY_COLORBUFFER_TO_SCANBUFFER:
		case IT_HLE_TRIGGER_SCANBUFFER_SWAP:
			return 16;
		case IT_HLE_REQUEST_SWAP_BUFFERS:
			return 32;
		case IT_WAIT_REG_MEM:
			return 64;
		case IT_HLE_WAIT_FOR_FLIP:
			return 1024;
		default:
			return 0;
		}
	}
	constexpr sint32 kTimerCheckThreshold = 1024;

	// a draw packet sent directly through the ring buffer (unusual, GX2 always uses command buffers)
	void RingDraw(uint32 op, CmdPtr body, uint32 nWords)
	{
		Walker single;
		single.BeginPass();
		if (op == IT_DRAW_INDEX_2)
		{
			cemuLog_log(LogType::Force, "LatteCP: DRAW_INDEX_2 in ring buffer");
			CmdDrawIndex2(body, nWords, single);
		}
		else if (op == IT_DRAW_INDEX_AUTO)
		{
			cemuLog_log(LogType::Force, "LatteCP: DRAW_INDEX_AUTO in ring buffer");
			CmdDrawIndexAuto(body, nWords, single);
		}
		else
		{
			cemuLog_log(LogType::Force, "LatteCP: DRAW_INDEX_IMMD in ring buffer");
			CmdDrawImmediate(body, nWords, single);
		}
		single.EndPass();
	}
}

void LatteCP_ProcessRingbuffer()
{
	// packets are gathered into this buffer before they are executed
	// (the largest body a header can describe is 0x4000 dwords)
	static uint32be s_packet[0x4000];
	sint32 timerBudget = 0;
	while (true)
	{
		const PacketHeader h{ ReadRingWord() };
		if (h.type() == 3)
		{
			const uint32 op = h.opcode();
			const uint32 nWords = h.count();
			cemu_assert(nWords < 128);
			for (uint32 i = 0; i < nWords; i++)
				s_packet[i] = ReadRingWord();
			CmdPtr body = s_packet;
			if (!ExecuteCommon(op, body, nWords))
			{
				switch (op)
				{
				case IT_INDIRECT_BUFFER_PRIV:
				{
					// run the whole buffer (and everything it calls) now
					CmdPtr begin, end;
					if (ReadIndirectBuffer(body, nWords, begin, end))
					{
						Walker w;
						w.Push(begin, begin, end);
						WalkBuffers(w);
						if (w.InPass())
							w.EndPass();
					}
					break;
				}
				case IT_DRAW_INDEX_2:
				case IT_DRAW_INDEX_AUTO:
				case IT_DRAW_INDEX_IMMD:
					RingDraw(op, body, nWords);
					break;
				case IT_WAIT_REG_MEM:
					CmdWaitRegMem(body, nWords);
					break;
				case IT_EVENT_WRITE_EOP:
					CmdEventWriteEOP(body, nWords);
					break;
				default:
					cemu_assert_debug(false);
					break;
				}
			}
			timerBudget += RingPacketCost(op);
		}
		else if (h.type() == 2)
		{
			cemu_assert_debug(h.raw == 0x80000000);
		}
		else if (h.type() == 0)
		{
			if (h.regBase() == 0x304A)
			{
				GX2::__GX2NotifyEvent(GX2::GX2CallbackEventType::TIMESTAMP_TOP);
				for (uint32 i = h.count(); i > 0; i--)
					ReadRingWord();
			}
			else if (h.regBase() == 0x304B)
			{
				for (uint32 i = h.count(); i > 0; i--)
					ReadRingWord();
			}
			else
				cemu_assert_debug(false);
		}
		else
		{
			debug_printf("LatteCP: type 1 packet in ring buffer\n");
			cemu_assert_debug(false);
		}
		if (timerBudget >= kTimerCheckThreshold)
		{
			LatteTiming_HandleTimedVsync();
			LatteAsyncCommands_checkAndExecute();
			timerBudget = 0;
		}
	}
}
