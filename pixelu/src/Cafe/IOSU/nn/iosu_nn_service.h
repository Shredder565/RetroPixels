// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "Cafe/IOSU/kernel/iosu_kernel.h"
#include "Cafe/IOSU/iosu_ipc_common.h"
#include "Cafe/IOSU/iosu_types_common.h"
#include "Cafe/OS/libs/nn_common.h"

namespace iosu
{
	namespace nn
	{
		// Base for devices that take plain ioctlv requests (for example /dev/fpd).
		// Handles the service thread, per client objects and an optional periodic timer.
		// Vector order for ServiceCall: inputs first, then outputs.
		class IPCSimpleService
		{
		  public:
			IPCSimpleService(std::string_view devicePath) : m_devicePath(devicePath) {};
			virtual ~IPCSimpleService() {};

			virtual void StartService() {};
			virtual void StopService() {};

			virtual std::string GetThreadName() = 0;

			virtual void* CreateClientObject() = 0; // nullptr rejects the open
			virtual void DestroyClientObject(void* clientObject) = 0;
			virtual uint32 ServiceCall(void* clientObject, uint32 requestId, IPCIoctlVector* vecIn, uint32 numVecIn, IPCIoctlVector* vecOut, uint32 numVecOut) = 0;
			virtual void TimerUpdate() {};

			// called from inside ServiceCall: the request is answered later with ServiceCallAsyncRespond
			IPCCommandBody* ServiceCallDelayCurrentResponse();
			static void ServiceCallAsyncRespond(IPCCommandBody* response, uint32 r);

			void Start();
			void Stop();

			// calls TimerUpdate() on the service thread every 'milliseconds'
			void SetTimerUpdate(uint32 milliseconds);

		  private:
			void ServiceThread();
			IOSDevHandle AllocateHandle();
			void* LookupClient(IOSDevHandle handle) const;

			std::string m_devicePath;
			std::thread m_serviceThread;
			std::atomic_bool m_requestStop{false};
			std::atomic_bool m_isRunning{false};
			std::atomic_bool m_threadInitialized{ false };
			std::unordered_map<IOSDevHandle, void*> m_clientObjects;
			IOSDevHandle m_nextHandle{1};
			IOSTimerId m_timerId{IOSInvalidTimerId};
			IPCCommandBody* m_activeCmd{nullptr};
			bool m_delayResponse{false};
			IOSMsgQueueId m_msgQueueId;
			SysAllocator<iosu::kernel::IOSMessage, 128> m_queueStorage;
		};

		// Base for nn style services (/dev/act, /dev/acp_main, /dev/boss, ...).
		// Each ioctlv carries a request header (service and command id) followed by
		// serialized parameters; the reply starts with the nn result code.
		// Vector order on the wire: outputs first, then inputs.
		class IPCService
		{
			struct IPCServiceRequestHeader
			{
				uint32be ukn00;
				uint32be serviceId;
				uint32be ukn08;
				uint32be commandId;
			};
			static_assert(sizeof(IPCServiceRequestHeader) == 0x10);

			struct IPCServiceResponseHeader
			{
				uint32be nnResultCode;
			};

		public:
			// sequential reader over one input buffer
			class IPCParameterStream
			{
			public:
				IPCParameterStream() = default;
				IPCParameterStream(void* data, uint32 size) : m_data((uint8_t*)data), m_size(size) {}

				// on overrun hasError is set and a value initialized T is returned
				template<typename T>
				T ReadParameter(bool& hasError)
				{
					if (m_size < sizeof(T) || m_readIndex > m_size - sizeof(T))
					{
						hasError = true;
						return T{};
					}
					hasError = false;
					T value;
					memcpy(&value, m_data + m_readIndex, sizeof(T));
					m_readIndex += sizeof(T);
					return value;
				}

				uint8* GetData() { return m_data; }
				uint32 GetSize() const { return m_size; }

			private:
				uint8* m_data{nullptr};
				uint32 m_size{0};
				uint32 m_readIndex{0};
			};

			// sequential writer over one output buffer; writes that do not fit are dropped
			class IPCResponseStream
			{
			public:
				IPCResponseStream() = default;
				IPCResponseStream(void* data, uint32 size) : m_data((uint8*)data), m_size(size) {}

				template<typename T>
				void Write(const T& value)
				{
					if (m_size < sizeof(T) || m_writtenSize > m_size - sizeof(T))
					{
						m_hasError = true;
						return;
					}
					memcpy(m_data + m_writtenSize, &value, sizeof(T));
					m_writtenSize += sizeof(T);
				}

				uint8* GetData() { return m_data; }
				uint32 GetSize() const { return m_size; }

			private:
				uint8* m_data{nullptr};
				uint32 m_writtenSize{0};
				uint32 m_size{0};
				bool m_hasError{false};
			};

			class IPCServiceCall
			{
				// describes how a large buffer was split by the client:
				// an aligned middle part in its own vector, head and tail bytes packed in another
				struct LargeBufferHeader
				{
					uint32be alignedSize;
					uint8be ukn4;
					uint8be ukn5;
					uint8be headSize;
					uint8be tailSize;
				};
				static_assert(sizeof(LargeBufferHeader) == 8);
			public:
				// a client buffer reassembled from its head, aligned middle and tail parts
				struct UnalignedBuffer
				{
					UnalignedBuffer(bool isOutput, std::span<uint8> head, std::span<uint8> middle, std::span<uint8> tail)
						: m_parts{ head, middle, tail }, m_isOutput(isOutput) {}

					// concatenates the three parts into a T (input buffers only)
					template<typename T>
					T ReadType()
					{
						cemu_assert(!m_isOutput);
						T v;
						uint8_t* dst = (uint8_t*)&v;
						for (const std::span<uint8>& part : m_parts)
						{
							memcpy(dst, part.data(), part.size());
							dst += part.size();
						}
						return v;
					}

					// scatters 'size' bytes over head, middle and tail (output buffers only)
					void WriteData(void* data, size_t size)
					{
						if (size == 0)
							return;
						cemu_assert(m_isOutput);
						cemu_assert(GetSize() >= size);
						const uint8* src = (const uint8*)data;
						for (const std::span<uint8>& part : m_parts)
						{
							if (size == 0)
								break;
							const size_t n = std::min<size_t>(size, part.size());
							memcpy(part.data(), src, n);
							src += part.size();
							size -= n;
						}
					}

					size_t GetSize() const
					{
						return m_parts[0].size() + m_parts[1].size() + m_parts[2].size();
					}

				private:
					std::span<uint8> m_parts[3];
					bool m_isOutput;
				};

				IPCServiceCall(IOSDevHandle clientHandle, uint32 serviceId, uint32 commandId)
					: m_clientHandle(clientHandle), m_serviceId(serviceId), m_commandId(commandId) {}

				void AddInputStream(void* data, uint32 size)
				{
					cemu_assert(m_numInputs < kMaxStreams);
					m_inputs[m_numInputs++] = IPCParameterStream(data, size);
				}

				void AddOutputStream(void* data, uint32 size)
				{
					cemu_assert(m_numOutputs < kMaxStreams);
					m_outputs[m_numOutputs++] = IPCResponseStream(data, size);
				}

				uint32 GetServiceId() const { return m_serviceId; }
				uint32 GetCommandId() const { return m_commandId; }

				// parameters are always read from the first input stream
				template<typename T>
				T ReadParameter()
				{
					return m_inputs[0].ReadParameter<T>(m_hasError);
				}

				// buffer descriptors consume two further streams each
				UnalignedBuffer ReadUnalignedInputBufferInfo()
				{
					const LargeBufferHeader info = ReadParameter<LargeBufferHeader>();
					IPCParameterStream& middle = m_inputs[m_nextInput];
					IPCParameterStream& edges = m_inputs[m_nextInput + 1];
					m_nextInput += 2;
					return SplitBuffer(false, info, middle.GetData(), middle.GetSize(), edges.GetData(), edges.GetSize());
				}

				UnalignedBuffer ReadUnalignedOutputBufferInfo()
				{
					const LargeBufferHeader info = ReadParameter<LargeBufferHeader>();
					IPCResponseStream& middle = m_outputs[m_nextOutput];
					IPCResponseStream& edges = m_outputs[m_nextOutput + 1];
					m_nextOutput += 2;
					return SplitBuffer(true, info, middle.GetData(), middle.GetSize(), edges.GetData(), edges.GetSize());
				}

				// responses always go to the first output stream
				template<typename T>
				void WriteResponse(const T& value)
				{
					m_outputs[0].Write<T>(value);
				}

			private:
				static UnalignedBuffer SplitBuffer(bool isOutput, const LargeBufferHeader& info, uint8* middle, uint32 middleSize, uint8* edges, uint32 edgesSize)
				{
					const uint32 alignedSize = info.alignedSize;
					const uint8 headSize = info.headSize;
					const uint8 tailSize = info.tailSize;
					cemu_assert(middleSize == alignedSize);
					cemu_assert((uint32)headSize + tailSize <= edgesSize);
					return UnalignedBuffer(isOutput, { edges, headSize }, { middle, alignedSize }, { edges + headSize, tailSize });
				}

				static constexpr size_t kMaxStreams = 4;
				IOSDevHandle m_clientHandle;
				uint32 m_serviceId;
				uint32 m_commandId;
				IPCParameterStream m_inputs[kMaxStreams]{};
				size_t m_numInputs{0};
				IPCResponseStream m_outputs[kMaxStreams]{};
				size_t m_numOutputs{0};
				bool m_hasError{false};
				sint8 m_nextInput{1};
				sint8 m_nextOutput{1};
			};

			IPCService(std::string_view devicePath) : m_devicePath(devicePath) {};
			virtual ~IPCService() {};

			virtual IOSDevHandle CreateClientHandle()
			{
				return (IOSDevHandle)0;
			}

			virtual void CloseClientHandle(IOSDevHandle handle)
			{
			}

			virtual nnResult ServiceCall(IPCServiceCall& serviceCall)
			{
				cemu_assert_unimplemented();
				return 0;
			}

			void Start();
			void Stop();

		private:
			void ServiceThread();
			void HandleServiceRequest(IPCCommandBody* cmd);

			std::string m_devicePath;
			std::thread m_serviceThread;
			std::atomic_bool m_requestStop{false};
			std::atomic_bool m_isRunning{false};
			std::atomic_bool m_threadInitialized{ false };
			IOSMsgQueueId m_msgQueueId;
			SysAllocator<iosu::kernel::IOSMessage, 128> m_queueStorage;
		};
	};
};
