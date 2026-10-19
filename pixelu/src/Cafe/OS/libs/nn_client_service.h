// Original code for RetroPixels (PixelU). Not derived from Cemu.
//
// PPC side of the nn service IPC protocol (/dev/boss and friends). One call is
// a single IOS_Ioctlv with request 0 whose vectors are laid out as:
//
//   [response 0] [extra responses...] [request 0] [extra requests...]
//
// Request 0 starts with a 16-byte header (0, service id, 0, command id)
// followed by the serialized parameters; response 0 starts with the nn::Result
// followed by the serialized return values. A caller buffer of arbitrary
// alignment becomes two vectors: one for its 64-byte aligned middle part
// (pointing straight at the caller's memory) and one for a staging buffer
// holding the unaligned head (just below offset 0x40) and tail (from offset
// 0x40). The split is described in the parameter stream as
// { u32 alignedSize, u8 0, u8 0, u8 headSize, u8 tailSize }.
#pragma once
#include "Cafe/OS/libs/coreinit/coreinit_IPC.h"
#include <boost/container/static_vector.hpp>

class IPCServiceClient
{
	struct IPCBuffer
	{
		uint8 data[256];
	};
	static_assert(sizeof(IPCBuffer) == 256);

	static constexpr uint32 kAlign = 0x40;

public:
	class IPCServiceCall
	{
		struct Vec
		{
			Vec() = default;
			Vec(uint8* p, uint32 n, IPCBuffer* owner) : ptr(p), size(n), owner(owner) {}
			uint8* ptr{ nullptr };
			uint32 size{ 0 };
			IPCBuffer* owner{ nullptr }; // pool buffer holding the data, null for caller memory
		};

		// staged output bytes copied back to the caller once the call succeeded
		struct CopyBack
		{
			CopyBack(void* to, const void* from, uint32 n) : to(to), from(from), size(n) {}
			void* to;
			const void* from;
			uint32 size;
		};

	public:
		IPCServiceCall(IPCServiceClient& client, uint32 serviceId, uint32 commandId) : m_client(client)
		{
			IPCBuffer* req = client.TakeBuffer();
			m_in.emplace_back(req->data, (uint32)sizeof(IPCBuffer), req);
			IPCBuffer* resp = client.TakeBuffer();
			m_out.emplace_back(resp->data, (uint32)sizeof(IPCBuffer), resp);
			WriteParam<uint32be>(0);
			WriteParam<uint32be>(serviceId);
			WriteParam<uint32be>(0);
			WriteParam<uint32be>(commandId);
		}

		~IPCServiceCall()
		{
			for (auto* list : { &m_in, &m_out })
			{
				for (Vec& v : *list)
				{
					if (v.owner)
						m_client.GiveBackBuffer(v.owner);
				}
			}
		}

		IPCServiceCall(const IPCServiceCall&) = delete;
		IPCServiceCall& operator=(const IPCServiceCall&) = delete;

		// append raw bytes of a value to the request
		template<typename T>
		void WriteParam(const T& value)
		{
			cemu_assert(m_writePos + sizeof(T) <= m_in[0].size);
			memcpy(m_in[0].ptr + m_writePos, &value, sizeof(T));
			m_writePos += (uint32)sizeof(T);
		}

		// caller buffer read by the service
		void WriteParamBuffer(MEMPTR<void> ptr, uint32 size)
		{
			AddBuffer(MEMPTR<uint8>(ptr), size, false);
		}

		// caller buffer filled by the service
		void WriteResponseBuffer(MEMPTR<void> ptr, uint32 size)
		{
			AddBuffer(MEMPTR<uint8>(ptr), size, true);
		}

		nnResult Submit()
		{
			const uint32 total = (uint32)(m_out.size() + m_in.size());
			cemu_assert(total <= 16);
			StackAllocator<IPCIoctlVector, 16> vecs;
			uint32 n = 0;
			for (auto* list : { &m_out, &m_in })
			{
				for (Vec& v : *list)
				{
					vecs[n].baseVirt = MEMPTR<uint8>(v.ptr);
					vecs[n].basePhys = nullptr;
					vecs[n].size = v.size;
					n++;
				}
			}
			IOS_ERROR err = coreinit::IOS_Ioctlv(m_client.GetDevHandle(), 0, (uint32)m_out.size(), (uint32)m_in.size(), vecs.GetPointer());
			if (IOS_ResultIsError(err))
				cemu_assert_unimplemented(); // transport errors are not expected from the emulated services
			const nnResult result = (uint32)ReadResponse<uint32be>();
			if (NN_RESULT_IS_SUCCESS(result))
			{
				for (const CopyBack& c : m_copyBack)
					memcpy(c.to, c.from, c.size);
			}
			return result;
		}

		// next raw value from the response (the first one is the result code)
		template<typename T>
		T ReadResponse()
		{
			cemu_assert(m_readPos + sizeof(T) <= m_out[0].size);
			T value;
			memcpy(&value, m_out[0].ptr + m_readPos, sizeof(T));
			m_readPos += (uint32)sizeof(T);
			return value;
		}

	private:
		void AddBuffer(MEMPTR<uint8> ptr, uint32 size, bool isOutput)
		{
			auto& list = isOutput ? m_out : m_in;
			cemu_assert(list.size() + 2 <= list.capacity());

			const uint32 addr = ptr.GetMPTR();
			const uint32 head = std::min<uint32>((kAlign - (addr % kAlign)) % kAlign, size);
			const uint32 rest = size - head;
			const uint32 tail = rest % kAlign;
			const uint32 middle = rest - tail;
			uint8* callerMem = ptr.GetPtr();

			list.emplace_back(callerMem + head, middle, nullptr);

			// staging buffer: head ends at offset 0x40, tail starts there
			IPCBuffer* stageBuf = m_client.TakeBuffer();
			uint8* stage = stageBuf->data;
			uint8* headAt = stage + kAlign - head;
			uint8* tailAt = stage + kAlign;
			if (head)
			{
				if (isOutput)
					m_copyBack.emplace_back(callerMem, headAt, head);
				else
					memcpy(headAt, callerMem, head);
			}
			if (tail)
			{
				if (isOutput)
					m_copyBack.emplace_back(callerMem + head + middle, tailAt, tail);
				else
					memcpy(tailAt, callerMem + head + middle, tail);
			}
			// the vector covers the head plus a full 0x40 bytes for the tail
			list.emplace_back(headAt, kAlign + head, stageBuf);

			WriteParam<uint32be>(middle);
			WriteParam<uint8be>(0);
			WriteParam<uint8be>(0);
			WriteParam<uint8be>((uint8)head);
			WriteParam<uint8be>((uint8)tail);
		}

		IPCServiceClient& m_client;
		boost::container::static_vector<Vec, 8> m_in;
		boost::container::static_vector<Vec, 8> m_out;
		boost::container::static_vector<CopyBack, 16> m_copyBack;
		uint32 m_writePos{ 0 };
		uint32 m_readPos{ 0 };
	};

	IPCServiceClient() = default;

	~IPCServiceClient()
	{
		Shutdown();
	}

	// buffer: guest memory carved into 256-byte IPC buffers; opens the device
	void Initialize(std::string_view devicePath, uint8_t* buffer, uint32_t bufferSize)
	{
		m_devicePath = devicePath;
		m_pool.clear();
		for (uint32 off = 0; off + sizeof(IPCBuffer) <= bufferSize; off += (uint32)sizeof(IPCBuffer))
			m_pool.push_back(reinterpret_cast<IPCBuffer*>(buffer + off));
		m_handle = coreinit::IOS_Open(m_devicePath.c_str(), 0);
	}

	void Shutdown()
	{
		if (m_handle == 0)
			return;
		coreinit::IOS_Close(m_handle);
		m_handle = 0;
	}

	IPCServiceCall Begin(uint32_t serviceId, uint32_t commandId)
	{
		return IPCServiceCall(*this, serviceId, commandId);
	}

	IOSDevHandle GetDevHandle() const
	{
		cemu_assert(m_handle != 0);
		return m_handle;
	}

private:
	IPCBuffer* TakeBuffer()
	{
		cemu_assert(!m_pool.empty());
		IPCBuffer* b = m_pool.back();
		m_pool.pop_back();
		return b;
	}

	void GiveBackBuffer(IPCBuffer* b)
	{
		m_pool.push_back(b);
	}

	std::string m_devicePath;
	IOSDevHandle m_handle{ 0 };
	std::vector<IPCBuffer*> m_pool;
};
