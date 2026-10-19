// Original code for RetroPixels (PixelU). Not derived from Cemu.
// h264.rpl: the console's H.264 decoder library.
//
// The guest owns a work memory block that holds our context (H264Context). Each opened context gets
// a decoder session (see H264DecInternal.h) identified by a handle stored in the context.
// H264DECExecute takes one access unit; in buffered mode up to five pictures stay in flight so the
// host decoder can work ahead, otherwise every call returns its own frame.
#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/HW/Espresso/PPCCallback.h"
#include "Cafe/OS/libs/h264_avc/parser/H264Parser.h"
#include "Cafe/OS/libs/h264_avc/H264DecInternal.h"
#include "util/highresolutiontimer/HighResolutionTimer.h"
#include "Cafe/CafeSystem.h"

#include "h264dec.h"

enum class H264DEC_STATUS : uint32
{
	SUCCESS = 0x0,
	BAD_STREAM = 0x1000000,
	INVALID_PARAM = 0x1010000,
};

namespace H264
{
	// Breath of the Wild (all regions) gets two adjustments that the Cemuhook plugin used to provide:
	// a tiny work memory requirement and 1920x1088 pictures reported as 1920x1080. Together they let
	// the game play 1080p replacement videos
	bool H264_IsBotW()
	{
		switch (CafeSystem::GetForegroundTitleId())
		{
		case 0x00050000101c9300:
		case 0x00050000101c9400:
		case 0x00050000101c9500:
			return true;
		default:
			return false;
		}
	}

	// lives in guest work memory
	struct H264Context
	{
		struct
		{
			MEMPTR<void> ptr{ nullptr };
			uint32be length{ 0 };
			float64be timestamp;
		}BitStream;
		struct
		{
			MEMPTR<void> outputFunc{ nullptr };
			uint8be outputPerFrame{ 0 }; // 0 = buffered output
			MEMPTR<void> userMemoryParam{ nullptr };
		}Param;
		uint32be sessionHandle;
		struct
		{
			uint32 numFramesInFlight{0};
			bool isFirstBegin{true};
			bool isTryingToRecover{false}; // set by every Begin after the first, cleared at the next IDR picture
		}decoderState;
	};

	/* stream inspection */

	// work memory needed per level_idc
	H264DEC_STATUS H264DECMemoryRequirement(uint32 codecProfile, uint32 codecLevel, uint32 width, uint32 height, uint32be* sizeRequirementOut)
	{
		if (H264_IsBotW())
		{
			static_assert(sizeof(H264Context) < 256);
			*sizeRequirementOut = 256;
			return H264DEC_STATUS::SUCCESS;
		}
		// the console seems to require at least 64x64 but Pikmin 3 asks for 32x32 and needs this to succeed
		const bool knownProfile = codecProfile == 66 || codecProfile == 77 || codecProfile == 100;
		if (!sizeRequirementOut || !knownProfile || codecLevel >= 52 || width < 32 || height < 32 || width > 2800 || height > 1408)
			return H264DEC_STATUS::INVALID_PARAM;

		struct LevelSize
		{
			uint32 lastLevel;
			uint32 size;
		};
		static constexpr LevelSize kSizes[] =
		{
			{ 0x0A, 0x18C << 10 },
			{ 0x0B, 0x384 << 10 },
			{ 0x14, 0x948 << 10 },
			{ 0x15, 0x1290 << 10 },
			{ 0x1E, 0x1FA4 << 10 },
			{ 0x1F, 0x4650 << 10 },
			{ 0x20, 0x1400000 },
			{ 0x29, 0x8000 << 10 },
			{ 0x2A, 0x2200000 },
			{ 0x32, 0x1AF40 << 10 },
			{ 0xFFFFFFFF, 0x2D000 << 10 },
		};
		uint32 size = 0;
		for (const LevelSize& e : kSizes)
		{
			if (codecLevel <= e.lastLevel)
			{
				size = e.size;
				break;
			}
		}
		*sizeRequirementOut = size + 0x447;
		return H264DEC_STATUS::SUCCESS;
	}

	uint32 H264DECCheckMemSegmentation(MPTR memory, uint32 size)
	{
		return 0; // every range is accepted (1 would mean invalid)
	}

	// Looks for a 00 00 01 start code whose NAL header (with nal_ref_idc masked out) is one of
	// 'types'. The reported offset is one byte before the start code, as on the console
	static H264DEC_STATUS FindNalStart(const uint8* data, uint32 length, uint32be* offsetOut, std::initializer_list<uint8> types)
	{
		if (!data || length < 4 || !offsetOut)
			return H264DEC_STATUS::INVALID_PARAM;
		for (uint32 i = 0; i < length - 4; i++) // start codes beginning in the last four bytes are not reported
		{
			if (data[i] != 0 || data[i + 1] != 0 || data[i + 2] != 1)
				continue;
			const uint8 header = data[i + 3] & 0x9F;
			if (std::find(types.begin(), types.end(), header) == types.end())
				continue;
			*offsetOut = i == 0 ? 0 : i - 1;
			return H264DEC_STATUS::SUCCESS;
		}
		return H264DEC_STATUS::BAD_STREAM;
	}

	H264DEC_STATUS H264DECFindDecstartpoint(uint8* ptr, uint32 length, uint32be* offsetOut)
	{
		return FindNalStart(ptr, length, offsetOut, { 7 }); // SPS
	}

	H264DEC_STATUS H264DECFindIdrpoint(uint8* ptr, uint32 length, uint32be* offsetOut)
	{
		return FindNalStart(ptr, length, offsetOut, { 5, 7, 8 }); // IDR slice, SPS or PPS
	}

	// the SPS fields H264DECGetImageSize needs
	struct SpsDimensions
	{
		uint32 widthInPixels{};
		uint32 heightInMapUnitsTimes16{};
		uint8 frameMbsOnly{};
	};

	static void SkipScalingList(RBSPInputBitstream& r, sint32 count)
	{
		sint32 last = 8;
		sint32 next = 8;
		for (sint32 i = 0; i < count; i++)
		{
			if (next != 0)
				next = (last + r.readSV_E() + 256) % 256;
			if (next != 0)
				last = next;
		}
	}

	// Reads an SPS up to the picture size, rejecting what the console decoder does not support
	// (profiles other than baseline/main/high, chroma formats other than 4:2:0, out of range fields)
	static bool ReadSpsDimensions(RBSPInputBitstream& r, SpsDimensions& out)
	{
		out = {};
		const uint32 profile = r.readBits(8);
		if (profile != 66 && profile != 77 && profile != 100)
			return false;
		r.readBits(16); // constraint flags, level_idc
		if (r.readUV_E() >= 32) // seq_parameter_set_id
			return false;
		if (profile == 100)
		{
			if (r.readUV_E() > 1) // chroma_format_idc
				return false;
			r.readUV_E(); // bit_depth_luma_minus8
			r.readUV_E(); // bit_depth_chroma_minus8
			r.readBit(); // qpprime_y_zero_transform_bypass_flag
			if (r.readBit()) // seq_scaling_matrix_present_flag
			{
				for (sint32 i = 0; i < 8; i++)
				{
					if (r.readBit())
						SkipScalingList(r, i < 6 ? 16 : 64);
				}
			}
		}
		if (r.readUV_E() > 12) // log2_max_frame_num_minus4
			return false;
		const uint32 pocType = r.readUV_E();
		if (pocType > 2)
			return false;
		if (pocType == 0 && r.readUV_E() > 12) // log2_max_pic_order_cnt_lsb_minus4
			return false;
		if (pocType == 1)
		{
			r.readBit(); // delta_pic_order_always_zero_flag
			r.readSV_E(); // offset_for_non_ref_pic
			r.readSV_E(); // offset_for_top_to_bottom_field
			const uint32 cycleLength = r.readUV_E();
			if (cycleLength > 255)
				return false;
			for (uint32 i = 0; i < cycleLength; i++)
				r.readSV_E();
		}
		if (r.readUV_E() > 16) // max_num_ref_frames
			return false;
		r.readBit(); // gaps_in_frame_num_value_allowed_flag
		const uint32 width = 16 * (r.readUV_E() + 1);
		if ((width & 0xFFF0) < 0x10)
			return false;
		const uint32 height = 16 * (r.readUV_E() + 1);
		if ((height & 0xFFF0) < 0x10)
			return false;
		out.widthInPixels = width;
		out.heightInMapUnitsTimes16 = height;
		out.frameMbsOnly = r.readBit();
		if (!out.frameMbsOnly)
			r.readBit(); // mb_adaptive_frame_field_flag
		r.readBit(); // direct_8x8_inference_flag
		if (r.readBit()) // frame_cropping_flag
		{
			for (int i = 0; i < 4; i++)
				r.readUV_E();
		}
		r.readBit(); // vui_parameters_present_flag (VUI is not needed)
		return true;
	}

	H264DEC_STATUS H264DECGetImageSize(uint8* stream, sint32 streamSize, sint32 offset, uint32be* outputWidth, uint32be* outputHeight)
	{
		if (!stream || !outputWidth || !outputHeight || streamSize < 4 || offset < 0 || offset >= streamSize || offset + 4 >= streamSize)
			return H264DEC_STATUS::INVALID_PARAM;
		const uint8* end = stream + streamSize;
		// The start code test looks two bytes behind the 01 byte, which for offset < 2 reads in front
		// of the buffer. The console library does the same
		for (const uint8* p = stream + offset; p < end - 2; p++)
		{
			if (p[0] != 1 || p[-1] != 0 || p[-2] != 0 || (p[1] & 0x1F) != 7)
				continue;
			uint8* sps = (uint8*)p + 2;
			RBSPInputBitstream reader(sps, (uint32)(end - sps), false);
			SpsDimensions dim;
			if (!ReadSpsDimensions(reader, dim))
			{
				cemuLog_log(LogType::Force, "H264DECGetImageSize: Invalid SPS data");
				cemu_assert_suspicious();
				return H264DEC_STATUS::BAD_STREAM;
			}
			*outputWidth = dim.widthInPixels;
			*outputHeight = dim.frameMbsOnly ? dim.heightInMapUnitsTimes16 : dim.heightInMapUnitsTimes16 * 2;
			if (*outputWidth == 0 || *outputHeight == 0)
				return H264DEC_STATUS::BAD_STREAM;
			if (H264_IsBotW() && *outputWidth == 1920 && *outputHeight == 1088)
				*outputHeight = 1080;
			return H264DEC_STATUS::SUCCESS;
		}
		return H264DEC_STATUS::BAD_STREAM;
	}

	// Size of the next decode unit at data+offset: everything up to the end of the first picture
	// slice (NAL type 1 or 5), counted from data+offset. Pictures are assumed to have one slice
	H264DEC_STATUS H264DECCheckDecunitLength(void* workMemory, uint8* data, uint32 maxLength, uint32 offset, uint32be* unitLengthOut)
	{
		if (offset >= maxLength || maxLength < 4)
			return H264DEC_STATUS::INVALID_PARAM;
		uint8* unit = data + offset;
		const uint32 available = maxLength - offset;
		NALInputBitstream nal(unit, available);
		if (nal.hasError())
		{
			cemu_assert_debug(false);
			return H264DEC_STATUS::BAD_STREAM;
		}
		// a unit without any start code is rejected
		sint32 startCode = -1;
		for (sint32 i = 0; i < (sint32)(available - 3); i++)
		{
			if (unit[i] == 0 && unit[i + 1] == 0 && unit[i + 2] == 1)
			{
				startCode = i;
				break;
			}
		}
		if (startCode < 0)
			return H264DEC_STATUS::BAD_STREAM;
		const uint8* unitStart = unit + startCode;
		RBSPInputBitstream r;
		while (!nal.isEndOfStream() && nal.getNextRBSP(r, true))
		{
			const uint8 header = r.readU8();
			if (header & 0x80)
			{
				cemu_assert_debug(false); // forbidden_zero_bit
				continue;
			}
			const uint8 type = header & 0x1F;
			switch (type)
			{
			case 1: // non-IDR slice
			case 5: // IDR slice
				*unitLengthOut = (uint32)(sint32)((r.getBasePtr() + r.getBaseLength()) - unitStart) + startCode;
				return H264DEC_STATUS::SUCCESS;
			case 6: // SEI
			case 7: // SPS
			case 8: // PPS
			case 9: // access unit delimiter
			case 10: // end of sequence
				break;
			case 14:
			case 20:
			case 21:
				cemu_assert_debug(false); // SVC/MVC
				break;
			default:
				cemuLog_logDebug(LogType::Force, "H264: NAL unit type {} skipped", type);
				cemu_assert_unimplemented();
				break;
			}
		}
		return H264DEC_STATUS::BAD_STREAM;
	}

	/* decoder sessions */

	class SessionTable
	{
	public:
		uint32 Create()
		{
			std::lock_guard lock(m_mutex);
			const uint32 handle = m_nextHandle.fetch_add(1);
			m_sessions.try_emplace(handle, CreateAVCDecoder());
			return handle;
		}

		H264DecoderBackend* Get(uint32 handle)
		{
			std::lock_guard lock(m_mutex);
			auto it = m_sessions.find(handle);
			if (it == m_sessions.end())
				return nullptr;
			if (m_sessions.size() >= 5)
			{
				cemuLog_log(LogType::Force, "H264: Warning - more than 5 active sessions");
				cemu_assert_suspicious();
			}
			return it->second;
		}

		void Destroy(uint32 handle)
		{
			std::lock_guard lock(m_mutex);
			auto it = m_sessions.find(handle);
			if (it == m_sessions.end())
				return;
			H264DecoderBackend* session = it->second;
			session->Destroy();
			delete session;
			m_sessions.erase(it);
		}

	private:
		std::mutex m_mutex;
		std::unordered_map<uint32, H264DecoderBackend*> m_sessions;
		std::atomic_uint32_t m_nextHandle{ 1 };
	};

	static SessionTable s_sessions;

	static H264DecoderBackend* SessionOf(void* workMemory, const char* caller)
	{
		H264DecoderBackend* session = s_sessions.Get(((H264Context*)workMemory)->sessionHandle);
		if (!session)
			cemuLog_log(LogType::Force, "{}(): Invalid session", caller);
		return session;
	}

	/* frame output */

	struct H264DECFrameOutput
	{
		/* +0x00 */ uint32be result;
		/* +0x04 */ uint32be padding04;
		/* +0x08 */ betype<double> timestamp;
		/* +0x10 */ uint32be frameWidth;
		/* +0x14 */ uint32be frameHeight;
		/* +0x18 */ uint32be bytesPerRow;
		/* +0x1C */ uint32be cropEnable;
		/* +0x20 */ uint32be cropTop;
		/* +0x24 */ uint32be cropBottom;
		/* +0x28 */ uint32be cropLeft;
		/* +0x2C */ uint32be cropRight;
		/* +0x30 */ uint32be ukn30;
		/* +0x34 */ uint32be ukn34;
		/* +0x38 */ uint32be ukn38;
		/* +0x3C */ uint32be ukn3C;
		/* +0x40 */ uint32be ukn40;
		/* +0x44 */ MEMPTR<uint8> imagePtr;
		/* +0x48 */ uint32 vuiEnable;
		/* +0x4C */ MPTR vuiPtr;
		/* +0x50 */ sint32 unused[10];
	};
	static_assert(sizeof(H264DECFrameOutput) == 0x78);

	// argument of the guest output callback
	struct H264OutputCBStruct
	{
		uint32be frameCount;
		MEMPTR<MEMPTR<H264DECFrameOutput>> resultArray;
		uint32be userParam;
	};
	static_assert(sizeof(H264OutputCBStruct) == 12);

	// passes one frame to the guest callback (if one is set). Space for 8 frames is reserved on the
	// guest stack, but frames are always reported one at a time
	static void OutputFrame(H264Context* ctx, H264DecoderBackend::DecodeResult& frame)
	{
		constexpr sint32 kFrameCount = 1;
		StackAllocator<MEMPTR<void>, 8> resultPtrs;
		StackAllocator<H264DECFrameOutput, 8> results;
		for (sint32 i = 0; i < kFrameCount; i++)
			resultPtrs[i] = &results + i;

		H264DECFrameOutput* out = &results + 0;
		memset(out, 0, sizeof(H264DECFrameOutput));
		out->result = 100;
		out->timestamp = frame.timestamp;
		out->frameWidth = frame.frameWidth;
		out->frameHeight = frame.frameHeight;
		out->bytesPerRow = frame.bytesPerRow;
		out->cropEnable = frame.cropEnable;
		out->cropTop = frame.cropTop;
		out->cropBottom = frame.cropBottom;
		out->cropLeft = frame.cropLeft;
		out->cropRight = frame.cropRight;
		out->imagePtr = (uint8*)frame.imageOutput;

		StackAllocator<H264OutputCBStruct> cbArg;
		cbArg->frameCount = kFrameCount;
		cbArg->resultArray = (MEMPTR<H264DECFrameOutput>*)resultPtrs.GetPointer();
		cbArg->userParam = ctx->Param.userMemoryParam.GetBEValue();

		if (ctx->Param.outputFunc.IsNull())
			return;
		cemuLog_log(LogType::H264, "H264: Outputting frame via callback. Timestamp: {} Buffer 0x{:08x} UserParam 0x{:08x}", (double)frame.timestamp, (uint32)out->imagePtr.GetMPTR(), ctx->Param.userMemoryParam.GetMPTR());
		PPCCoreCallback(ctx->Param.outputFunc.GetMPTR(), cbArg.GetMPTR());
	}

	// true when the first picture slice in the data is an IDR slice
	static bool StartsWithIdrPicture(uint8* data, sint32 length)
	{
		NALInputBitstream nal(data, length);
		RBSPInputBitstream r;
		while (nal.getNextRBSP(r))
		{
			const uint8 header = r.readU8();
			if (header & 0x80)
			{
				cemu_assert_suspicious();
				return false;
			}
			const uint8 type = header & 0x1F;
			if (type == 5)
				return true;
			if (type == 1)
				return false;
		}
		return false;
	}

	/* session control */

	uint32 H264DECInitParam(uint32 workMemorySize, void* workMemory)
	{
		H264Context* ctx = (H264Context*)workMemory;
		*ctx = {};
		return 0;
	}

	uint32 H264DECOpen(void* workMemory)
	{
		((H264Context*)workMemory)->sessionHandle = s_sessions.Create();
		return 0;
	}

	uint32 H264DECClose(void* workMemory)
	{
		if (workMemory)
			s_sessions.Destroy(((H264Context*)workMemory)->sessionHandle);
		return 0;
	}

	uint32 H264DECBegin(void* workMemory)
	{
		H264Context* ctx = (H264Context*)workMemory;
		H264DecoderBackend* session = SessionOf(workMemory, "H264DECBegin");
		if (!session)
			return 0;
		if (!ctx->decoderState.isFirstBegin)
		{
			// playback restarts, possibly somewhere else in the stream: wait for the next IDR picture
			ctx->decoderState.isTryingToRecover = true;
			return 0;
		}
		session->Init(ctx->Param.outputPerFrame == 0);
		ctx->decoderState.isFirstBegin = false;
		return 0;
	}

	// outputs all remaining frames. The session stays usable for a following H264DECBegin
	H264DEC_STATUS H264DECEnd(void* workMemory)
	{
		H264Context* ctx = (H264Context*)workMemory;
		H264DecoderBackend* session = SessionOf(workMemory, "H264DECEnd");
		if (!session)
			return H264DEC_STATUS::SUCCESS;
		coreinit::OSEvent* flushDone = &session->GetFlushEvent();
		coreinit::OSResetEvent(flushDone);
		session->QueueFlush();
		coreinit::OSWaitEvent(flushDone);
		H264DecoderBackend::DecodeResult frame;
		while (session->GetFrameOutputIfReady(frame))
		{
			OutputFrame(ctx, frame);
			ctx->decoderState.numFramesInFlight--;
			frame = {};
		}
		cemu_assert_debug(ctx->decoderState.numFramesInFlight == 0);
		return H264DEC_STATUS::SUCCESS;
	}

	/* parameters */

	H264DEC_STATUS H264DECSetParam_FPTR_OUTPUT(H264Context* ctx, void* outputFunc)
	{
		ctx->Param.outputFunc = outputFunc;
		return H264DEC_STATUS::SUCCESS;
	}

	H264DEC_STATUS H264DECSetParam_OUTPUT_PER_FRAME(H264Context* ctx, uint32 outputPerFrame)
	{
		ctx->Param.outputPerFrame = outputPerFrame != 0 ? 1 : 0;
		return H264DEC_STATUS::SUCCESS;
	}

	H264DEC_STATUS H264DECSetParam_USER_MEMORY(H264Context* ctx, MEMPTR<void*>* userMemoryParamPtr)
	{
		ctx->Param.userMemoryParam = *userMemoryParamPtr;
		return H264DEC_STATUS::SUCCESS;
	}

	H264DEC_STATUS H264DECSetParam(H264Context* ctx, uint32 paramId, void* paramValue)
	{
		enum : uint32
		{
			kParamOutputFunction = 0x1,
			kParamOutputPerFrame = 0x20000002,
			kParamUnknown30 = 0x20000030, // used by MK8, points to a bool; no effect
			kParamUserMemory = 0x70000001,
		};
		switch (paramId)
		{
		case kParamOutputFunction:
			ctx->Param.outputFunc = paramValue;
			break;
		case kParamUserMemory:
			ctx->Param.userMemoryParam = paramValue;
			break;
		case kParamOutputPerFrame:
			ctx->Param.outputPerFrame = *(uint8be*)paramValue != 0;
			break;
		case kParamUnknown30:
			break;
		default:
			cemuLog_log(LogType::Force, "h264Export_H264DECSetParam(): Unsupported parameterId 0x{:08x}\n", paramId);
			cemu_assert_unimplemented();
			break;
		}
		return H264DEC_STATUS::SUCCESS;
	}

	/* decoding */

	uint32 H264DECSetBitstream(void* workMemory, void* ptr, uint32 length, double timestamp)
	{
		H264Context* ctx = (H264Context*)workMemory;
		ctx->BitStream.ptr = ptr;
		ctx->BitStream.length = length;
		ctx->BitStream.timestamp = timestamp;
		return 0;
	}

	uint32 H264DECExecute(void* workMemory, void* imageOutput)
	{
		constexpr uint32 kResultFrameOutput = 0x80 | 100;
		constexpr uint32 kResultNotRecovered = 0x400; // exact console code unknown
		cemuLog_log(LogType::H264, "H264DECExecute(): [BEGIN]");
		BenchmarkTimer timer;
		timer.Start();
		H264Context* ctx = (H264Context*)workMemory;
		H264DecoderBackend* session = SessionOf(workMemory, "H264DECExecute");
		if (!session)
			return 0;
		uint8* data = (uint8*)ctx->BitStream.ptr.GetPtr();
		const uint32 length = ctx->BitStream.length;
		if (ctx->decoderState.isTryingToRecover)
		{
			if (!StartsWithIdrPicture(data, length))
				return kResultNotRecovered;
			ctx->decoderState.isTryingToRecover = false;
		}
		session->QueueForDecode(data, length, ctx->BitStream.timestamp, imageOutput);
		ctx->decoderState.numFramesInFlight++;
		// the console decodes synchronously; buffered mode lets the host decoder run five pictures ahead
		const uint32 maxInFlight = ctx->Param.outputPerFrame == 0 ? 5 : 0;
		if (ctx->decoderState.numFramesInFlight > maxInFlight)
		{
			ctx->decoderState.numFramesInFlight--;
			H264DecoderBackend::DecodeResult frame;
			do
			{
				coreinit::OSWaitEvent(&session->GetFrameOutputEvent());
			} while (!session->GetFrameOutputIfReady(frame));
			OutputFrame(ctx, frame);
		}
		timer.Stop();
		cemuLog_log(LogType::H264, "H264Bench | H264DECExecute took {}ms", timer.GetElapsedMilliseconds());
		return kResultFrameOutput;
	}

	class : public COSModule
	{
	public:
		std::string_view GetName() override
		{
			return "h264";
		}

		void RPLMapped() override
		{
			cafeExportRegister("h264", H264DECCheckMemSegmentation, LogType::H264);
			cafeExportRegister("h264", H264DECMemoryRequirement, LogType::H264);
			cafeExportRegister("h264", H264DECFindDecstartpoint, LogType::H264);
			cafeExportRegister("h264", H264DECFindIdrpoint, LogType::H264);
			cafeExportRegister("h264", H264DECGetImageSize, LogType::H264);

			cafeExportRegister("h264", H264DECInitParam, LogType::H264);
			cafeExportRegister("h264", H264DECOpen, LogType::H264);
			cafeExportRegister("h264", H264DECClose, LogType::H264);
			cafeExportRegister("h264", H264DECBegin, LogType::H264);
			cafeExportRegister("h264", H264DECEnd, LogType::H264);

			cafeExportRegister("h264", H264DECSetParam_FPTR_OUTPUT, LogType::H264);
			cafeExportRegister("h264", H264DECSetParam_OUTPUT_PER_FRAME, LogType::H264);
			cafeExportRegister("h264", H264DECSetParam_USER_MEMORY, LogType::H264);
			cafeExportRegister("h264", H264DECSetParam, LogType::H264);

			cafeExportRegister("h264", H264DECSetBitstream, LogType::H264);
			cafeExportRegister("h264", H264DECExecute, LogType::H264);

			cafeExportRegister("h264", H264DECCheckDecunitLength, LogType::H264);
		};
	}s_h264Module;

	COSModule* GetModule()
	{
		return &s_h264Module;
	}
}
