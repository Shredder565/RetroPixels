// Original code for RetroPixels (PixelU). Not derived from Cemu.
// Interface between the h264 export layer (H264Dec.cpp) and a decoder backend.
//
// The export layer queues compressed pictures; the backend decodes them on its own thread and
// hands finished frames back in display order. Up to 32 pictures can be in flight.
#pragma once

#include "util/helpers/Semaphore.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "Cafe/OS/libs/coreinit/coreinit_SysHeap.h"

#include "Cafe/OS/libs/h264_avc/parser/H264Parser.h"

namespace H264
{
	class H264DecoderBackend
	{
	public:
		// a decoded frame as reported to the guest
		struct DecodeResult
		{
			bool isDecoded{false};
			bool hasFrame{false};
			double timestamp{};
			void* imageOutput{nullptr}; // guest buffer the frame is written to
			sint32 frameWidth{0};
			sint32 frameHeight{0};
			uint32 bytesPerRow{0};
			bool cropEnable{false};
			sint32 cropTop{0};
			sint32 cropBottom{0};
			sint32 cropLeft{0};
			sint32 cropRight{0};
		};

		H264DecoderBackend()
		{
			m_displayQueueEvt = NewAutoResetEvent();
			m_flushEvt = NewAutoResetEvent();
		}

		virtual ~H264DecoderBackend()
		{
			coreinit::OSFreeToSystem(m_displayQueueEvt);
			coreinit::OSFreeToSystem(m_flushEvt);
		}

		virtual void Init(bool isBufferedMode) = 0;
		virtual void Destroy() = 0;

		// copies the data and queues it for decoding
		void QueueForDecode(uint8* data, uint32 length, double timestamp, void* imagePtr)
		{
			std::lock_guard lock(m_decodeQueueMtx);
			const uint32 index = ClaimSlot();
			DecodedSlice& slot = m_decodedSliceArray[index];
			slot.dataToDecode.m_buffer.assign(data, data + length);
			slot.dataToDecode.m_data = slot.dataToDecode.m_buffer.data();
			slot.dataToDecode.m_length = length;
			slot.result.isDecoded = false;
			slot.result.imageOutput = imagePtr;
			slot.result.timestamp = timestamp;
			m_decodeQueue.push_back(index);
			m_decodeSem.increment();
		}

		// asks the decoder thread to drain all buffered frames; m_flushEvt is signalled when done
		void QueueFlush()
		{
			std::lock_guard lock(m_decodeQueueMtx);
			m_decodeQueue.push_back(CMD_FLUSH);
			m_decodeSem.increment();
		}

		// takes the next frame in display order and frees its slot
		bool GetFrameOutputIfReady(DecodeResult& result)
		{
			std::lock_guard lock(m_decodeQueueMtx);
			if (m_displayQueue.empty())
				return false;
			DecodedSlice& slot = m_decodedSliceArray[m_displayQueue.front()];
			m_displayQueue.erase(m_displayQueue.begin());
			cemu_assert_debug(slot.result.isDecoded);
			std::swap(result, slot.result);
			slot.isUsed = false;
			return true;
		}

		coreinit::OSEvent& GetFrameOutputEvent() { return *m_displayQueueEvt; }
		coreinit::OSEvent& GetFlushEvent() { return *m_flushEvt; }

	protected:
		struct DataToDecode
		{
			uint8* m_data;
			uint32 m_length;
			std::vector<uint8> m_buffer;
		};

		struct DecodedSlice
		{
			bool isUsed{false};
			DecodeResult result;
			DataToDecode dataToDecode;
		};

		static constexpr uint32 CMD_FLUSH = 0xFFFFFFFF; // queue entry that requests a flush

		// first free slot; when all are taken slot 0 is reused
		uint32 ClaimSlot()
		{
			for (uint32 i = 0; i < m_decodedSliceArray.size(); i++)
			{
				if (!m_decodedSliceArray[i].isUsed)
				{
					m_decodedSliceArray[i].isUsed = true;
					return i;
				}
			}
			cemu_assert_suspicious();
			return 0;
		}

		std::mutex m_decodeQueueMtx; // guards the queues and the slots
		std::vector<uint32> m_decodeQueue; // slot indices (or CMD_FLUSH) in submission order
		CounterSemaphore m_decodeSem;
		std::vector<uint32> m_displayQueue; // slot indices in display order
		coreinit::OSEvent* m_displayQueueEvt; // signalled whenever a frame was added to m_displayQueue
		coreinit::OSEvent* m_flushEvt;
		std::array<DecodedSlice, 32> m_decodedSliceArray;

	private:
		static coreinit::OSEvent* NewAutoResetEvent()
		{
			auto* evt = (coreinit::OSEvent*)coreinit::OSAllocFromSystem(sizeof(coreinit::OSEvent), 4);
			coreinit::OSInitEvent(evt, coreinit::OSEvent::EVENT_STATE::STATE_NOT_SIGNALED, coreinit::OSEvent::EVENT_MODE::MODE_AUTO);
			return evt;
		}
	};

	H264DecoderBackend* CreateAVCDecoder();
}
